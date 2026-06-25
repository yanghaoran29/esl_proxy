/*
 * Host-side swimlane collector framework (BufferPoolManager + ProfilerBase).
 */
#ifndef ESL_PROXY_SWIMLANE_COLLECTOR_BASE_H
#define ESL_PROXY_SWIMLANE_COLLECTOR_BASE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "swimlane/memory_barrier.h"
#define ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
#include "onboard_config.h"
#undef ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
#include "onboard_log.h"

int profiling_copy_to_device(volatile void *dev_dst, const void *host_src, size_t size);
int profiling_copy_from_device(volatile void *host_dst, const volatile void *dev_src, size_t size);

inline int profiling_copy_to_device_for_ops(void *dev_dst, const void *host_src, size_t size) {
    return profiling_copy_to_device(dev_dst, host_src, size);
}
inline int profiling_copy_from_device_for_ops(void *host_dst, const void *dev_src, size_t size) {
    return profiling_copy_from_device(host_dst, dev_src, size);
}

std::function<int(void *, const void *, size_t)> profiling_copy_to_device_or_null();
std::function<int(void *, const void *, size_t)> profiling_copy_from_device_or_null();

namespace profiling_common {



/**
 * Thread factory for spawning the mgmt thread with optional device-context
 * binding. Pass `create_thread()` from device_runner to get a sim/onboard
 * device-bound worker; pass {} (default) to fall back to a bare std::thread.
 */
using ThreadFactory = std::function<std::thread(std::function<void()>)>;

/**
 * Type-erased memory-op callbacks used by BufferPoolManager.
 *
 * - alloc:            allocate `size` bytes of device memory; return nullptr
 *                     on failure.
 * - reg:              "register" dev_ptr for host visibility. On a5 this
 *                     allocates a paired host shadow (malloc + memset 0 +
 *                     copy_to_device of the zeros) and writes its address to
 *                     *host_ptr_out. ProfilerBase::start always installs a
 *                     non-null reg wrapper — collectors do not need to
 *                     branch.
 * - free_:            free a previously allocated device pointer.
 * - copy_to_device:   memcpy host → device. Used at init/teardown for the
 *                     bulk shm push, and used by the mgmt loop via
 *                     `write_range_to_device` to push narrow host-modified
 *                     fields back (advanced `queue_heads[q]`, refilled
 *                     `free_queue.tail` + `buffer_ptrs[slot]`) without
 *                     clobbering AICPU-owned fields.
 * - copy_from_device: memcpy device → host, used at the top of every mgmt
 *                     tick to mirror device-side `queue_tails` /
 *                     BufferState updates into the host shadow.
 */
struct MemoryOps {
    std::function<void *(size_t)> alloc;
    std::function<int(void *dev_ptr, size_t size, int device_id, void **host_ptr_out)> reg;
    std::function<int(void *dev_ptr)> free_;
    std::function<int(void *dev_dst, const void *host_src, size_t size)> copy_to_device;
    std::function<int(void *host_dst, const void *dev_src, size_t size)> copy_from_device;
};

/**
 * Per-buffer ownership info threaded through the done_queue so that the mgmt
 * thread, when it recycles a finished buffer, knows which per-kind pool it
 * came from.
 */
struct DoneInfo {
    void *dev_ptr;
    int kind;  // [0, Module::kBufferKinds)
};

template <typename Module>
class BufferPoolManager {
    // Static checks for the Module concept. Required type aliases trigger
    // clear "no type named X in Module" errors at instantiation if missing;
    // the explicit static_asserts cover constants and surface invariants.
    using _DataHeaderRequired = typename Module::DataHeader;
    using _ReadyEntryRequired = typename Module::ReadyEntry;
    using _ReadyBufferInfoRequired = typename Module::ReadyBufferInfo;
    static_assert(Module::kBufferKinds > 0, "Module::kBufferKinds must be > 0");

public:
    using ReadyBufferInfo = typename Module::ReadyBufferInfo;

    BufferPoolManager() :
        recycled_(Module::kBufferKinds) {}
    ~BufferPoolManager() = default;

    BufferPoolManager(const BufferPoolManager &) = delete;
    BufferPoolManager &operator=(const BufferPoolManager &) = delete;

    /**
     * Configure the buffer pool's memory context. Called by ProfilerBase::start()
     * before any allocator-touching method (alloc_and_register / free_buffer /
     * resolve_host_ptr / drain_done_into_recycled triggered by the mgmt loop)
     * is invoked. Must NOT be called concurrently with the mgmt thread.
     *
     * @param ops              Memory-op callbacks (alloc/reg/free/copy_*).
     * @param shared_mem_dev   Device base of the subsystem's shared memory.
     * @param shared_mem_host  Host shadow of the same region.
     * @param shm_size         Total bytes of the shared-memory region (used
     *                         by the mgmt loop's per-tick mirror).
     * @param device_id        Forwarded to ops.reg.
     */
    void
    set_memory_context(MemoryOps ops, void *shared_mem_dev, void *shared_mem_host, size_t shm_size, int device_id) {
        ops_ = std::move(ops);
        shared_mem_dev_ = shared_mem_dev;
        shared_mem_host_ = shared_mem_host;
        shm_size_ = shm_size;
        device_id_ = device_id;
    }

    /**
     * Release every device buffer the framework currently owns: recycled
     * pools, done_queue, and ready_queue. Buffers still in the per-pool
     * free_queue or held as current_buf_ptr are NOT touched — those belong
     * to the collector and must be released by it (the AICPU may still be
     * referencing them via shared memory until execution ends).
     *
     * For each unique device pointer freed, the paired host shadow is
     * `std::free`d ONLY if it lives in `malloc_shadows_` (i.e. the
     * framework itself malloc'd it via `default_host_shadow_register` or
     * `ProfilerBase::alloc_paired_buffer`'s copy-to-device branch). HAL
     * mappings (e.g. `halHostRegister` results) are never freed here.
     *
     * `release_fn(dev_ptr)` is invoked once per unique pointer; the
     * collector is expected to call its free_cb on the device pointer.
     *
     * Only safe to call after ProfilerBase::stop() has joined the mgmt thread.
     */
    template <typename ReleaseFn>
    void release_owned_buffers(const ReleaseFn &release_fn) {
        std::unordered_map<void *, bool> seen;
        auto release_once = [&](void *p) {
            if (p == nullptr) return;
            if (seen.emplace(p, true).second) {
                auto it = dev_to_host_.find(p);
                void *host_ptr = (it != dev_to_host_.end()) ? it->second : nullptr;
                release_fn(p);
                if (host_ptr != nullptr && malloc_shadows_.erase(host_ptr) > 0) {
                    std::free(host_ptr);
                }
                if (it != dev_to_host_.end()) {
                    dev_to_host_.erase(it);
                }
            }
        };

        for (auto &pool : recycled_) {
            for (void *p : pool)
                release_once(p);
            pool.clear();
        }
        {
            std::scoped_lock<std::mutex> lock(done_mutex_);
            while (!done_queue_.empty()) {
                release_once(done_queue_.front().dev_ptr);
                done_queue_.pop();
            }
        }
        {
            std::scoped_lock<std::mutex> lock(ready_mutex_);
            while (!ready_queue_.empty()) {
                release_once(ready_queue_.front().dev_buffer_ptr);
                ready_queue_.pop();
            }
        }
    }

