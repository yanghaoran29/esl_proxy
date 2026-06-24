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
 * @file l2_swimlane_collector_aicpu.h
 * @brief AICPU performance data collection interface
 *
 * Provides performance profiling management interface for AICPU side.
 * Handles buffer initialization, switching, and flushing.
 */

#ifndef PLATFORM_AICPU_L2_SWIMLANE_COLLECTOR_AICPU_H_
#define PLATFORM_AICPU_L2_SWIMLANE_COLLECTOR_AICPU_H_

#include "common/l2_swimlane_profiling.h"

// Include platform-specific timestamp implementation
// Build system selects the correct inner_aicpu.h based on platform:
// Both provide unified get_sys_cnt_aicpu() interface
#include "aicpu/device_time.h"

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base);
uint64_t get_platform_l2_swimlane_base(void);
void set_l2_swimlane_enabled(bool enable);
bool is_l2_swimlane_enabled(void);
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr);
uint64_t get_platform_l2_swimlane_aicore_rotation_table(void);
void l2_swimlane_aicpu_init(int worker_count);
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx);
int l2_swimlane_aicpu_complete_task(
    int core_id, int thread_idx, uint32_t reg_task_id, uint64_t dispatch_time, uint64_t finish_time);
void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num);
/* C-callable: the orchestrator thread (C: aicpu_runtime.c) sets its index here. */
void l2_swimlane_aicpu_set_orch_thread_idx(int thread_idx);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

L2SwimlaneLevel get_l2_swimlane_level(void);

/**
 * Initialize AICPU phase profiling
 *
 * Writes phase metadata (num_sched_phase_threads, num_orch_phase_threads,
 * num_phase_cores, core_to_thread[]) into L2SwimlaneDataHeader and primes
 * per-thread sched and orch phase pools. Must be called once from thread 0
 * after l2_swimlane_aicpu_init().
 *
 * @param worker_count             Number of AICore workers (cores) — used to
 *                                 resolve the phase region's offset relative
 *                                 to the L2Swimlane base
 * @param num_sched_phase_threads  Number of sched-phase pools to prime
 * @param num_orch_phase_threads   Number of orch-phase pools to prime
 *                                 (typically 1; in orch_to_sched mode =
 *                                 num_aicpu_threads)
 */
void l2_swimlane_aicpu_init_phase(int worker_count, int num_sched_phase_threads, int num_orch_phase_threads);

/**
 * Record a single scheduler phase
 *
 * Appends an L2SwimlaneAicpuSchedPhaseRecord to the specified thread's sched
 * pool. Silently drops records when the buffer is full or the pool was not
 * primed (init failed for this thread).
 *
 * Queue-depth snapshots distinguish "task hidden in T0's local_buf" from
 * "shared queue has it but peers spin on the wrong shape" — the former shows
 * `local_depth > 0, shared_depth == 0` for the owning thread while peers see
 * `shared_depth == 0` until overflow. Pass nullptr for any of the four arrays
 * when not capturing (the record's corresponding slot is zero-filled).
 *
 * @param thread_idx       Scheduler thread index
 * @param kind             Complete or Dispatch
 * @param start_time       Phase start timestamp
 * @param end_time         Phase end timestamp
 * @param loop_iter        Current scheduler-loop iteration number
 * @param tasks_processed  Tasks processed in this phase batch
 * @param pop_hit          Dispatch delta since last emit (0 for Complete)
 * @param pop_miss         Dispatch delta since last emit (0 for Complete)
 * @param local_at_start   Per-shape PTO2LocalReadyBuffer.count at phase start (size L2SWIMLANE_NUM_QUEUE_SHAPES; may be
 * nullptr)
 * @param shared_at_start  Per-shape sched.ready_queues[shape].size() at phase start (may be nullptr)
 * @param local_at_end     Per-shape PTO2LocalReadyBuffer.count at phase end (may be nullptr)
 * @param shared_at_end    Per-shape sched.ready_queues[shape].size() at phase end (may be nullptr)
 */
void l2_swimlane_aicpu_record_sched_phase(
    int thread_idx, L2SwimlaneSchedPhaseKind kind, uint64_t start_time, uint64_t end_time, uint32_t loop_iter,
    uint32_t tasks_processed, uint32_t pop_hit = 0, uint32_t pop_miss = 0, const int16_t *local_at_start = nullptr,
    const int16_t *shared_at_start = nullptr, const int16_t *local_at_end = nullptr,
    const int16_t *shared_at_end = nullptr
);

/**
 * Set orchestrator thread index for per-task phase recording
 *
 * Must be called once from the orchestrator thread before any
 * l2_swimlane_aicpu_record_orch_phase() calls.
 *
 * @param thread_idx Thread index for the orchestrator (typically num_sched_threads;
 *                   in orch_to_sched mode each scheduler thread sets its own)
 *
 * (Declared in the extern "C" block above — C-callable from aicpu_runtime.c.)
 */

/**
 * Record one orchestrator submit envelope
 *
 * Appends an L2SwimlaneAicpuOrchPhaseRecord covering an entire submit_task()
 * / alloc_tensors() call. Uses the orchestrator's dedicated orch-phase pool
 * (chosen via set_orch_thread_idx).
 *
 * @param start_time  Submit start timestamp
 * @param end_time    Submit end timestamp
 * @param task_id     Task identifier. For tensormap_and_ringbuffer, full PTO2
 *                    encoding: (ring_id << 32) | local_id, enabling
 *                    cross-view correlation between orchestrator and
 *                    scheduler swimlanes.
 * @param submit_idx  Monotonic submit counter
 */
void l2_swimlane_aicpu_record_orch_phase(uint64_t start_time, uint64_t end_time, uint64_t task_id, uint32_t submit_idx);

/**
 * Write core-to-thread assignment mapping to shared memory.
 *
 * Callers invoke `l2_swimlane_aicpu_init_core_assignments(total_cores)` once, then
 * `l2_swimlane_aicpu_write_core_assignments_for_thread(t, ids, n)` for every
 * scheduler thread.
 */
void l2_swimlane_aicpu_init_core_assignments(int total_cores);
void l2_swimlane_aicpu_write_core_assignments_for_thread(int thread_idx, const int *core_ids, int core_num);

/**
 * Flush the remaining scheduler-phase records for a scheduler thread.
 *
 * Marks the thread's current WRITING sched-phase buffer as READY and enqueues
 * it for host collection. Called at scheduler-thread exit.
 *
 * @param thread_idx Scheduler thread index (= sched pool index = ready queue)
 */
void l2_swimlane_aicpu_flush_sched_phase_buffer(int thread_idx);

/**
 * Flush the remaining orchestrator-phase records (single orch instance, pool
 * ordinal 0). Called once by the orchestrator thread at orchestration end.
 *
 * @param thread_idx Calling (orchestrator) AICPU thread index — selects the
 *                   ready queue to enqueue into. The pool/lane tag is ordinal 0.
 */
void l2_swimlane_aicpu_flush_orch_phase_buffer(int thread_idx);

#endif /* __cplusplus */

#endif  // PLATFORM_AICPU_L2_SWIMLANE_COLLECTOR_AICPU_H_
