/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "cutter.h"
#include "executor.h"
#include "log.h"
#include "ring_buf.h"

#ifdef ESL_PROXY_ONBOARD
#include "spin.h"
#endif

#include <stdint.h>

#include <stdatomic.h>

#ifdef ESL_PROXY_ONBOARD
#include "aicpu_runtime.h"
#include "onboard_config.h"
#include "onboard/onboard_crosscore_sync.h"
#include "onboard/onboard_trace.h"
#include "onboard_log.h"
extern _Atomic uint16_t g_commit_task_id;
static AicoreBridge *g_aicore_bridge;
void dispatch_set_aicore_bridge(void *bridge)
{
    g_aicore_bridge = (AicoreBridge *)bridge;
}
#endif

static inline void get_free_exe(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            uint64_t msg =
                atomic_load_explicit(&g_ctrl_t[tid].msg_bitmap[i][j], memory_order_acquire);
            (void)atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[i][j], msg, memory_order_release);
#ifdef ESL_PROXY_ONBOARD
            esl_onboard_publish_atomic_u64(&g_ctrl_t[tid].free_bitmap[i][j]);
#endif
        }
    }
}

static inline void get_completed(_Atomic uint64_t *bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[])
{
    uint64_t b = atomic_exchange_explicit(bitmap, 0, memory_order_acq_rel);
    int cnt = __builtin_popcountll(b);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(b);
        task_id[(*complete_cnt)] = task_id_map[idx];
        WORKER_LOGF("completed,complete_cnt,%d,task_id,%u,core,%d,bitmap,%u", *complete_cnt,
                    task_id_map[idx], (int)idx, (unsigned)b);
        (*complete_cnt)++;
        cnt--;
        b &= (b - 1);
    }
}

// TODO: add counter for spmd
static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_publish_counters();
#endif
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    // Check both slots - slot is free if neither slot 0 nor slot 1 has been sent a task
    uint64_t free_bitmap =
        atomic_load_explicit(&ctrl->free_bitmap[type][0], memory_order_acquire) &
        atomic_load_explicit(&ctrl->free_bitmap[type][1], memory_order_acquire);
#ifdef ESL_PROXY_ONBOARD
    free_bitmap &= (uint64_t)((1ULL << ESL_PROXY_ONBOARD_BLOCK_DIM) - 1);
#endif
    int cnt = __builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    
    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);

        uint64_t mask = (uint64_t)0x1 << idx;
        // Determine which slot to use - prefer slot 0 if it's not busy
        uint64_t fb0 = atomic_load_explicit(&ctrl->free_bitmap[type][0], memory_order_acquire);
        int slot = (fb0 & mask) != 0 ? 0 : 1;
        // Set executor's tasks and duration
        int core = (int)idx;
        g_executors[exe_type][core].tasks[slot] = task_id;
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_consume_task_slot(task_id);
#endif
        // Scale down duration for faster simulation (divide by 10000 to handle large durations)
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].duration[slot] = (raw_duration > 10000) ? (raw_duration / 10000) : 1;
        g_executors[exe_type][core].idx = slot;  // Point to the slot with the new task
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        
        (void)atomic_fetch_and_explicit(&ctrl->free_bitmap[type][slot], ~mask, memory_order_release);
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_publish_atomic_u64(&ctrl->free_bitmap[type][slot]);
#endif

#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_dispatch_task(g_aicore_bridge, (int)ctrl->tid, task_id, core, slot,
                                        exe_type);
        } else {
            (void)atomic_fetch_or_explicit(&ctrl->msg_bitmap[type][slot], mask, memory_order_release);
        }
#else
        /* Host sim: Fake Return */
        (void)atomic_fetch_or_explicit(&ctrl->msg_bitmap[type][slot], mask, memory_order_release);
#endif
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_invalidate_sched_snapshot();
#endif
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/* Cutter / dispatch / orchestrator run on separate AICPU threads when onboard. */
void dispatch_loop_run(int tid)
{
    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_LOOP_ENTER, (uint64_t)tid, 0, 0);
    int total_sent = 0;
    uint64_t start_ns = get_time_ns();
    uint32_t phase1_iter = 0;
    uint32_t phase2_iter = 0;

    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE1, start_ns, 0, 0);
    while (!atomic_load(&g_orch_is_done)) {
#ifdef ESL_PROXY_ONBOARD
        if ((phase1_iter & 0x3FFFFU) == 0) {
            esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE1, phase1_iter,
                              (uint64_t)atomic_load_explicit(&g_completed_cnt, memory_order_acquire),
                              (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire));
        }
        phase1_iter++;
#endif
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_invalidate_sched_snapshot();
#endif
        total_sent += dispatch(tid);
#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, tid);
        }
        spin_wait();
#endif
    }
    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE2,
                      (uint64_t)atomic_load_explicit(&g_completed_cnt, memory_order_acquire),
                      (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire), 0);
    int prev = atomic_load_explicit(&g_completed_cnt, memory_order_acquire);
    int count = 10000;
    while (atomic_load_explicit(&g_completed_cnt, memory_order_acquire) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
#ifdef ESL_PROXY_ONBOARD
        if ((phase2_iter & 0x3FFFFU) == 0) {
            esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE2, phase2_iter,
                              (uint64_t)prev, (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire));
        }
        phase2_iter++;
#endif
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_invalidate_sched_snapshot();
#endif
        total_sent += dispatch(tid);
#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, tid);
        }
        spin_wait();
#endif
        if (prev == atomic_load_explicit(&g_completed_cnt, memory_order_acquire)) {
            count--;
            if (count < 0) {
                LOG_ERROR("[scheduler] stall timeout: completed_cnt=%u task_id=%u",
                          (unsigned)g_completed_cnt, (unsigned)g_task_id);
                esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_STALL,
                                  (uint64_t)g_completed_cnt, (uint64_t)g_task_id, (uint64_t)prev);
                break;
            }
        } else {
            count = 10000;
        }
        prev = atomic_load_explicit(&g_completed_cnt, memory_order_acquire);
    }

    atomic_store(&g_is_done, true);
    (void)total_sent;
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", (unsigned)atomic_load_explicit(&g_completed_cnt, memory_order_acquire));
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",
              (float)(atomic_load_explicit(&g_completed_cnt, memory_order_acquire) * 1000.0 / elapsed_ns));

#ifdef ESL_PROXY_ONBOARD
    {
        extern task_state *g_state_buf;
        extern uint16_t g_predecessor_cnt[];
        extern int g_subtask_cnt;
        int end = atomic_load_explicit(&g_task_id, memory_order_acquire);
        int first_uncomp = -1;
        int n_uncomp = 0;
        for (int i = 0; i < end; i++) {
            if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
                if (first_uncomp < 0) {
                    first_uncomp = i;
                }
                n_uncomp++;
            }
        }
        uint64_t pred0 = (first_uncomp >= 0) ? (uint64_t)g_predecessor_cnt[first_uncomp] : 0;
        uint64_t rqc = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE].cnt;
        uint64_t rqv = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_VECTOR].cnt;
        esl_write_stats((uint64_t)end, (uint64_t)g_subtask_cnt,
                        (uint64_t)atomic_load_explicit(&g_completed_cnt, memory_order_acquire),
                        (uint64_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire),
                        (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                        (rqc & 0xffffffffULL) | (rqv << 32));
    }
#endif
}

void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    dispatch_loop_run(tid);
    return NULL;
}