    /**
     * Drop the dev↔host mapping table — call after the collector has freed
     * its share of buffers (free_queue + current_buf_ptr) and there are no
     * further resolve_host_ptr() lookups expected. `std::free`s any host
     * shadow still listed in `malloc_shadows_` (collectors may have invoked
     * free_cb on the dev pointer without going through release_owned_buffers).
     * HAL mappings are not touched.
     */
    void clear_mappings() {
        for (auto &kv : dev_to_host_) {
            if (kv.second != nullptr && malloc_shadows_.count(kv.second) > 0) {
                std::free(kv.second);
            }
        }
        dev_to_host_.clear();
        malloc_shadows_.clear();
    }

    /**
     * Abort-path cleanup: free EVERY framework-tracked device pointer (via
     * `release_fn`) and every framework-malloc'd host shadow, then clear all
     * containers. Distinct from `release_owned_buffers()` + `clear_mappings()`
     * because this also catches buffers parked in callers' SPSC free_queues
     * (which the framework tracked via `register_mapping` but does not own a
     * queue for). Intended for `init()` error paths where `finalize()` has
     * not run.
     *
     * Drains recycled/done/ready first (just discards — release goes via
     * dev_to_host_ to avoid double-free) and then iterates the full
     * dev→host map. Each unique dev_ptr is released exactly once.
     */
    template <typename ReleaseFn>
    void release_all_owned(const ReleaseFn &release_fn) {
        for (auto &pool : recycled_)
            pool.clear();
        {
            std::scoped_lock<std::mutex> lock(done_mutex_);
            std::queue<DoneInfo>().swap(done_queue_);
        }
        {
            std::scoped_lock<std::mutex> lock(ready_mutex_);
            std::queue<ReadyBufferInfo>().swap(ready_queue_);
        }
        for (auto &kv : dev_to_host_) {
            if (kv.first != nullptr) {
                release_fn(kv.first);
            }
            // erase-based check (matches release_owned_buffers): atomic
            // check-and-remove guards against a double-free if any duplicate
            // mapping ever sneaks into dev_to_host_.
            if (kv.second != nullptr && malloc_shadows_.erase(kv.second) > 0) {
                std::free(kv.second);
            }
        }
        dev_to_host_.clear();
        malloc_shadows_.clear();
    }

    // -------------------------------------------------------------------------
    // Per-tick mirror of the shared-memory region
    // -------------------------------------------------------------------------

    /**
     * Pull the entire device-side shared-memory region into the host shadow.
     * Called at the top of every mgmt tick so that subsequent reads of
     * `queue_tails`, `BufferState::current_buf_ptr`, etc. see fresh values.
     */
    int mirror_shm_from_device() {
        if (shared_mem_host_ == nullptr || shared_mem_dev_ == nullptr || shm_size_ == 0) {
            return 0;
        }
        if (!ops_.copy_from_device) return 0;
        return ops_.copy_from_device(shared_mem_host_, shared_mem_dev_, shm_size_);
    }

    /**
     * Push the host-side modifications (advanced `queue_heads`, refilled
     * free_queues) back to the device. Called at the bottom of every mgmt
     * tick.
     *
     * NOTE: deprecated for a5 — bulk write_back races with AICPU writes to
     * device-owned fields (BufferState::current_buf_ptr, total/dropped/mismatch
     * counters, queue_tails, free_queue.head, and on a5
     * L2SwimlaneAicpuPhaseHeader::magic, ...).
     * The bulk write rolls those updates back to whatever was in the host
     * shadow at mirror_from_device time. Keep the method around so callers
     * outside the mgmt loop (init/teardown) still have a way to push the
     * whole region, but the mgmt loop now uses `write_field_to_device` /
     * `write_range_to_device` for the few fields host actually modifies.
     */
    int mirror_shm_to_device() {
        if (shared_mem_host_ == nullptr || shared_mem_dev_ == nullptr || shm_size_ == 0) {
            return 0;
        }
        if (!ops_.copy_to_device) return 0;
        return ops_.copy_to_device(shared_mem_dev_, shared_mem_host_, shm_size_);
    }

    /**
     * Push a single field/range from host shadow to its mirrored device
     * location. `host_field_ptr` must lie inside the host shm shadow
     * (`[shared_mem_host_, shared_mem_host_ + shm_size_)`). Used by the
     * mgmt loop to avoid bulk writing the entire shm region, which would
     * clobber device-only counters and current_buf_ptr values written by
     * AICPU between the from/to mirror calls.
     *
     * Accepts `const volatile void*` so callers can pass the address of
     * volatile fields (queue_heads[], free_queue.tail, free_queue.buffer_ptrs[])
     * without an explicit cast at the call site.
     *
     * Returns the underlying ops result, or -1 on bounds violation.
     */
    int write_range_to_device(const volatile void *host_field_ptr, size_t size) {
        if (shared_mem_host_ == nullptr || shared_mem_dev_ == nullptr || shm_size_ == 0) {
            return 0;
        }
        if (!ops_.copy_to_device) return 0;
        const auto *host_base = static_cast<const char *>(shared_mem_host_);
        const auto *host_field = const_cast<const char *>(static_cast<const volatile char *>(host_field_ptr));
        if (host_field < host_base || host_field + size > host_base + shm_size_) {
            LOG_ERROR(
                "BufferPoolManager::write_range_to_device: field [%p, %p) outside shm [%p, %p)",
                static_cast<const void *>(host_field), static_cast<const void *>(host_field + size),
                static_cast<const void *>(host_base), static_cast<const void *>(host_base + shm_size_)
            );
            return -1;
        }
        size_t offset = static_cast<size_t>(host_field - host_base);
        void *dev_field = static_cast<char *>(shared_mem_dev_) + offset;
        return ops_.copy_to_device(dev_field, host_field, size);
    }

