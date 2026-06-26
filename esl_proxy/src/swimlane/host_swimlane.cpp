/* Host-side L2 swimlane collector wrapper (C ABI for host_onboard.c). */
#define ESL_SWIMLANE_HOST_CPP
#include "swimlane_host.h"

#include "kernel_args.h"
#include "swimlane_collector.h"

#include "core_type.h"

#include <acl/acl.h>
#include <ascend_hal.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

void *g_hal_handle = nullptr;

using HalHostRegisterFn = int (*)(void *dev_ptr, size_t size, unsigned int flags, int device_id, void **host_ptr);
using HalHostUnregisterFn = int (*)(void *host_ptr, int device_id);

int load_hal_if_needed(void)
{
    if (g_hal_handle != nullptr) {
        return 0;
    }
    g_hal_handle = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    return g_hal_handle != nullptr ? 0 : -1;
}

HalHostRegisterFn get_hal_host_register(void)
{
    if (g_hal_handle == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HalHostRegisterFn>(dlsym(g_hal_handle, "halHostRegister"));
}

HalHostUnregisterFn get_hal_host_unregister(void)
{
    if (g_hal_handle == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HalHostUnregisterFn>(dlsym(g_hal_handle, "halHostUnregister"));
}

L2SwimlaneCollector g_collector;
L2SwimlaneLevel g_level = L2SwimlaneLevel::AICORE_TIMING;
int g_device_id = 0;
bool g_initialized = false;
std::string g_output_prefix = ".";

void *acl_alloc(size_t size)
{
    void *ptr = nullptr;
    if (aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        return nullptr;
    }
    return ptr;
}

int acl_free(void *ptr)
{
    return ptr != nullptr ? (int)aclrtFree(ptr) : 0;
}

int hal_register(void *dev_ptr, size_t size, int device_id, void **host_ptr)
{
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostRegisterFn fn = get_hal_host_register();
    if (fn == nullptr) {
        return -1;
    }
    return fn(dev_ptr, size, DEV_SVM_MAP_HOST, device_id, host_ptr);
}

int hal_unregister(void *host_ptr, int device_id)
{
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostUnregisterFn fn = get_hal_host_unregister();
    if (fn == nullptr) {
        return -1;
    }
    return fn(host_ptr, device_id);
}

static void export_perfetto_trace_json(void)
{
    const char *esl_root = getenv("ESL_PROXY_ROOT");
    if (esl_root == nullptr || esl_root[0] == '\0') {
        esl_root = ".";
    }
    const std::string raw = g_output_prefix + "/l2_swimlane_records.json";
    const std::string out = g_output_prefix + "/l2_swimlane_trace.json";
    const std::string script = std::string(esl_root) + "/tools/swimlane_to_perfetto.py";
    const std::string cmd =
        "python3 '" + script + "' '" + raw + "' -o '" + out + "' >/dev/null 2>&1";

    if (std::system(cmd.c_str()) == 0) {
        fprintf(stderr, "[esl_proxy] Perfetto trace: %s\n", out.c_str());
        fprintf(stderr, "[esl_proxy] Import at https://ui.perfetto.dev/\n");
    } else {
        fprintf(stderr,
            "[esl_proxy] Perfetto conversion failed; raw swimlane data at %s "
            "(run: python3 %s %s)\n",
            raw.c_str(), script.c_str(), raw.c_str());
    }
}

}  // namespace

extern "C" void esl_swimlane_host_set_level(int level)
{
    if (level < 0) {
        g_level = L2SwimlaneLevel::DISABLED;
        return;
    }
    if (level > static_cast<int>(L2SwimlaneLevel::AICORE_TIMING)) {
        level = static_cast<int>(L2SwimlaneLevel::AICORE_TIMING);
    }
    g_level = static_cast<L2SwimlaneLevel>(level);
}

extern "C" int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix)
{
    if (g_level == L2SwimlaneLevel::DISABLED || worker_count <= 0) {
        return 0;
    }
    g_device_id = device_id;
    if (output_prefix != nullptr && output_prefix[0] != '\0') {
        g_output_prefix = output_prefix;
    } else {
        g_output_prefix = ".";
    }

    auto alloc_cb = [](size_t size) -> void * { return acl_alloc(size); };
    auto free_cb = [](void *ptr) -> int { return acl_free(ptr); };

    int rc = g_collector.initialize(
        worker_count, aicpu_thread_num, device_id, g_level, alloc_cb, hal_register, free_cb, g_output_prefix);
    if (rc != 0) {
        fprintf(stderr, "[esl_proxy] swimlane init failed rc=%d\n", rc);
        return rc;
    }
    g_initialized = true;
    return 0;
}

