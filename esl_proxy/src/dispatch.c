/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "log.h"
#include "ring_buf.h"

#include <stdint.h>

#ifdef ESL_PROXY_ONBOARD
#include "aicpu_bridge.h"
static AicoreBridge *g_aicore_bridge;
void dispatch_set_aicore_bridge(void *bridge)
{
    g_aicore_bridge = (AicoreBridge *)bridge;
}
#endif

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static inline void get_free_exe(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
}

static inline void get_completed(uint64_t* bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[])
{
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)] = task_id_map[idx];
        WORKER_LOGF("completed,complete_cnt,%d,task_id,%u,core,%d,bitmap,%u",*complete_cnt, task_id_map[idx], idx, *bitmap);
        (*complete_cnt)++;
        cnt--;
        *bitmap &= (*bitmap - 1);
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
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    // Check both slots - slot is free if neither slot 0 nor slot 1 has been sent a task
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
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
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        // Set executor's tasks and duration
        int core = (int)idx;
        g_executors[exe_type][core].tasks[slot] = task_id;
        // Scale down duration for faster simulation (divide by 10000 to handle large durations)
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        g_executors[exe_type][core].duration[slot] = (raw_duration > 10000) ? (raw_duration / 10000) : 1;
        g_executors[exe_type][core].idx = slot;  // Point to the slot with the new task
        
        if (slot == 1) {
            ctrl->task_id_map2[type][idx] = task_id;
        } else {
            ctrl->task_id_map1[type][idx] = task_id;
        }
        
        ctrl->free_bitmap[type][slot] &= ~mask;

#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_dispatch_task(g_aicore_bridge, (int)ctrl->tid, task_id, core, slot,
                                        exe_type);
        } else {
            ctrl->msg_bitmap[type][slot] |= mask;
        }
#else
        /* Host sim: Fake Return */
        ctrl->msg_bitmap[type][slot] |= mask;
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
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_flush_after_dispatch();
#endif
    return total_sent;
}

/* Host-sim pthread path only (main.c). Onboard uses esl_singlethread_drive instead. */
void dispatch_loop_run(int tid)
{
    int total_sent = 0;
    uint64_t start_ns = get_time_ns();

#ifdef ESL_PROXY_ONBOARD
    esl_onboard_invalidate_shared_before_worker();
#endif
    while (!atomic_load(&g_orch_is_done)) {
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_invalidate_shared_before_worker();
#endif
        total_sent += dispatch(tid);
#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, tid);
        }
#endif
    }
    int prev = g_completed_cnt;
    int count = 10000;
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        esl_onboard_invalidate_shared_before_worker();
        total_sent += dispatch(tid);
#ifdef ESL_PROXY_ONBOARD
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, tid);
        }
#endif
        if (prev == g_completed_cnt) {
            count--;
            if (count < 0) {
                MAIN_LOGF("[scheduler] stall timeout: completed_cnt=%u task_id=%u",
                          (unsigned)g_completed_cnt, (unsigned)g_task_id);
                break;
            }
        } else {
            count = 10000;
        }
        prev = g_completed_cnt;
    }

    atomic_store(&g_is_done, true);
    (void)total_sent;
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s", (float)(g_completed_cnt * 1000.0 / elapsed_ns));
}

void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    dispatch_loop_run(tid);
    return NULL;
}

#ifdef ESL_PROXY_ONBOARD
/*
 * Single-threaded onboard driver: orchestration has already run; here we drive
 * the cutter (dependency resolution) and dispatch (send + HW completion poll)
 * from ONE AICPU thread. The AICPU cores are not cache-coherent and the
 * shared-queue spinlocks do not provide cross-core mutual exclusion, so running
 * cutter and dispatch on separate threads corrupts the ready/completed queues.
 * Single-threading removes all cross-core queue contention.
 */
void esl_singlethread_drive(void)
{
    extern void deal_completed_queue(void);
    extern int g_subtask_cnt;
    extern uint16_t g_commit_task_id;
    extern atomic_int g_min_uncomplete_task;
    extern void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt,
                                uint64_t commit, uint64_t ready_cube, uint64_t ready_vec,
                                uint64_t min_uncomplete);

    /* First pass: commit all tasks, build the dependency graph, ready roots. */
    deal_completed_queue();

    int prev = atomic_load(&g_completed_cnt);
    int stall = 0;
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        if (g_aicore_bridge != NULL) {
            aicore_bridge_poll_completions(g_aicore_bridge, 0);
        }
        dispatch(0);
        deal_completed_queue();

        int cc = atomic_load(&g_completed_cnt);
        if (cc == prev) {
            if (++stall > 2000000) {
                break;  /* avoid AICPU watchdog hang if a task never completes */
            }
        } else {
            stall = 0;
        }
        prev = cc;
    }
    atomic_store(&g_is_done, true);
    /* Diagnostic: find the first uncompleted task and whether it is ready
     * (predecessor_cnt==0 => readied-but-lost; >0 => predecessor never completed). */
    extern task_state *g_state_buf;
    extern uint16_t g_predecessor_cnt[];
    int end = atomic_load(&g_task_id);
    int first_uncomp = -1, second_uncomp = -1, n_uncomp = 0;
    for (int i = 0; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            if (first_uncomp < 0) first_uncomp = i;
            else if (second_uncomp < 0) second_uncomp = i;
            n_uncomp++;
        }
    }
    uint64_t pred0 = (first_uncomp >= 0) ? (uint64_t)g_predecessor_cnt[first_uncomp] : 0;
    uint64_t rqc = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE].cnt;
    uint64_t rqv = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_VECTOR].cnt;
    esl_write_stats((uint64_t)end, (uint64_t)g_subtask_cnt,
                    (uint64_t)atomic_load(&g_completed_cnt), (uint64_t)g_commit_task_id,
                    (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                    (rqc & 0xffffffffULL) | (rqv << 32));
}
#endif