    /**
     * Re-pull a single field/range from device into the host shadow.
     * Symmetric counterpart of `write_range_to_device`. Used to refresh
     * a specific field after the per-tick `mirror_shm_from_device` to
     * defeat a torn-read race: the bulk mirror is not atomic w.r.t.
     * concurrent AICPU writes, so a producer-published entry (e.g. a
     * `queues[t][tail]` slot) may be observed half-written if it was
     * mirrored before AICPU finished writing it. Re-reading the entry
     * after observing `head < tail` gives the latest device-side bytes.
     *
     * Accepts `volatile void*` so callers can pass the address of volatile
     * fields without an explicit cast.
     *
     * Returns the underlying ops result, or -1 on bounds violation.
     */
    int read_range_from_device(volatile void *host_field_ptr, size_t size) {
        if (shared_mem_host_ == nullptr || shared_mem_dev_ == nullptr || shm_size_ == 0) {
            return 0;
        }
        if (!ops_.copy_from_device) return 0;
        const auto *host_base = static_cast<const char *>(shared_mem_host_);
        const auto *host_field = const_cast<const char *>(static_cast<volatile char *>(host_field_ptr));
        if (host_field < host_base || host_field + size > host_base + shm_size_) {
            LOG_ERROR(
                "BufferPoolManager::read_range_from_device: field [%p, %p) outside shm [%p, %p)",
                static_cast<const void *>(host_field), static_cast<const void *>(host_field + size),
                static_cast<const void *>(host_base), static_cast<const void *>(host_base + shm_size_)
            );
            return -1;
        }
        size_t offset = static_cast<size_t>(host_field - host_base);
        const void *dev_field = static_cast<const char *>(shared_mem_dev_) + offset;
        return ops_.copy_from_device(const_cast<void *>(static_cast<const void *>(host_field)), dev_field, size);
    }

    /**
     * Pull a single buffer's contents (e.g. an L2SwimlaneAicpuTaskBuffer / PmuBuffer /
     * DumpMetaBuffer) from device to its host shadow. Called by
     * ProfilerAlgorithms::process_entry after resolving the host pointer
     * for a popped ready entry, before delivering it to the collector.
     */
    int copy_buffer_from_device(void *host_dst, void *dev_src, size_t size) {
        if (!ops_.copy_from_device) return 0;
        return ops_.copy_from_device(host_dst, dev_src, size);
    }

    /**
     * Push a single buffer's contents from host shadow to device. Currently
     * unused by the mgmt loop (AICPU resets buffer state itself when it
     * pops from free_queue), but exposed for collector-side use cases.
     */
    int copy_buffer_to_device(void *dev_dst, const void *host_src, size_t size) {
        if (!ops_.copy_to_device) return 0;
        return ops_.copy_to_device(dev_dst, host_src, size);
    }

    // -------------------------------------------------------------------------
    // ready_queue: mgmt thread pushes, collector thread pops
    // -------------------------------------------------------------------------

    void push_to_ready(const ReadyBufferInfo &info) {
        {
            std::scoped_lock<std::mutex> lock(ready_mutex_);
            ready_queue_.push(info);
        }
        ready_cv_.notify_one();
    }

    bool try_pop_ready(ReadyBufferInfo &out) {
        std::scoped_lock<std::mutex> lock(ready_mutex_);
        if (ready_queue_.empty()) return false;
        out = ready_queue_.front();
        ready_queue_.pop();
        return true;
    }

    bool wait_pop_ready(ReadyBufferInfo &out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(ready_mutex_);
        if (!ready_cv_.wait_for(lock, timeout, [this] {
                return !ready_queue_.empty();
            })) {
            return false;
        }
        out = ready_queue_.front();
        ready_queue_.pop();
        return true;
    }

    // -------------------------------------------------------------------------
    // done_queue: collector thread reports buffers it has finished copying;
    // mgmt thread folds them back into the recycled pool of the right kind.
    // -------------------------------------------------------------------------

    void notify_copy_done(void *dev_ptr, int kind) {
        std::scoped_lock<std::mutex> lock(done_mutex_);
        done_queue_.push(DoneInfo{dev_ptr, kind});
    }

    // -------------------------------------------------------------------------
    // Helpers used from Module::process_entry / proactive_replenish
    // -------------------------------------------------------------------------

    /**
     * Allocate a new device buffer and pair it with a host shadow via
     * ops_.reg. Tracks the resulting dev→host mapping so resolve_host_ptr()
     * can find it on subsequent ready-queue pops.
     *
     * @param size              Byte size to allocate.
     * @param[out] host_ptr_out Host shadow pointer.
     * @return                  Device pointer, or nullptr on failure.
     */
    void *alloc_and_register(size_t size, void **host_ptr_out) {
        void *dev_ptr = ops_.alloc(size);
        if (dev_ptr == nullptr) {
            *host_ptr_out = nullptr;
            return nullptr;
        }
        void *host_ptr = nullptr;
        int rc = ops_.reg(dev_ptr, size, device_id_, &host_ptr);
        if (rc != 0 || host_ptr == nullptr) {
            LOG_ERROR("BufferPoolManager: register failed: %d", rc);
            // Best-effort dev free; no shadow was registered yet.
            if (ops_.free_) {
                ops_.free_(dev_ptr);
            }
            *host_ptr_out = nullptr;
            return nullptr;
        }
        *host_ptr_out = host_ptr;
        dev_to_host_[dev_ptr] = host_ptr;
        return dev_ptr;
    }

    /**
     * Free a device pointer + paired host shadow tracked in dev_to_host_.
     * Currently unused by the mgmt loop (recycle path keeps buffers alive)
     * but kept for symmetry with a2a3.
     */
    void free_buffer(void *dev_ptr) {
        if (dev_ptr == nullptr) return;
        auto it = dev_to_host_.find(dev_ptr);
        void *host_ptr = (it != dev_to_host_.end()) ? it->second : nullptr;
        if (it != dev_to_host_.end()) {
            dev_to_host_.erase(it);
        }
        if (ops_.free_) {
            ops_.free_(dev_ptr);
        }
        if (host_ptr != nullptr && malloc_shadows_.erase(host_ptr) > 0) {
            std::free(host_ptr);
        }
    }

    /**
     * Resolve a device pointer to the host-mapped pointer recorded at
     * alloc_and_register / register_mapping time.
     */
    void *resolve_host_ptr(void *dev_ptr) {
        auto it = dev_to_host_.find(dev_ptr);
        if (it != dev_to_host_.end()) return it->second;
        LOG_ERROR("BufferPoolManager: no host mapping for dev_ptr=%p", dev_ptr);
        return nullptr;
    }