extern "C" void esl_swimlane_host_start(void)
{
    if (!g_initialized) {
        return;
    }
    // CANN device context is per-thread; collector mgmt/poll threads call
    // aclrtMalloc when recycling buffers — mirror simpler DeviceRunner::create_thread.
    const int dev_id = g_device_id;
    g_collector.start([dev_id](std::function<void()> fn) -> std::thread {
        return std::thread([dev_id, fn = std::move(fn)]() {
            if (aclrtSetDevice(dev_id) != ACL_SUCCESS) {
                fprintf(stderr, "[esl_proxy] swimlane thread aclrtSetDevice(%d) failed\n", dev_id);
                return;
            }
            fn();
        });
    });
}

extern "C" void esl_swimlane_host_stop_and_export(void)
{
    if (!g_initialized) {
        return;
    }
    g_collector.stop();
    g_collector.reconcile_counters();
    if (g_collector.export_swimlane_json() == 0) {
        export_perfetto_trace_json();
    }
}

extern "C" void esl_swimlane_host_finalize(void)
{
    if (!g_initialized) {
        return;
    }
    g_collector.finalize(hal_unregister, [](void *ptr) -> int { return acl_free(ptr); });
    g_initialized = false;
}

extern "C" uint64_t esl_swimlane_host_data_base(void)
{
    if (!g_initialized) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(g_collector.get_l2_swimlane_setup_device_ptr());
}

extern "C" uint64_t esl_swimlane_host_rotation_table(void)
{
    if (!g_initialized) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(g_collector.get_aicore_ring_addr_table_device_ptr());
}

extern "C" void esl_swimlane_host_set_core_types(const int32_t *core_types, int count)
{
    if (!g_initialized || core_types == nullptr || count <= 0) {
        return;
    }
    std::vector<CoreType> types(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        types[static_cast<size_t>(i)] = static_cast<CoreType>(core_types[i]);
    }
    g_collector.set_core_types(types.data(), count);
}

static int parse_swimlane_level_from_env(void)
{
    const char *env_val = getenv("ESL_PROXY_L2_SWIMLANE_LEVEL");
    int level = 0;

    if (env_val != nullptr && env_val[0] != '\0') {
        level = atoi(env_val);
        if (level < 0) {
            level = 0;
        } else if (level > 1) {
            level = 1;
        }
    }
    return level;
}

extern "C" int esl_swimlane_host_onboard_begin(int device_id, const char *output_prefix)
{
    const int level = parse_swimlane_level_from_env();

    esl_swimlane_host_set_level(level);
    if (level > 0) {
        fprintf(stderr, "[esl_proxy] L2 swimlane enabled level=%d (no PMU)\n", level);
        return esl_swimlane_host_init(ESL_PROXY_ONBOARD_WORKER_COUNT, ESL_PROXY_AICPU_THREAD_NUM,
                                      device_id, output_prefix);
    }
    return 0;
}

