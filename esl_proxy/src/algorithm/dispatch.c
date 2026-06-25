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
#include "spin.h"

#include "platform.h"

#include <stdbool.h>
#include <stdint.h>

#include <stdatomic.h>

extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;

#ifdef ESL_PROXY_ONBOARD
#include "aicpu_runtime.h"
#include "onboard_config.h"
#include "onboard_trace.h"
#include "onboard_log.h"
extern uint16_t g_commit_task_id;
static AicoreBridge *g_aicore_bridge;
void dispatch_set_aicore_bridge(void *bridge)
{
    g_aicore_bridge = (AicoreBridge *)bridge;
}
#endif

/* Single-task dispatch: one FIN completes one task (pre-SPMD semantics). */
int g_completed_subtask_cnt = 0;

static inline void get_free_exe(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            uint64_t msg =
                g_ctrl_t[tid].msg_bitmap[i][j];
            g_ctrl_t[tid].free_bitmap[i][j] |= msg;
#ifdef ESL_PROXY_ONBOARD
            esl_onboard_publish_atomic_u64(&g_ctrl_t[tid].free_bitmap[i][j]);
#endif
        }
    }
}

static inline void get_completed(uint64_t *bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[], int exe_type, int ostd_slot)
{
    uint64_t b = *bitmap;
    *bitmap = 0;
    int cnt = __builtin_popcountll(b);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(b);
        uint16_t tid = task_id_map[idx];
        task_id[(*complete_cnt)] = tid;
        (*complete_cnt)++;
#ifndef ESL_PROXY_ONBOARD
        g_ctrl_t[0].free_bitmap[exe_type][ostd_slot] |= (uint64_t)0x1 << idx;
        g_executors[exe_type][(int)idx].tasks[ostd_slot] = EXEC_SLOT_EMPTY;
        if (g_executors[exe_type][(int)idx].idx == (uint8_t)ostd_slot) {
            g_executors[exe_type][(int)idx].idx = (uint8_t)AIC_OSTD;
        }
#endif
        WORKER_LOGF("completed,complete_cnt,%d,task_id,%u,core,%d", *complete_cnt, tid, (int)idx);
        cnt--;
        b &= (b - 1);
    }
}

static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = 0;
#ifndef ESL_PROXY_ONBOARD
    PlatformCompletion fin;
    while (platform_pop_completion(&fin) == 0) {
        task_id[complete_cnt++] = fin.task_id;
        g_ctrl_t[0].free_bitmap[(int)fin.exe_type][(int)fin.slot] |= fin.mask;
        g_executors[(int)fin.exe_type][(int)fin.core].tasks[(int)fin.slot] = EXEC_SLOT_EMPTY;
        if (g_executors[(int)fin.exe_type][(int)fin.core].idx == (uint8_t)fin.slot) {
            g_executors[(int)fin.exe_type][(int)fin.core].idx = (uint8_t)AIC_OSTD;
        }
    }
#else
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i], i, 0);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i], i, 1);
    }
#endif
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
    g_completed_subtask_cnt += complete_cnt;
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_publish_counters();
#endif
}

// TODO: Work Stealing
/* Dispatch one block (subtask) of `task_id` to (core, slot). Returns 0 on success. */
static inline int send_block(ctrl_t *ctrl, int type, int exe_type, uint16_t task_id, int core,
                             int slot, uint64_t mask)
{
    g_executors[exe_type][core].tasks[slot] = task_id;
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_consume_task_slot(task_id);
#endif
    uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
    uint32_t jitter_mask = g_basic_buf[task_id & RING_MASK].jitter_mask;
#ifdef ESL_PROXY_ONBOARD
    g_executors[exe_type][core].duration[slot] = (raw_duration > 10000) ? (raw_duration / 10000) : 1;
#else
    g_executors[exe_type][core].duration[slot] = raw_duration;
#endif
    g_executors[exe_type][core].idx = slot;

    if (slot == 1) {
        ctrl->task_id_map2[type][core] = task_id;
    } else {
        ctrl->task_id_map1[type][core] = task_id;
    }

    ctrl->free_bitmap[type][slot] &= ~mask;
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_publish_atomic_u64(&ctrl->free_bitmap[type][slot]);
    if (g_aicore_bridge != NULL) {
        aicore_bridge_dispatch_task(g_aicore_bridge, (int)ctrl->tid, task_id, core, slot, exe_type,
                                    0);
    } else {
        ctrl->msg_bitmap[type][slot] |= mask;
    }
    return 0;
#else
    {
        uint16_t phys = 0;
        if (platform_dispatch_block(task_id, exe_type, core, slot, mask, raw_duration, jitter_mask,
                                    &phys) != 0) {
            ctrl->free_bitmap[type][slot] |= mask;
            return -1;
        }
        if (platform_fake_kernel_enabled()) {
            g_executors[exe_type][core].block_idx[slot] = phys;
        }
    }
    return 0;
#endif
}