    /**
     * Register an externally-allocated mapping. Used by the Collector during
     * initialize() when it pre-allocates buffers and wants the mgmt thread
     * to be able to resolve them later.
     */
    void register_mapping(void *dev_ptr, void *host_ptr) { dev_to_host_[dev_ptr] = host_ptr; }

    /**
     * Claim ownership of a host shadow that the framework malloc'd. Only
     * shadows tracked here are `std::free`d by `clear_mappings()`,
     * `release_owned_buffers()`, and `free_buffer()` — HAL-managed
     * mappings (e.g. `halHostRegister` results) must NOT be added here.
     */
    void add_malloc_shadow(void *host_ptr) {
        if (host_ptr != nullptr) {
            malloc_shadows_.insert(host_ptr);
        }
    }

    /**
     * Pull from the recycled pool of the given kind, or return nullptr if
     * empty. Caller is responsible for resolving host_ptr (via
     * resolve_host_ptr) before handing the buffer back to AICPU.
     */
    void *pop_recycled(int kind) {
        auto &pool = recycled_[kind];
        if (pool.empty()) return nullptr;
        void *p = pool.back();
        pool.pop_back();
        return p;
    }

    void push_recycled(int kind, void *dev_ptr) { recycled_[kind].push_back(dev_ptr); }

    bool recycled_empty() const {
        for (const auto &pool : recycled_) {
            if (!pool.empty()) return false;
        }
        return true;
    }

    /**
     * Drain everything currently in done_queue back into the per-kind
     * recycled pool. May be called from Module::process_entry when its
     * primary recycled pool ran out, to harvest buffers the collector freed
     * in the meantime.
     */
    void drain_done_into_recycled() {
        std::scoped_lock<std::mutex> lock(done_mutex_);
        while (!done_queue_.empty()) {
            const DoneInfo &info = done_queue_.front();
            recycled_[info.kind].push_back(info.dev_ptr);
            done_queue_.pop();
        }
    }

    void *shared_mem_dev() const { return shared_mem_dev_; }
    void *shared_mem_host() const { return shared_mem_host_; }
    int device_id() const { return device_id_; }

private:
    // Subsystem inputs (set by ProfilerBase::start via set_memory_context).
    void *shared_mem_dev_{nullptr};
    void *shared_mem_host_{nullptr};
    size_t shm_size_{0};
    int device_id_{-1};
    MemoryOps ops_;

    // mgmt → collector
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    std::queue<ReadyBufferInfo> ready_queue_;

    // collector → mgmt
    std::mutex done_mutex_;
    std::queue<DoneInfo> done_queue_;

    // dev → host mapping (single source of truth for resolve_host_ptr)
    std::unordered_map<void *, void *> dev_to_host_;

    // Host shadows the framework itself malloc'd (via
    // `default_host_shadow_register` or `ProfilerBase::alloc_paired_buffer`'s
    // copy-to-device branch). Only these are `std::free`d on teardown —
    // HAL-managed mappings (halHostRegister) live outside this set.
    std::unordered_set<void *> malloc_shadows_;

    // Per-kind recycled buffer pools (vector indexed by Module-defined kind id)
    std::vector<std::vector<void *>> recycled_;
};

// Common subsystem callback signatures. All four collectors (PMU / TensorDump
// / L2Swimlane / DepGen) used to declare their own typedefs with identical
// shapes; these are the canonical types stashed in ProfilerBase via
// set_memory_context().
//
// Alloc / free use std::function so callers can bind state (e.g. their
// MemoryAllocator) directly via lambda capture. Register / unregister stay
// as plain function pointers — they wrap stateless HAL globals (halHost*),
// so the captureless C-callback shape matches their actual nature.
using ProfAllocCallback = std::function<void *(size_t size)>;
using ProfRegisterCallback = int (*)(void *dev_ptr, size_t size, int device_id, void **host_ptr_out);
using ProfUnregisterCallback = int (*)(void *dev_ptr, int device_id);
using ProfFreeCallback = std::function<int(void *dev_ptr)>;

// `default_host_shadow_register` was previously a free function; it has been
// folded into a lambda inside `ProfilerBase::start()` so the shadow it
// malloc's can be registered with the manager's `malloc_shadows_` set for
// safe teardown via `clear_mappings()` / `release_owned_buffers()`. See
// `ProfilerBase::start()` for the inline definition.

/**
 * RAII scope guard for collector `init()` rollback. On destruction (without
 * `commit()`) it (1) calls `manager.release_all_owned(release_fn)` to free
 * every framework-tracked dev_ptr + host shadow, and (2) releases any extra
 * direct dev_ptrs the collector added via `add_direct_ptr()` (used for
 * pointers the collector owns outside the framework — e.g. PMU per-core
 * `PmuAicoreRing` allocations on a5).
 *
 * Pattern:
 *   int Collector::init(...) {
 *       ...
 *       set_memory_context(...);
 *       InitRollbackGuard<Manager> guard(manager_, free_cb);
 *       void *dev_ptr = alloc_paired_buffer(size, &host_ptr);
 *       if (dev_ptr == nullptr) return -1;       // guard runs, frees nothing yet
 *       ...
 *       void *direct = alloc_cb(...);
 *       guard.add_direct_ptr(direct);            // ensure it's freed on abort
 *       ...
 *       guard.commit();                          // success — disarm
 *       initialized_ = true;
 *       return 0;
 *   }
 */
template <typename Manager>
class InitRollbackGuard {
public:
    using ReleaseFn = std::function<int(void *)>;

    InitRollbackGuard(Manager &manager, ReleaseFn release_fn) :
        manager_(manager),
        release_fn_(std::move(release_fn)),
        committed_(false) {}

    ~InitRollbackGuard() {
        if (committed_) return;
        for (void *p : direct_ptrs_) {
            if (p != nullptr && release_fn_) {
                release_fn_(p);
            }
        }
        // Call release_all_owned unconditionally: it also frees malloc'd
        // host shadows (via std::free, no callback needed). Gating on
        // release_fn_ here would leak shadows if a collector ever passed
        // an empty free_cb. Device-pointer release is gated inside the
        // lambda instead.
        manager_.release_all_owned([this](void *p) {
            if (p != nullptr && release_fn_) {
                release_fn_(p);
            }
        });
    }

    InitRollbackGuard(const InitRollbackGuard &) = delete;
    InitRollbackGuard &operator=(const InitRollbackGuard &) = delete;