extern "C" void esl_swimlane_host_onboard_fill_kargs(void *k_args_opaque)
{
    EslKernelArgs *k_args = static_cast<EslKernelArgs *>(k_args_opaque);
    if (k_args == nullptr || g_level == L2SwimlaneLevel::DISABLED) {
        return;
    }
    k_args->l2_swimlane_data_base = esl_swimlane_host_data_base();
    k_args->l2_swimlane_aicore_rotation_table = esl_swimlane_host_rotation_table();
    ESL_SWIMLANE_PROFILING_FLAG_ON(k_args->enable_profiling_flag);
}

extern "C" void esl_swimlane_host_onboard_sync_core_types(void *dev_runtime_dev_ptr)
{
    if (g_level == L2SwimlaneLevel::DISABLED || dev_runtime_dev_ptr == nullptr) {
        return;
    }
    EslRuntime dev_runtime_host;
    int32_t core_types[ESL_PROXY_ONBOARD_WORKER_COUNT];
    int w;

    memset(&dev_runtime_host, 0, sizeof(dev_runtime_host));
    if (aclrtMemcpy(&dev_runtime_host, sizeof(dev_runtime_host), dev_runtime_dev_ptr,
                    sizeof(EslRuntime), ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        return;
    }
    for (w = 0; w < ESL_PROXY_ONBOARD_WORKER_COUNT; ++w) {
        core_types[w] = dev_runtime_host.workers[w].core_type;
    }
    esl_swimlane_host_set_core_types(core_types, ESL_PROXY_ONBOARD_WORKER_COUNT);
}

extern "C" void esl_swimlane_host_onboard_end(void)
{
    esl_swimlane_host_stop_and_export();
    esl_swimlane_host_finalize();
}

#include <chrono>
#include <cinttypes>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "memory_barrier.h"
#include "onboard_log.h"

// =============================================================================
// L2SwimlaneCollector Implementation
// =============================================================================

L2SwimlaneCollector::~L2SwimlaneCollector() {
    stop();
    if (shm_host_ != nullptr) {
        LOG_WARN("L2SwimlaneCollector destroyed without finalize()");
    }
}

void *L2SwimlaneCollector::alloc_single_buffer(size_t size, void **host_ptr_out) {
    void *dev_ptr = alloc_cb_(size);
    if (dev_ptr == nullptr) {
        LOG_ERROR("Failed to allocate buffer (%zu bytes)", size);
        *host_ptr_out = nullptr;
        return nullptr;
    }

    if (register_cb_ != nullptr) {
        void *host_ptr = nullptr;
        int rc = register_cb_(dev_ptr, size, device_id_, &host_ptr);
        if (rc != 0 || host_ptr == nullptr) {
            LOG_ERROR("Buffer registration failed: %d", rc);
            *host_ptr_out = nullptr;
            return nullptr;
        }
        *host_ptr_out = host_ptr;
    } else {
        *host_ptr_out = dev_ptr;
    }

    // Register mapping so the BufferPoolManager can resolve dev→host
    manager_.register_mapping(dev_ptr, *host_ptr_out);
    return dev_ptr;
}

int L2SwimlaneCollector::initialize(
    int num_aicore, int aicpu_thread_num, int device_id, L2SwimlaneLevel l2_swimlane_level,
    const L2SwimlaneAllocCallback &alloc_cb, L2SwimlaneRegisterCallback register_cb,
    const L2SwimlaneFreeCallback &free_cb, const std::string &output_prefix
) {
    if (shm_host_ != nullptr) {
        LOG_ERROR("L2SwimlaneCollector already initialized");
        return -1;
    }

    LOG_INFO_V0("Initializing performance profiling");

    if (num_aicore <= 0 || num_aicore > PLATFORM_MAX_CORES) {
        LOG_ERROR("Invalid number of AICores: %d (max=%d)", num_aicore, PLATFORM_MAX_CORES);
        return -1;
    }

    num_aicore_ = num_aicore;
    aicpu_thread_num_ = aicpu_thread_num;
    l2_swimlane_level_ = l2_swimlane_level;
    output_prefix_ = output_prefix;
    total_aicore_collected_ = 0;

    // Stash the memory context on the base up-front so alloc_single_buffer
    // sees consistent values during init. shm_host_ stays nullptr until the
    // shm allocation succeeds — the nullptr guard makes a post-failure
    // start(tf) a no-op.
    set_memory_context(
        alloc_cb, register_cb, free_cb, /*copy_to=*/nullptr, /*copy_from=*/nullptr, /*shm_dev=*/nullptr,
        /*shm_host=*/nullptr, /*shm_size=*/0, device_id
    );

    size_t total_size = calc_l2_swimlane_data_size(num_aicore);

    LOG_DEBUG("Shared memory allocation plan:");
    LOG_DEBUG("  Number of cores:      %d", num_aicore);
    LOG_DEBUG("  Header size:          %zu bytes", sizeof(L2SwimlaneDataHeader));
    LOG_DEBUG("  L2SwimlaneAicoreTaskPool size: %zu bytes each", sizeof(L2SwimlaneAicoreTaskPool));
    LOG_DEBUG("  Total shared memory:  %zu bytes (%zu KB)", total_size, total_size / 1024);

    // Step 2: Allocate shared memory for slot arrays
    void *perf_dev_ptr = alloc_cb(total_size);
    if (perf_dev_ptr == nullptr) {
        LOG_ERROR("Failed to allocate shared memory (%zu bytes)", total_size);
        return -1;
    }
    LOG_DEBUG("Allocated shared memory: %p", perf_dev_ptr);

    // Step 3: Register to host mapping (optional)
    void *perf_host_ptr = nullptr;
    if (register_cb != nullptr) {
        int rc = register_cb(perf_dev_ptr, total_size, device_id, &perf_host_ptr);
        if (rc != 0) {
            LOG_ERROR("Memory registration failed: %d", rc);
            return rc;
        }
        if (perf_host_ptr == nullptr) {
            LOG_ERROR("register_cb succeeded but returned null host_ptr");
            return -1;
        }
        LOG_DEBUG("Mapped to host memory: %p", perf_host_ptr);
    } else {
        perf_host_ptr = perf_dev_ptr;
        LOG_DEBUG("Simulation mode: host_ptr = dev_ptr = %p", perf_host_ptr);
    }

    // Step 4: Initialize header
    L2SwimlaneDataHeader *header = get_l2_swimlane_header(perf_host_ptr);

    for (int t = 0; t < PLATFORM_MAX_AICPU_THREADS; t++) {
        memset(header->queues[t], 0, sizeof(header->queues[t]));
        header->queue_heads[t] = 0;
        header->queue_tails[t] = 0;
    }

    header->num_cores = num_aicore;
    header->l2_swimlane_level = static_cast<uint32_t>(l2_swimlane_level_);
    // Legacy phase metadata fields — always zero at level 1.
    header->num_sched_phase_threads = 0;
    header->num_orch_phase_threads = 0;
    header->num_phase_cores = 0;
    memset(header->core_to_thread, -1, sizeof(header->core_to_thread));

    LOG_DEBUG("Initialized L2SwimlaneDataHeader:");
    LOG_DEBUG("  num_cores:              %d", header->num_cores);
    LOG_DEBUG("  l2_swimlane_level: %u", header->l2_swimlane_level);
    LOG_DEBUG("  aicore_buffer_capacity: %d", PLATFORM_AICORE_BUFFER_SIZE);
    LOG_DEBUG("  queue capacity:         %d", PLATFORM_PROF_READYQUEUE_SIZE);

    // Step 5: Initialize L2SwimlaneAicoreTaskPools — per-core AICore rotation
    // channel + buffer pool. Same SPSC pattern as the AICPU pool above.
    for (int i = 0; i < num_aicore; i++) {
        L2SwimlaneAicoreTaskPool *ac_state = get_aicore_buffer_state(perf_host_ptr, num_aicore, i);
        memset(ac_state, 0, sizeof(L2SwimlaneAicoreTaskPool));

        for (int s = 0; s < PLATFORM_AICORE_BUFFERS_PER_CORE; s++) {
            void *host_buf_ptr = nullptr;
            void *dev_buf_ptr = alloc_single_buffer(sizeof(L2SwimlaneAicoreTaskBuffer), &host_buf_ptr);
            if (dev_buf_ptr == nullptr) {
                LOG_ERROR("Failed to allocate L2SwimlaneAicoreTaskBuffer for core %d, buffer %d", i, s);
                return -1;
            }
            L2SwimlaneAicoreTaskBuffer *buf = reinterpret_cast<L2SwimlaneAicoreTaskBuffer *>(host_buf_ptr);
            memset(buf, 0, sizeof(L2SwimlaneAicoreTaskBuffer));
            buf->count = 0;

            if (s == 0) {
                ac_state->free_queue.buffer_ptrs[0] = reinterpret_cast<uint64_t>(dev_buf_ptr);
            } else {
                manager_.push_recycled(static_cast<int>(ProfBufferType::AICORE_TASK), dev_buf_ptr);
            }
        }
        wmb();
        ac_state->free_queue.tail = 1;
        wmb();
    }
    LOG_DEBUG(
        "Initialized buffer pools: %d L2SwimlaneAicoreTaskBuffers/core (1 in free_queue, rest in recycled pool)",
        PLATFORM_AICORE_BUFFERS_PER_CORE
    );

    // Step 5c: Standalone uint64_t[num_aicore] table that will hold per-core
    // L2SwimlaneActiveHead device addresses. Host only allocates the bytes and
    // hands the device pointer to AICPU via KernelArgs::l2_swimlane_aicore_rotation_table;
    // AICPU itself fills the entries inside `l2_swimlane_aicpu_init` (it has
    // direct access to `&ac_state->head` device addresses, no
    // host-to-device translation needed). AICore reads
    // rotation_table[block_idx] at kernel entry.
    {
        size_t table_bytes = static_cast<size_t>(num_aicore) * sizeof(uint64_t);
        void *rotation_table_host = nullptr;
        void *rotation_table_dev = alloc_single_buffer(table_bytes, &rotation_table_host);
        if (rotation_table_dev == nullptr) {
            LOG_ERROR("Failed to allocate l2_swimlane_aicore_rotation_table (rotation) table (%zu bytes)", table_bytes);
            return -1;
        }
        aicore_ring_addr_table_dev_ = rotation_table_dev;
    }

    wmb();

    // Step 6: Stash device pointer for the caller to publish via
    // kernel_args.l2_swimlane_data_base (read back via get_l2_swimlane_setup_device_ptr()).
    LOG_DEBUG("L2 swimlane device base = 0x%lx", reinterpret_cast<uint64_t>(perf_dev_ptr));

    perf_shared_mem_dev_ = perf_dev_ptr;
    // Refresh memory context with the now-known SHM tuple. start(tf) (inherited)
    // gates on shm_host_, so this is the moment the collector becomes startable.
    set_memory_context(
        alloc_cb, register_cb, free_cb, /*copy_to=*/nullptr, /*copy_from=*/nullptr, perf_dev_ptr, perf_host_ptr,
        total_size, device_id
    );

    collected_aicore_records_.assign(num_aicore_, {});

    LOG_INFO_V0("Performance profiling initialized (dynamic buffer mode)");
    return 0;
}

// ---------------------------------------------------------------------------
// ProfilerBase callbacks
// ---------------------------------------------------------------------------

void L2SwimlaneCollector::copy_aicore_buffer(const ReadyBufferInfo &info) {
    L2SwimlaneAicoreTaskBuffer *buf = reinterpret_cast<L2SwimlaneAicoreTaskBuffer *>(info.host_buffer_ptr);
    rmb();
    uint32_t core_index = info.index;
    if (core_index >= static_cast<uint32_t>(num_aicore_)) {
        return;
    }
    uint32_t count = buf->count;
    if (count > static_cast<uint32_t>(PLATFORM_AICORE_BUFFER_SIZE)) {
        count = PLATFORM_AICORE_BUFFER_SIZE;
    }
    auto &dst = collected_aicore_records_[core_index];
    dst.reserve(dst.size() + count);
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < count; i++) {
        const L2SwimlaneAicoreTaskRecord &r = buf->records[i];
        if (r.start_time == 0) {
            skipped++;
            continue;
        }
        dst.push_back(r);
    }
    total_aicore_collected_ += static_cast<uint64_t>(count - skipped);
    if (skipped > 0) {
        LOG_WARN(
            "Core %u: skipped %u AICore record slot(s) with start_time=0 (race-window write or "
            "recycled-buffer tail). buf seq=%u count=%u",
            core_index, skipped, info.buffer_seq, count
        );
    }
}

