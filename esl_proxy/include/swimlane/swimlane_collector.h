/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */

/**
 * @file swimlane_collector.h
 * @brief Platform-agnostic performance data collector with dynamic memory management.
 *
 * Architecture:
 * - BufferPoolManager<L2SwimlaneModule>: shared mgmt-thread infrastructure that polls
 *   the AICPU ready queue, replenishes per-core / per-thread free queues, and
 *   hands full buffers off to the collector thread.
 * - L2SwimlaneCollector: main thread copies records from the manager's ready queue
 *   into host vectors and exports the swimlane visualization.
 *
 * Memory operations are injected through callbacks for sim/onboard portability.
 */

#ifndef SRC_A2A3_PLATFORM_INCLUDE_HOST_SWIMLANE_COLLECTOR_H_
#define SRC_A2A3_PLATFORM_INCLUDE_HOST_SWIMLANE_COLLECTOR_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core_type.h"
#include "swimlane_types.h"
#include "memory_barrier.h"
#include "onboard_log.h"
#include "swimlane_collector_base.h"

// ---------------------------------------------------------------------------
// L2 Perf profiling Module (drives BufferPoolManager<L2SwimlaneModule>)
// ---------------------------------------------------------------------------

/** Level 1 only: per-core L2SwimlaneAicoreTaskBuffer records via ready queue. */

enum class ProfBufferType {
    AICORE_TASK = 0,
};

/**
 * Information about a ready (full) buffer, passed from mgmt thread to main thread.
 */
struct ReadyBufferInfo {
    ProfBufferType type;
    uint32_t index;         // core_index (task) or thread_idx (phase)
    uint32_t slot_idx;      // Reserved (unused in free queue design)
    void *dev_buffer_ptr;   // Device address of the full buffer
    void *host_buffer_ptr;  // Host-mapped address (sim: same as dev)
    uint32_t buffer_seq;    // Sequence number for ordering
};

struct L2SwimlaneModule {
    using DataHeader = L2SwimlaneDataHeader;
    using ReadyEntry = ReadyQueueEntry;
    using ReadyBufferInfo = ::ReadyBufferInfo;
    using FreeQueue = L2SwimlaneFreeQueue;

    static constexpr int kBufferKinds = 1;
    static constexpr uint32_t kReadyQueueSize = PLATFORM_PROF_READYQUEUE_SIZE;
    static constexpr uint32_t kSlotCount = PLATFORM_PROF_SLOT_COUNT;
    static constexpr const char *kSubsystemName = "L2SwimlaneModule";

    static constexpr int batch_size(int /*kind*/) {
        constexpr int kAicoreBatch = PLATFORM_AICORE_BUFFERS_PER_CORE - PLATFORM_PROF_SLOT_COUNT;
        return kAicoreBatch < 1 ? 1 : kAicoreBatch;
    }

    static int kind_of(const ReadyBufferInfo &info) { return static_cast<int>(info.type); }

    static DataHeader *header_from_shm(void *shm) { return get_l2_swimlane_header(shm); }

    static std::optional<profiling_common::EntrySite<L2SwimlaneModule>>
    resolve_entry(void *shm, DataHeader *header, int /*q*/, const ReadyEntry &entry) {
        const int num_cores = static_cast<int>(header->num_cores);
        if (entry.kind != L2SwimlaneBufferKind::AicoreTask) {
            LOG_ERROR("L2SwimlaneModule: invalid entry kind=%u", static_cast<uint32_t>(entry.kind));
            return std::nullopt;
        }
        if (entry.core_index >= static_cast<uint32_t>(num_cores)) {
            LOG_ERROR("L2SwimlaneModule: invalid AICore entry: core=%u", entry.core_index);
            return std::nullopt;
        }

        profiling_common::EntrySite<L2SwimlaneModule> site;
        site.kind = static_cast<int>(L2SwimlaneBufferKind::AicoreTask);
        site.info.index = entry.core_index;
        site.info.slot_idx = 0;
        site.info.dev_buffer_ptr = reinterpret_cast<void *>(entry.buffer_ptr);
        site.info.host_buffer_ptr = nullptr;
        site.info.buffer_seq = entry.buffer_seq;

        auto *ac_state = get_aicore_buffer_state(shm, num_cores, static_cast<int>(entry.core_index));
        site.free_queue = &ac_state->free_queue;
        site.buffer_size = sizeof(L2SwimlaneAicoreTaskBuffer);
        site.info.type = ProfBufferType::AICORE_TASK;
        return site;
    }