    void add_direct_ptr(void *p) {
        if (p != nullptr) direct_ptrs_.push_back(p);
    }
    void commit() { committed_ = true; }

private:
    Manager &manager_;
    ReleaseFn release_fn_;
    std::vector<void *> direct_ptrs_;
    bool committed_;
};

// Result of Module::resolve_entry. Carries everything the unified
// process_entry algorithm needs to (a) refill the originating pool's free
// queue and (b) hand the ready buffer off to the collector.
//
//   kind        — recycled-pool index in [0, Module::kBufferKinds).
//   free_queue  — the originating pool's SPSC queue to refill with one buffer.
//   buffer_size — bytes to allocate if the recycled+done fallbacks are dry.
//   info        — partially-filled ReadyBufferInfo (dev_buffer_ptr, buffer_seq,
//                 and any module-specific index fields are set; algorithm fills
//                 host_buffer_ptr after a resolve_host_ptr lookup).
template <typename Module>
struct EntrySite {
    int kind;
    typename Module::FreeQueue *free_queue;
    size_t buffer_size;
    typename Module::ReadyBufferInfo info;
};

// Unified mgmt-loop algorithms parameterized on Module's data-access traits.
// Module supplies the layout (constants + types + resolve_entry +
// for_each_instance); ProfilerAlgorithms supplies the control flow that used
// to be hand-rolled per subsystem.
template <typename Module>
struct ProfilerAlgorithms {
    using DataHeader = typename Module::DataHeader;
    using ReadyEntry = typename Module::ReadyEntry;
    using ReadyBufferInfo = typename Module::ReadyBufferInfo;
    using FreeQueue = typename Module::FreeQueue;

    // Pop one entry from the per-thread ready queue, advancing the head with
    // the appropriate memory barriers. Returns false if the queue is empty
    // (or the device wrote an out-of-range head/tail, which is treated as
    // empty and reported).
    //
    // a5: the head advance is written back to device immediately via
    // `mgr.write_range_to_device(&header->queue_heads[q], ...)` so AICPU sees
    // the consumer-side update without us bulk-mirroring the whole shm region
    // (which would clobber AICPU-owned fields elsewhere in the shm).
    //
    // Torn-read defense: the per-tick `mirror_shm_from_device` is a single
    // bulk rtMemcpy that is not atomic w.r.t. concurrent AICPU writes. AICPU
    // publishes a ready entry by first writing `queues[q][tail].{buffer_ptr,
    // core_index, buffer_seq}` and then bumping `queue_tails[q]`. If the
    // bulk mirror happens to scan the entry slot first and the tail counter
    // last, host can observe `head < tail` while the entry it's about to
    // read is still pre-publish (e.g. `buffer_ptr == 0`). We refresh the
    // entry with `read_range_from_device` and skip the pop if the refreshed
    // entry still looks empty — try again next tick.
    template <typename Mgr>
    static bool try_pop_aicpu_entry(Mgr &mgr, DataHeader *header, int q, ReadyEntry &out) {
        uint32_t head = header->queue_heads[q];
        uint32_t tail = header->queue_tails[q];
        if (head >= Module::kReadyQueueSize || tail >= Module::kReadyQueueSize) {
            LOG_ERROR(
                "%s: invalid queue indices for thread %d: head=%u tail=%u (max=%u)", Module::kSubsystemName, q, head,
                tail, Module::kReadyQueueSize
            );
            return false;
        }
        if (head == tail) return false;
        // Order the tail-vs-empty check before the entry read so the
        // entry load cannot be speculated past it on aarch64.
        rmb();

        // Re-pull this single entry from device to defeat the torn-read
        // race described above. If the entry's `buffer_ptr` is still 0 the
        // producer hasn't finished publishing — treat the queue as empty
        // for this tick.
        mgr.read_range_from_device(&header->queues[q][head], sizeof(header->queues[q][head]));
        rmb();
        out = header->queues[q][head];
        if (out.buffer_ptr == 0) {
            return false;
        }
        head = (head + 1) % Module::kReadyQueueSize;
        header->queue_heads[q] = head;
        wmb();
        // Push the new head value back to device. The bulk mirror_shm_to_device
        // is intentionally not used here — see buffer_pool_manager.h.
        mgr.write_range_to_device(&header->queue_heads[q], sizeof(header->queue_heads[q]));
        return true;
    }

    // Refill the originating pool's free_queue with exactly one buffer
    // (recycled → drain done → alloc), then push the popped buffer's
    // ReadyBufferInfo to the collector LAST. Skips the push if host_ptr
    // resolution fails — handing a null pointer to on_buffer_collected
    // would crash the collector thread.
    //
    // a5 specifics: after resolving the popped buffer's host shadow, copy
    // the buffer contents from device to host before delivery. The host
    // shadow seen by the collector then matches what the device wrote.
    template <typename Mgr>
    static void process_entry(Mgr &mgr, DataHeader *header, int q, const ReadyEntry &entry) {
        auto site_opt = Module::resolve_entry(mgr.shared_mem_host(), header, q, entry);
        if (!site_opt.has_value()) return;
        auto &site = *site_opt;

        void *new_dev = obtain_buffer(mgr, site.kind, site.buffer_size);
        if (new_dev != nullptr) {
            push_to_free_queue(mgr, *site.free_queue, new_dev);
        }

        site.info.host_buffer_ptr = mgr.resolve_host_ptr(site.info.dev_buffer_ptr);
        if (site.info.host_buffer_ptr == nullptr) {
            // resolve_host_ptr already logged. Drop rather than deliver null.
            return;
        }
        // a5: pull buffer contents from device into the host shadow before
        // the collector reads `count` and `records[]`.
        mgr.copy_buffer_from_device(site.info.host_buffer_ptr, site.info.dev_buffer_ptr, site.buffer_size);

        mgr.push_to_ready(site.info);
    }

    // Drain done_queue into recycled, then top up every (kind, instance)
    // free_queue to kSlotCount. When the recycled pool of a given kind drains
    // mid-fill, batch-allocate `batch_size(kind)` buffers and continue.
    template <typename Mgr>
    static void proactive_replenish(Mgr &mgr, DataHeader *header) {
        mgr.drain_done_into_recycled();
        Module::for_each_instance(mgr.shared_mem_host(), header, [&](int kind, FreeQueue *fq, size_t buf_size) {
            top_up_free_queue(mgr, kind, *fq, buf_size);
        });
    }

private:
    // Three-level fallback used by process_entry's 1-in/1-out replenish.
    template <typename Mgr>
    static void *obtain_buffer(Mgr &mgr, int kind, size_t buf_size) {
        void *p = mgr.pop_recycled(kind);
        if (p != nullptr) return p;
        mgr.drain_done_into_recycled();
        p = mgr.pop_recycled(kind);
        if (p != nullptr) return p;

        void *host_ptr = nullptr;
        p = mgr.alloc_and_register(buf_size, &host_ptr);
        if (p == nullptr) {
            LOG_WARN(
                "%s: alloc failed for %zu bytes (kind=%d) — increase BUFFERS_PER_* to reduce drops",
                Module::kSubsystemName, buf_size, kind
            );
        }
        return p;
    }