void L2SwimlaneCollector::on_buffer_collected(const ReadyBufferInfo &info) {
    if (info.type == ProfBufferType::AICORE_TASK) {
        copy_aicore_buffer(info);
    }
}

// ---------------------------------------------------------------------------
// reconcile_counters
// ---------------------------------------------------------------------------

void L2SwimlaneCollector::reconcile_counters() {
    if (shm_host_ == nullptr) {
        return;
    }

    rmb();

    auto reconcile_one = [&](const char *kind, int unit_count, auto get_state, auto read_buf_count, uint64_t collected) {
        int leftover_active = 0;
        for (int i = 0; i < unit_count; i++) {
            L2SwimlaneAicoreTaskPool *state = get_state(i);
            uint64_t buf_ptr = state->head.current_buf_ptr;
            if (buf_ptr == 0) continue;
            void *host_ptr = manager_.resolve_host_ptr(reinterpret_cast<void *>(buf_ptr));
            if (host_ptr == nullptr) continue;
            uint32_t count = read_buf_count(host_ptr);
            if (count == 0) continue;
            LOG_ERROR(
                "L2Swimlane reconcile: core %d has un-flushed %s buffer (current_buf_ptr=0x%lx, count=%u) "
                "after stop() — device flush failed",
                i, kind, static_cast<unsigned long>(buf_ptr), count
            );
            leftover_active++;
        }

        uint64_t total_device = 0;
        uint64_t dropped_device = 0;
        for (int i = 0; i < unit_count; i++) {
            L2SwimlaneAicoreTaskPool *state = get_state(i);
            total_device += state->head.total_record_count;
            dropped_device += state->head.dropped_record_count;
        }

        if (dropped_device > 0) {
            LOG_WARN(
                "L2Swimlane reconcile: %lu %s records dropped on device side (buffer full / ready_queue full).",
                static_cast<unsigned long>(dropped_device), kind
            );
        }
        uint64_t accounted = collected + dropped_device;
        if (accounted != total_device) {
            LOG_WARN(
                "L2Swimlane reconcile: %s count mismatch (collected=%lu + dropped=%lu != "
                "device_total=%lu, silent_loss=%ld)",
                kind, static_cast<unsigned long>(collected), static_cast<unsigned long>(dropped_device),
                static_cast<unsigned long>(total_device), static_cast<long>(total_device) - static_cast<long>(accounted)
            );
        } else {
            LOG_INFO_V0(
                "L2Swimlane reconcile: %s counts match (collected=%lu, dropped=%lu, device_total=%lu)", kind,
                static_cast<unsigned long>(collected), static_cast<unsigned long>(dropped_device),
                static_cast<unsigned long>(total_device)
            );
        }

        if (leftover_active > 0) {
            LOG_ERROR(
                "L2Swimlane reconcile: %d core(s) had un-cleared %s current_buf_ptr — see prior errors",
                leftover_active, kind
            );
        }
    };

    reconcile_one(
        "AICORE", num_aicore_,
        [this](int core_index) { return get_aicore_buffer_state(shm_host_, num_aicore_, core_index); },
        [](void *host_ptr) { return reinterpret_cast<L2SwimlaneAicoreTaskBuffer *>(host_ptr)->count; },
        total_aicore_collected_
    );
}