    template <typename Cb>
    static void for_each_instance(void *shm, DataHeader *header, Cb &&cb) {
        const int num_cores = static_cast<int>(header->num_cores);
        for (int i = 0; i < num_cores; i++) {
            auto *ac_state = get_aicore_buffer_state(shm, num_cores, i);
            cb(static_cast<int>(L2SwimlaneBufferKind::AicoreTask), &ac_state->free_queue,
               sizeof(L2SwimlaneAicoreTaskBuffer));
        }
    }
};

// Memory callbacks — thin aliases for the canonical profiling_common shapes.
// alloc / free are std::function so callers bind their MemoryAllocator via
// lambda capture; register / unregister stay as plain function pointers
// because they wrap stateless HAL globals (halHost*).
using L2SwimlaneAllocCallback = profiling_common::ProfAllocCallback;
using L2SwimlaneRegisterCallback = profiling_common::ProfRegisterCallback;
using L2SwimlaneUnregisterCallback = profiling_common::ProfUnregisterCallback;
using L2SwimlaneFreeCallback = profiling_common::ProfFreeCallback;

// =============================================================================
// L2SwimlaneCollector
// =============================================================================

/**
 * Performance data collector.
 *
 * Lifecycle:
 *   1. initialize()                — allocate shared memory, pre-fill free_queues,
 *                                    hand the memory context to the base via
 *                                    set_memory_context().
 *   2. start(tf)                   — inherited from ProfilerBase: assembles a
 *                                    MemoryOps from the stashed callbacks and
 *                                    launches the mgmt + poll threads.
 *   3. ... device execution ...
 *   4. stop()                      — joins both threads in the correct order
 *                                    (mgmt first so its final-drain entries
 *                                    have a consumer).
 *   5. reconcile_counters()        — device-side accounting for AICore pools.
 *   6. export_swimlane_json() / finalize().
 *
 * Host never reads from device-side `current_buf_ptr` to recover records:
 * device flush is the only data path. Any non-zero `current_buf_ptr` after
 * stop() is logged as a bug.
 */
class L2SwimlaneCollector : public profiling_common::ProfilerBase<L2SwimlaneCollector, L2SwimlaneModule> {
public:
    L2SwimlaneCollector() = default;
    ~L2SwimlaneCollector();

    L2SwimlaneCollector(const L2SwimlaneCollector &) = delete;
    L2SwimlaneCollector &operator=(const L2SwimlaneCollector &) = delete;

    // ProfilerBase contract
    static constexpr int kIdleTimeoutSec = PLATFORM_PROF_TIMEOUT_SECONDS;
    static constexpr const char *kSubsystemName = "L2Swimlane";

    /**
     * Initialize performance profiling.
     *
     * Allocates the shared-memory region (header + per-core / per-thread
     * BufferStates), pre-allocates initial L2SwimlaneAicoreTaskBuffers,
     * and seeds the per-pool free_queues + the framework's recycled pools.
     *
     * @param num_aicore               Number of AICore instances
     * @param device_id                Device ID (forwarded to register_cb)
     * @param l2_swimlane_level   DISABLED (0) or AICORE_TIMING (1). Values >1 are
     *                                 clamped to 1 by the host wrapper.
     * @param alloc_cb                 Device memory allocation callback
     * @param register_cb              Memory registration callback (nullptr for
     *                                 simulation)
     * @param free_cb                  Device memory free callback
     * @param user_data                Opaque pointer forwarded to callbacks
     * @param output_prefix            Per-task directory; l2_swimlane_records.json
     *                                 lands here. Required (non-empty);
     *                                 CallConfig::validate() enforces this
     *                                 upstream.
     * @return 0 on success, error code on failure
     */
    int initialize(
        int num_aicore, int aicpu_thread_num, int device_id, L2SwimlaneLevel l2_swimlane_level,
        const L2SwimlaneAllocCallback &alloc_cb, L2SwimlaneRegisterCallback register_cb,
        const L2SwimlaneFreeCallback &free_cb, const std::string &output_prefix
    );

    /**
     * Per-buffer callback invoked by ProfilerBase's poll loop. Dispatches on
     * info.type to copy an L2SwimlaneAicoreTaskBuffer into the per-core record vector.
     */
    void on_buffer_collected(const ReadyBufferInfo &info);

    /**
     * Publish per-core core_type (AIC/AIV/...) so the host emit path can
     * resolve the lane label without consulting an AICPU task record. Required
     * for AICORE_TIMING (level=1) where complete_task is bypassed and the
     * AICore record alone is on disk. Caller is the device_runner — sim sets
     * it from `runtime.workers[i].core_type` (rule-based), onboard sets it
     * from the handshake-discovered table.
     *
     * Safe to call multiple times; the last call wins.
     *
     * @param types  CoreType[n] table indexed by core_id
     * @param n      table length (typically `num_aicore`)
     */
    void set_core_types(const CoreType *types, int n);