    // Append one buffer pointer to a per-instance free_queue. Caller owns
    // the "queue is not full" guarantee (process_entry: 1-in/1-out;
    // top_up_free_queue: explicit fq_used < kSlotCount).
    //
    // a5: write the new slot and the advanced tail back to device via
    // `write_range_to_device` so AICPU sees the refill without us bulk
    // mirroring (which would clobber AICPU-owned fields). The slot is
    // written before the tail so AICPU never observes a tail update without
    // the corresponding pointer.
    template <typename Mgr>
    static void push_to_free_queue(Mgr &mgr, FreeQueue &fq, void *dev_ptr) {
        uint32_t fq_tail = fq.tail;
        uint32_t slot_idx = fq_tail % Module::kSlotCount;
        fq.buffer_ptrs[slot_idx] = reinterpret_cast<uint64_t>(dev_ptr);
        wmb();
        mgr.write_range_to_device(&fq.buffer_ptrs[slot_idx], sizeof(fq.buffer_ptrs[slot_idx]));
        fq.tail = fq_tail + 1;
        wmb();
        mgr.write_range_to_device(&fq.tail, sizeof(fq.tail));
    }

    // Fill one (kind, instance) free_queue to kSlotCount, batch-allocating
    // when the recycled pool of this kind drains mid-fill.
    template <typename Mgr>
    static void top_up_free_queue(Mgr &mgr, int kind, FreeQueue &fq, size_t buf_size) {
        rmb();
        uint32_t fq_head = fq.head;
        uint32_t fq_tail = fq.tail;
        uint32_t fq_used = fq_tail - fq_head;

        while (fq_used < Module::kSlotCount) {
            void *new_dev = mgr.pop_recycled(kind);
            if (new_dev == nullptr) {
                const int batch = Module::batch_size(kind);
                for (int i = 0; i < batch; i++) {
                    void *host_ptr = nullptr;
                    void *dev = mgr.alloc_and_register(buf_size, &host_ptr);
                    if (dev == nullptr) break;
                    mgr.push_recycled(kind, dev);
                }
                new_dev = mgr.pop_recycled(kind);
            }
            if (new_dev == nullptr) return;

            uint32_t slot_idx = fq_tail % Module::kSlotCount;
            fq.buffer_ptrs[slot_idx] = reinterpret_cast<uint64_t>(new_dev);
            wmb();
            mgr.write_range_to_device(&fq.buffer_ptrs[slot_idx], sizeof(fq.buffer_ptrs[slot_idx]));
            fq_tail++;
            fq.tail = fq_tail;
            wmb();
            mgr.write_range_to_device(&fq.tail, sizeof(fq.tail));
            fq_used++;
        }
    }
};

template <typename Derived, typename Module>
class ProfilerBase {
public:
    using Manager = BufferPoolManager<Module>;
    using DataHeader = typename Module::DataHeader;
    using ReadyEntry = typename Module::ReadyEntry;
    using ReadyBufferInfo = typename Module::ReadyBufferInfo;

    ProfilerBase(const ProfilerBase &) = delete;
    ProfilerBase &operator=(const ProfilerBase &) = delete;

private:
    friend Derived;
    ProfilerBase() = default;
    ~ProfilerBase() = default;

public:
    /**
     * Stash the memory context produced by Derived::init(). Must be called
     * on the init() success path; if init aborts before this, start(tf) is
     * a no-op.
     *
     * `copy_to_device` / `copy_from_device` are arch-specific: SVM platforms
     * (a2a3) leave them null and pass `shm_dev == shm_host`; non-SVM
     * platforms (a5) install `profiling_copy_to_device` /
     * `profiling_copy_from_device` and pass distinct shm pointers. The
     * framework picks the right register fallback (identity vs host-shadow
     * malloc) based on whether `copy_to_device` was provided.
     *
     * `register_cb` may be nullptr — start(tf) installs the appropriate
     * default for the arch path (identity on SVM platforms, host-shadow
     * malloc + memset 0 + copy_to_device on non-SVM platforms).
     */
    void set_memory_context(
        const ProfAllocCallback &alloc_cb, ProfRegisterCallback register_cb, const ProfFreeCallback &free_cb,
        std::function<int(void *, const void *, size_t)> copy_to_device,
        std::function<int(void *, const void *, size_t)> copy_from_device, void *shm_dev, void *shm_host,
        size_t shm_size, int device_id
    ) {
        alloc_cb_ = alloc_cb;
        register_cb_ = register_cb;
        free_cb_ = free_cb;
        copy_to_device_ = std::move(copy_to_device);
        copy_from_device_ = std::move(copy_from_device);
        shm_dev_ = shm_dev;
        shm_host_ = shm_host;
        shm_size_ = shm_size;
        device_id_ = device_id;
    }

    /**
     * Drop the stashed memory context. Called by Derived::finalize() so
     * that a subsequent start(tf) on a finalized collector becomes a no-op.
     */
    void clear_memory_context() {
        alloc_cb_ = nullptr;
        register_cb_ = nullptr;
        free_cb_ = nullptr;
        copy_to_device_ = nullptr;
        copy_from_device_ = nullptr;
        shm_dev_ = nullptr;
        shm_host_ = nullptr;
        shm_size_ = 0;
        device_id_ = -1;
    }