static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    uint64_t free_bitmap =
        ctrl->free_bitmap[type][0] &
        ctrl->free_bitmap[type][1];
#if defined(ESL_PROXY_ONBOARD)
    free_bitmap &= (uint64_t)((1ULL << ESL_PROXY_ONBOARD_BLOCK_DIM) - 1);
#else
    free_bitmap &= (uint64_t)((1ULL << PLATFORM_WORKER_BLOCK_DIM) - 1);
#endif
    uint16_t cnt = (uint16_t)__builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)) {
        return 0;
    }

    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        if (send_block(ctrl, type, exe_type, task_id, (int)idx, slot, mask) != 0) {
            uint16_t one = task_id;
            batch_enqueue(&ctrl->ready_queue[type], &one, 1);
            break;
        }
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, (int)idx, slot, type);
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
    int total_sent = 0;
    uint64_t start_ns = get_time_ns();
#ifdef ESL_PROXY_ONBOARD
    uint32_t phase1_iter = 0;
    uint32_t phase2_iter = 0;

    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_LOOP_ENTER, (uint64_t)tid, 0, 0);
    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE1, start_ns, 0, 0);
#endif
    while (!atomic_load(&g_orch_is_done)) {
#ifdef ESL_PROXY_ONBOARD
        if ((phase1_iter & 0x3FFFFU) == 0) {
            esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE1, phase1_iter,
                              (uint64_t)g_completed_cnt,
                              (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire));
        }
        phase1_iter++;
        esl_onboard_invalidate_sched_snapshot();
#endif
        total_sent += dispatch(tid);
#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, tid);
        }
        spin_wait();
#else
        spin_wait();
#endif
    }
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_PHASE2,
                      (uint64_t)g_completed_cnt,
                      (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire), 0);
#endif
    int prev = g_completed_cnt;
    int count = 10000;
#ifndef ESL_PROXY_ONBOARD
    count = 50000000;
#endif
    while (atomic_load(&g_completed_cnt) <
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
#else
        spin_wait();
#endif
        if (prev == g_completed_cnt) {
            count--;
            if (count < 0) {
#ifdef ESL_PROXY_ONBOARD
                LOG_ERROR("[scheduler] stall timeout: completed_cnt=%u task_id=%u",
                          (unsigned)g_completed_cnt, (unsigned)g_task_id);
                esl_onboard_trace(ESL_AICPU_ROLE_DISPATCH, ESL_TRACE_DISPATCH_STALL,
                                  (uint64_t)g_completed_cnt, (uint64_t)g_task_id, (uint64_t)prev);
#else
                MAIN_LOGF("[scheduler] stall timeout: completed_cnt=%u task_id=%u",
                          (unsigned)g_completed_cnt, (unsigned)g_task_id);
#endif
                break;
            }
        } else {
            count = 10000;
#ifndef ESL_PROXY_ONBOARD
            count = 50000000;
#endif
        }
        prev = g_completed_cnt;
    }

    atomic_store(&g_is_done, true);
    (void)total_sent;
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", (unsigned)g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",
              (float)(g_completed_cnt * 1000.0 / elapsed_ns));

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
        /* stats[4] packs commit (low32) | completed_subtask_cnt (high32) so the
         * host can verify both task-count and subtask-count match. */
        esl_write_stats((uint64_t)end, (uint64_t)g_subtask_cnt,
                        (uint64_t)g_completed_cnt,
                        ((uint64_t)(uint32_t)g_commit_task_id) |
                            ((uint64_t)(uint32_t)g_completed_subtask_cnt << 32),
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