void L2SwimlaneCollector::set_core_types(const CoreType *types, int n) {
    if (types == nullptr || n <= 0) {
        core_types_.clear();
        return;
    }
    core_types_.assign(types, types + n);
}

// JSON v2 emit: the host now dumps raw cycle-domain per-stream records plus
// metadata, and `swimlane_converter.py` performs the join (AICore↔AICPU on
// reg_task_id, base_time normalization, cycles→µs conversion, sort, core_type
// lookup, func_id resolution against deps.json). Moving the join into Python
// makes the schema easy to evolve without round-tripping through C++ + a
// rebuild, and shrinks this file to a pure dump.
int L2SwimlaneCollector::export_swimlane_json() {
    if (shm_host_ == nullptr) {
        return -1;
    }

    bool has_any_records = false;
    for (const auto &ac_records : collected_aicore_records_) {
        if (!ac_records.empty()) {
            has_any_records = true;
            break;
        }
    }
    if (!has_any_records) {
        LOG_WARN("Warning: No performance data to export.");
        return -1;
    }

    std::error_code ec;
    std::filesystem::create_directories(output_prefix_, ec);
    if (ec) {
        LOG_ERROR("Error: Failed to create output directory %s: %s", output_prefix_.c_str(), ec.message().c_str());
        return -1;
    }

    std::string filepath = output_prefix_ + "/l2_swimlane_records.json";
    std::ofstream outfile(filepath);
    if (!outfile.is_open()) {
        LOG_ERROR("Error: Failed to open file: %s", filepath.c_str());
        return -1;
    }

    int l2_swimlane_level = static_cast<int>(l2_swimlane_level_);

    outfile << "{\n";
    outfile << "  \"l2_swimlane_level\": " << l2_swimlane_level << ",\n";

    // metadata: everything python needs that isn't in a per-record stream.
    // clock_freq_hz drives the cycles→µs conversion (a2a3 = 50 MHz, a5 =
    // 1 GHz — must come from the host, not be hardcoded in python).
    outfile << "  \"metadata\": {\n";
    outfile << "    \"clock_freq_hz\": " << PLATFORM_PROF_SYS_CNT_FREQ << ",\n";
    outfile << "    \"num_cores\": " << num_aicore_ << ",\n";
    outfile << "    \"core_types\": [";
    for (int i = 0; i < num_aicore_; i++) {
        CoreType ct = (i < static_cast<int>(core_types_.size())) ? core_types_[i] : CoreType::AIV;
        if (i > 0) outfile << ", ";
        outfile << "\"" << ((ct == CoreType::AIC) ? "aic" : "aiv") << "\"";
    }
    outfile << "]\n  },\n";

    {
        outfile << "  \"aicore_tasks\": [";
        bool first = true;
        size_t total = 0;
        for (size_t core_idx = 0; core_idx < collected_aicore_records_.size(); core_idx++) {
            for (const auto &r : collected_aicore_records_[core_idx]) {
                if (!first) outfile << ",";
                outfile << "\n    [" << core_idx << ", " << r.task_token_raw << ", " << r.reg_task_id << ", "
                        << r.start_time << ", " << r.end_time << "]";
                first = false;
                total++;
            }
        }
        if (!first) outfile << "\n  ";
        outfile << "]";
        LOG_INFO_V0("  aicore_tasks: %zu records", total);
    }

    outfile << "\n}\n";
    outfile.close();

    if (!outfile) {
        LOG_ERROR("Failed to write JSON file (stream error): %s", filepath.c_str());
        return -1;
    }

    LOG_INFO_V0("=== JSON Export Complete ===");
    LOG_INFO_V0("File: %s", filepath.c_str());

    return 0;
}