    /**
     * Assemble a MemoryOps from the callbacks stashed by set_memory_context()
     * and launch the mgmt + poll threads. If shm_host_ is nullptr (Derived's
     * init() aborted before set_memory_context, or finalize() has cleared
     * the context) this is a no-op.
     *
     * Order matters: mgmt is started before poll because mgmt is the only
     * writer to L2 (the ready_queue) and poll is its sole consumer. The
     * register slot defaults to identity on the SVM path (copy_to_device_
     * is null) or to a host-shadow malloc lambda on the non-SVM path
     * (copy_to_device_ installed) — so BufferPoolManager always has a
     * valid reg path. The host-shadow lambda registers each malloc'd
     * shadow with `manager_.add_malloc_shadow()` so teardown can free
     * exactly the framework-owned shadows and leave HAL mappings alone.
     */
    void start(const ThreadFactory &thread_factory) {
        if (shm_host_ == nullptr) return;

        MemoryOps ops;
        ops.alloc = alloc_cb_;
        ops.free_ = free_cb_;
        if (register_cb_ != nullptr) {
            ops.reg = register_cb_;
        } else if (copy_to_device_) {
            // Non-SVM platform: host-shadow allocate + copy zeros to device.
            // Capture `this` so the malloc'd shadow can be registered as
            // framework-owned via the manager.
            auto copy_to_device = copy_to_device_;
            ops.reg = [this, copy_to_device](void *dev_ptr, size_t size, int /*device_id*/, void **host_ptr_out) {
                if (host_ptr_out == nullptr) return -1;
                void *host_ptr = std::malloc(size);
                if (host_ptr == nullptr) {
                    *host_ptr_out = nullptr;
                    return -1;
                }
                std::memset(host_ptr, 0, size);
                int rc = copy_to_device(dev_ptr, host_ptr, size);
                if (rc != 0) {
                    std::free(host_ptr);
                    *host_ptr_out = nullptr;
                    return rc;
                }
                manager_.add_malloc_shadow(host_ptr);
                *host_ptr_out = host_ptr;
                return 0;
            };
        } else {
            // SVM platform: identity-map (host_ptr == dev_ptr).
            ops.reg = [](void *dev_ptr, size_t /*size*/, int /*device_id*/, void **host_ptr_out) {
                *host_ptr_out = dev_ptr;
                return 0;
            };
        }
        // copy_to_device_ / copy_from_device_ may be null (SVM path); the
        // manager's internal null-checks short-circuit mirror_/range_/buffer_
        // calls to no-ops in that case.
        ops.copy_to_device = copy_to_device_;
        ops.copy_from_device = copy_from_device_;
        manager_.set_memory_context(std::move(ops), shm_dev_, shm_host_, shm_size_, device_id_);

        mgmt_running_.store(true, std::memory_order_release);
        if (thread_factory) {
            mgmt_thread_ = thread_factory([this]() {
                mgmt_loop();
            });
        } else {
            mgmt_thread_ = std::thread(&ProfilerBase::mgmt_loop, this);
        }

        execution_complete_.store(false, std::memory_order_release);
        if (thread_factory) {
            collector_thread_ = thread_factory([this]() {
                poll_and_collect_loop();
            });
        } else {
            collector_thread_ = std::thread(&ProfilerBase::poll_and_collect_loop, this);
        }
    }

    /**
     * Stop the mgmt thread, drain whatever it pushes during its final pass,
     * and join the collector. Idempotent. Caller is guaranteed on return
     * that mgmt's L1 ringbuffer and the host-side L2 ready_queue are both
     * empty and Derived::on_buffer_collected has been called for every
     * entry that was in either queue. Framework-owned buffers are NOT freed
     * here — Derived's finalize() must do that.
     *
     * Order matters: stop+join mgmt first so its final-drain pass is fully
     * landed in L2 BEFORE we tell poll to exit. Otherwise mgmt's last batch
     * has no consumer.
     */
    void stop() {
        mgmt_running_.store(false, std::memory_order_release);
        if (mgmt_thread_.joinable()) {
            mgmt_thread_.join();
        }
        execution_complete_.store(true, std::memory_order_release);
        if (collector_thread_.joinable()) {
            collector_thread_.join();
        }
    }

    Manager &manager() { return manager_; }
    const Manager &manager() const { return manager_; }

protected:
    Manager manager_;
    std::atomic<bool> execution_complete_{false};
    std::thread collector_thread_;

    // Memory context stashed by Derived::init() via set_memory_context().
    // Derived may read these from finalize() / alloc helpers via the
    // inherited names. ProfilerBase owns the lifetime: Derived must call
    // clear_memory_context() in finalize() to drop them.
    ProfAllocCallback alloc_cb_{nullptr};
    ProfRegisterCallback register_cb_{nullptr};
    ProfFreeCallback free_cb_{nullptr};
    // copy_to_device_ / copy_from_device_ are set by non-SVM platforms
    // (a5) to profiling_copy_* wrappers; left null by SVM platforms (a2a3)
    // so the manager's mirror methods short-circuit to no-ops.
    std::function<int(void *, const void *, size_t)> copy_to_device_;
    std::function<int(void *, const void *, size_t)> copy_from_device_;
    void *shm_dev_{nullptr};
    void *shm_host_{nullptr};
    size_t shm_size_{0};
    int device_id_{-1};

    /**
     * RAII counterpart of ``alloc_single_buffer``: unregister the host
     * mapping (if there is one) then release the device memory. Each
     * Derived's ``finalize()`` funnels every release site through here so
     * the framework never frees a dev_ptr without first taking down the
     * matching ``halHostRegister`` slot. On a5 onboard ``register_cb`` is
     * always nullptr so the unregister branch is a no-op — the helper is
     * shared with a2a3 anyway for code uniformity.
     */
    void release_one_buffer(void *dev_ptr, ProfUnregisterCallback unregister_cb, const ProfFreeCallback &free_cb) {
        if (dev_ptr == nullptr) return;
        if (unregister_cb != nullptr) {
            int rc = unregister_cb(dev_ptr, device_id_);
            if (rc != 0) {
                LOG_ERROR("halHostUnregister failed for dev_ptr %p: %d", dev_ptr, rc);
            }
        }
        if (free_cb) {
            free_cb(dev_ptr);
        }
    }