    /**
     * Export collected records as a Chrome Trace Event JSON (swimlane view).
     * Writes <output_prefix>/l2_swimlane_records.json — directory is captured at
     * initialize() time.
     *
     * @return 0 on success, error code on failure
     */
    int export_swimlane_json();

    /**
     * Free all device memory and unregister mappings. Idempotent on a
     * collector that was never initialized.
     *
     * @param unregister_cb  Memory unregister callback (nullptr in sim mode)
     * @param free_cb        Memory free callback
     * @param user_data      Opaque pointer forwarded to callbacks
     * @return 0 on success, error code on failure
     */
    int finalize(L2SwimlaneUnregisterCallback unregister_cb, const L2SwimlaneFreeCallback &free_cb);

    /**
     * @return true if initialize() succeeded and finalize() has not run.
     */
    bool is_initialized() const { return shm_host_ != nullptr; }

    /**
     * Device pointer to the L2SwimlaneDataHeader. Set kernel_args.l2_swimlane_data_base
     * to this after initialize() succeeds so the AICPU side can find the
     * shared memory.
     */
    void *get_l2_swimlane_setup_device_ptr() const { return perf_shared_mem_dev_; }

    /**
     * Device pointer to a uint64_t[num_aicore] table where each entry will
     * hold this core's `&L2SwimlaneAicoreTaskPool::rotation` device address. Host
     * only allocates the bytes here; AICPU populates the entries inside
     * `l2_swimlane_aicpu_init`. Freed by finalize(). Set kernel_args.l2_swimlane_aicore_rotation_table
     * to this so the AICore kernel entry can index by block_idx and feed the
     * per-core rotation channel into `set_l2_swimlane_aicore_head_slot()`. Returns
     * nullptr before initialize() succeeds.
     */
    void *get_aicore_ring_addr_table_device_ptr() const { return aicore_ring_addr_table_dev_; }

    /**
     * Sum per-core AICore pool counters, cross-check
     * `collected + dropped == device_total`, and LOG_ERROR any non-zero
     * current_buf_ptr after stop(). Must be called after stop().
     */
    void reconcile_counters();

private:
    // Shared memory pointers. shm_host_ / device_id_ live on ProfilerBase
    // (set via set_memory_context in initialize()).
    void *perf_shared_mem_dev_{nullptr};

    // Standalone uint64_t[num_aicore] table holding per-core L2SwimlaneAicoreTaskBuffer
    // addresses. Allocated in initialize(), freed in finalize(). AICore reads
    // ring_table[block_idx] via KernelArgs::l2_swimlane_aicore_rotation_table.
    void *aicore_ring_addr_table_dev_{nullptr};

    int num_aicore_{0};
    // Total AICPU threads launched this run. The dedicated orchestrator runs on
    // the last one (aicpu_thread_num_ - 1); used to report its thread number in
    // the phase-metadata log (orch-phase is a single pool, so its index alone
    // does not encode the AICPU thread).
    int aicpu_thread_num_{0};
    L2SwimlaneLevel l2_swimlane_level_{L2SwimlaneLevel::DISABLED};

    // Per-core core_type table populated by set_core_types(). Indexed by
    // core_id; size matches num_aicore_ once populated. Used by the level=1
    // emit path which has no AICPU record to read core_type from.
    std::vector<CoreType> core_types_;

    // Per-task output directory captured at initialize() time. Consumed by
    // export_swimlane_json() to build <prefix>/l2_swimlane_records.json.
    std::string output_prefix_;

    // Collected AICore records (per-core vectors). Each entry is a full
    // L2SwimlaneAicoreTaskRecord captured from a rotated L2SwimlaneAicoreTaskBuffer. The
    // order across rotations is preserved by `copy_aicore_buffer` (we sort
    // incoming buffers by buffer_seq before flattening).
    std::vector<std::vector<L2SwimlaneAicoreTaskRecord>> collected_aicore_records_;
    uint64_t total_aicore_collected_{0};

    // Running totals used at reconcile time to cross-check device-side counters.
    // Allocate a single buffer (any of the L2SwimlaneAicpu*Buffer kinds) and register it.
    // The RAII counterpart ``release_one_buffer`` lives on ProfilerBase and
    // is shared with every other collector.
    void *alloc_single_buffer(size_t size, void **host_ptr_out);

    // Per-buffer-kind handlers used by on_buffer_collected.
    void copy_aicore_buffer(const ReadyBufferInfo &info);
};

#endif  // SRC_A2A3_PLATFORM_INCLUDE_HOST_SWIMLANE_COLLECTOR_H_