int L2SwimlaneCollector::finalize(L2SwimlaneUnregisterCallback unregister_cb, const L2SwimlaneFreeCallback &free_cb) {
    if (shm_host_ == nullptr) {
        return 0;
    }

    // Stop mgmt + collector threads if the caller didn't already (idempotent).
    stop();

    LOG_DEBUG("Cleaning up performance profiling resources");

    // Every release site below goes through release_one_buffer so the
    // unregister and free are an inseparable pair — each dev_ptr that
    // alloc_single_buffer installed via halHostRegister is unregistered
    // before its device memory is freed. Without this the Ascend HAL's
    // per-device registration table accumulates leaked entries across
    // init_l2_swimlane() invocations and back-to-back l2_swimlane tests on
    // a reused Worker fail at rc=8 from halHostRegister.

    // Free standalone l2_swimlane_aicore_rotation_table table
    release_one_buffer(aicore_ring_addr_table_dev_, unregister_cb, free_cb);
    aicore_ring_addr_table_dev_ = nullptr;

    // Release framework-owned buffers (recycled pools, done_queue, ready_queue).
    manager_.release_owned_buffers([this, unregister_cb, free_cb](void *p) {
        release_one_buffer(p, unregister_cb, free_cb);
    });

    // Per-core AICore buffer pool cleanup.
    auto drain_free_queue = [&](L2SwimlaneFreeQueue &fq) {
        rmb();
        uint32_t head = fq.head;
        uint32_t tail = fq.tail;
        uint32_t queued = tail - head;
        if (queued > PLATFORM_PROF_SLOT_COUNT) {
            queued = PLATFORM_PROF_SLOT_COUNT;
        }
        for (uint32_t k = 0; k < queued; k++) {
            uint32_t slot = (head + k) % PLATFORM_PROF_SLOT_COUNT;
            release_one_buffer(reinterpret_cast<void *>(fq.buffer_ptrs[slot]), unregister_cb, free_cb);
            fq.buffer_ptrs[slot] = 0;
        }
        fq.head = tail;
    };

    for (int i = 0; i < num_aicore_; i++) {
        L2SwimlaneAicoreTaskPool *ac_state = get_aicore_buffer_state(shm_host_, num_aicore_, i);
        release_one_buffer(reinterpret_cast<void *>(ac_state->head.current_buf_ptr), unregister_cb, free_cb);
        ac_state->head.current_buf_ptr = 0;
        drain_free_queue(ac_state->free_queue);
    }

    // Main shm: unregister + free as a pair, same as every other buffer.
    // ProfilerBase's set_memory_context handed register_cb == nullptr iff the
    // caller doesn't intend to register, so checking unregister_cb inside
    // release_one_buffer is sufficient — no separate ``was_registered_`` flag.
    release_one_buffer(perf_shared_mem_dev_, unregister_cb, free_cb);
    LOG_DEBUG("Main shm released");

    perf_shared_mem_dev_ = nullptr;
    // shm_host_ aliases freed device/host memory now; null it so is_initialized()
    // reports false, the dtor's "destroyed without finalize()" warning stays
    // quiet, and a re-entrant finalize() / re-init hits the early-out instead of
    // walking freed buffer state. Mirrors PMU/DepGen/TensorDump collectors.
    shm_host_ = nullptr;
    collected_aicore_records_.clear();
    total_aicore_collected_ = 0;
    clear_memory_context();

    LOG_DEBUG("Performance profiling cleanup complete");
    return 0;
}

/* a2a3 SVM stubs — satisfy profiler_base symbol refs; unreachable at runtime. */
int profiling_copy_to_device(volatile void * /*dev_dst*/, const void * /*host_src*/, size_t /*size*/)
{
    return 0;
}

int profiling_copy_from_device(volatile void * /*host_dst*/, const volatile void * /*dev_src*/, size_t /*size*/)
{
    return 0;
}

std::function<int(void *, const void *, size_t)> profiling_copy_to_device_or_null()
{
    return {};
}

std::function<int(void *, const void *, size_t)> profiling_copy_from_device_or_null()
{
    return {};
}