    /**
     * Allocate a device buffer and its paired host view, picking the right
     * pairing strategy based on the memory context stashed by
     * set_memory_context():
     *
     *   - `register_cb_` set       (a2a3 onboard): `register_cb_(dev, …)`
     *     installs the halHostRegister mapping; host_ptr is the
     *     identity-mapped view of the same memory.
     *   - non-SVM platform (a5):   `copy_to_device_` is installed →
     *     malloc a paired host shadow, zero it, push the zeros to the
     *     device side. The host shadow lives until release_one_buffer
     *     `std::free()`s it.
     *   - SVM platform (a2a3 sim): `register_cb_` null AND `copy_to_device_`
     *     null → identity-map (host_ptr == dev_ptr).
     *
     * On any failure the device pointer is freed via `free_cb_` and
     * nullptr is returned; on success the dev↔host mapping is registered
     * with the buffer pool so resolve_host_ptr() finds it later.
     *
     * Used by leaf collectors' init() to allocate the shared-memory header
     * region and any per-instance buffers, replacing the per-arch ad-hoc
     * branch trees they used to carry.
     */
    void *alloc_paired_buffer(size_t size, void **host_ptr_out) {
        if (host_ptr_out == nullptr) return nullptr;
        *host_ptr_out = nullptr;
        if (!alloc_cb_) return nullptr;

        void *dev_ptr = alloc_cb_(size);
        if (dev_ptr == nullptr) return nullptr;

        void *host_ptr = nullptr;
        if (register_cb_ != nullptr) {
            int rc = register_cb_(dev_ptr, size, device_id_, &host_ptr);
            if (rc != 0 || host_ptr == nullptr) {
                LOG_ERROR("ProfilerBase::alloc_paired_buffer: register_cb_ failed: %d", rc);
                if (free_cb_) free_cb_(dev_ptr);
                return nullptr;
            }
        } else if (copy_to_device_) {
            // Non-SVM: malloc + zero + push to device.
            host_ptr = std::malloc(size);
            if (host_ptr == nullptr) {
                LOG_ERROR("ProfilerBase::alloc_paired_buffer: host shadow alloc failed for %zu bytes", size);
                if (free_cb_) free_cb_(dev_ptr);
                return nullptr;
            }
            std::memset(host_ptr, 0, size);
            int rc = copy_to_device_(dev_ptr, host_ptr, size);
            if (rc != 0) {
                LOG_ERROR("ProfilerBase::alloc_paired_buffer: copy_to_device failed: %d", rc);
                std::free(host_ptr);
                if (free_cb_) free_cb_(dev_ptr);
                return nullptr;
            }
            manager_.add_malloc_shadow(host_ptr);
        } else {
            // SVM: identity-map.
            host_ptr = dev_ptr;
        }

        *host_ptr_out = host_ptr;
        manager_.register_mapping(dev_ptr, host_ptr);
        return dev_ptr;
    }

private:
    /**
     * mgmt thread main loop. Each tick:
     *   0) Mirror the device-side shared-memory region (DataHeader + all
     *      BufferStates) into the host shadow so subsequent reads see the
     *      latest queue_tails / current_buf_ptr / per-state counters.
     *   1) Drain done_queue into recycled pools.
     *   2) Iterate AICPU per-thread ready queues (PLATFORM_MAX_AICPU_THREADS
     *      upper bound; empty queues are O(1) head==tail checks) and call
     *      Module::process_entry per entry. process_entry pulls each
     *      popped buffer's contents from device on demand.
     *      try_pop_aicpu_entry / push_to_free_queue write the few host-modified
     *      fields (queue_heads[q], free_queue.tail/buffer_ptrs[]) back to
     *      device immediately via `write_range_to_device`.
     *   3) Call Module::proactive_replenish to top up any depleted free
     *      queues.
     *   4) Sleep 10 us if no work was done.
     *
     * The bulk `mirror_shm_to_device` deliberately is NOT called: it races
     * with AICPU writes to device-only fields (current_buf_ptr, total/dropped/
     * mismatch counters, queue_tails, free_queue.head, core_to_thread[],
     * and on a5 L2SwimlaneAicpuPhaseHeader::magic) and rolls them back to
     * whatever was mirrored in at the start of the tick. Each host-side
     * modification is written back as a narrow field write inside Alg.
     *
     * On exit (mgmt_running_ → false) it does one final drain pass without
     * sleeping to flush any straggler entries the device pushed before
     * stopping.
     */
    void mgmt_loop() {
        DataHeader *header = Module::header_from_shm(manager_.shared_mem_host());
        using Alg = ProfilerAlgorithms<Module>;

        while (mgmt_running_.load(std::memory_order_acquire)) {
            manager_.mirror_shm_from_device();

            manager_.drain_done_into_recycled();

            bool found_any = false;
            for (int q = 0; q < PLATFORM_MAX_AICPU_THREADS; q++) {
                ReadyEntry entry;
                while (Alg::try_pop_aicpu_entry(manager_, header, q, entry)) {
                    Alg::process_entry(manager_, header, q, entry);
                    found_any = true;
                }
            }

            Alg::proactive_replenish(manager_, header);

            if (!found_any) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        // Final drain after mgmt_running_ flipped: don't sleep, don't
        // replenish. try_pop_aicpu_entry still pushes the advanced
        // queue_heads back to device per-pop.
        manager_.mirror_shm_from_device();
        for (int q = 0; q < PLATFORM_MAX_AICPU_THREADS; q++) {
            ReadyEntry entry;
            while (Alg::try_pop_aicpu_entry(manager_, header, q, entry)) {
                Alg::process_entry(manager_, header, q, entry);
            }
        }
    }

    /**
     * Main collector loop. Blocks on the manager's ready_queue with a 100 ms
     * cv-wait tick. On each hit it dispatches the buffer to Derived via
     * on_buffer_collected() and recycles the buffer. Exits in two cases:
     *
     *   1. execution_complete_ was set (by stop()) and the ready_queue is
     *      empty, after a final non-blocking drain pass.
     *   2. No buffer arrived for `Derived::kIdleTimeoutSec` consecutive
     *      seconds AND execution_complete_ has not been signalled — this
     *      is a hang detector that logs an error and bails out.
     */
    void poll_and_collect_loop() {
        const auto wait_tick = std::chrono::milliseconds(100);
        const auto idle_timeout = std::chrono::seconds(Derived::kIdleTimeoutSec);
        std::optional<std::chrono::steady_clock::time_point> idle_start;

        while (true) {
            ReadyBufferInfo info;
            if (manager_.wait_pop_ready(info, wait_tick)) {
                consume(info);
                idle_start.reset();
                continue;
            }
            if (execution_complete_.load(std::memory_order_acquire)) {
                while (manager_.try_pop_ready(info)) {
                    consume(info);
                }
                break;
            }
            if (!idle_start.has_value()) {
                idle_start = std::chrono::steady_clock::now();
            }
            if (std::chrono::steady_clock::now() - idle_start.value() >= idle_timeout) {
                LOG_ERROR(
                    "%s collector idle timeout after %d seconds — giving up", Derived::kSubsystemName,
                    Derived::kIdleTimeoutSec
                );
                break;
            }
        }
    }

    void consume(const ReadyBufferInfo &info) {
        static_cast<Derived *>(this)->on_buffer_collected(info);
        if constexpr (Module::kBufferKinds > 1) {
            manager_.notify_copy_done(info.dev_buffer_ptr, Module::kind_of(info));
        } else {
            manager_.notify_copy_done(info.dev_buffer_ptr, 0);
        }
    }

    std::thread mgmt_thread_;
    std::atomic<bool> mgmt_running_{false};
};

}  // namespace profiling_common

#endif  // ESL_PROXY_SWIMLANE_COLLECTOR_BASE_H
