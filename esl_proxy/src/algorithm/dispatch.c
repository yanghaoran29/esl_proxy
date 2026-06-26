/*
 * dispatch.c - Dispatch Worker Thread Implementation
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

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];

int g_completed_subtask_cnt = 0;

static AicoreBridge *g_bridge;

static inline void drain_completed_bitmap(uint64_t *bitmap, uint16_t task_ids[], int *complete_cnt,
                                          const uint16_t task_id_map[], int max_cnt)
{
    uint64_t b = *bitmap;
    int cnt;

    *bitmap = 0;
    cnt = __builtin_popcountll(b);
    while (cnt > 0 && *complete_cnt < max_cnt) {
        uint64_t idx = (uint64_t)__builtin_ctzll(b);
        uint16_t tid = task_id_map[idx];

        task_ids[(*complete_cnt)++] = tid;
        WORKER_LOGF("completed,complete_cnt,%d,task_id,%u,core,%d", *complete_cnt, tid, (int)idx);
        cnt--;
        b &= (b - 1);
    }
}

static inline void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

static inline void get_free_exe(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
            platform_publish_atomic_u64(&g_ctrl_t[tid].free_bitmap[i][j]);
        }
    }
    set_mix(tid);
}

void dispatch_bind(void *bridge)
{
    g_bridge = (AicoreBridge *)bridge;
}

void dispatch_tick_begin(int tid)
{
    (void)tid;
    platform_invalidate_sched_snapshot();
}

void dispatch_poll(int tid)
{
    if (g_bridge != NULL) {
        aicore_bridge_poll_completions(g_bridge, tid);
    }
}

int dispatch_submit(ctrl_t *ctrl, int type, int exe_type, uint16_t task_id, int core, int slot,
                    uint64_t mask, uint32_t raw_duration, uint32_t jitter_mask)
{
    (void)raw_duration;
    (void)jitter_mask;

    platform_consume_task_slot(task_id);
    g_executors[exe_type][core].duration[slot] =
        dispatch_executor_duration(g_basic_buf[task_id & RING_MASK].duration);
    g_executors[exe_type][core].idx = (uint8_t)slot;

    if (slot == 1) {
        ctrl->task_id_map2[type][core] = task_id;
    } else {
        ctrl->task_id_map1[type][core] = task_id;
    }

    ctrl->free_bitmap[type][slot] &= ~mask;
    platform_publish_atomic_u64(&ctrl->free_bitmap[type][slot]);

    if (g_bridge != NULL) {
        return aicore_bridge_dispatch_task(g_bridge, (int)ctrl->tid, task_id, core, slot, exe_type,
                                           0);
    }
    ctrl->msg_bitmap[type][slot] |= mask;
    return 0;
}

void dispatch_drain_completions(int tid, uint16_t *task_ids, int *complete_cnt, int max_cnt)
{
    for (int i = 0; i < EXE_TYPE_CNT && *complete_cnt < max_cnt; i++) {
        drain_completed_bitmap(&g_ctrl_t[tid].msg_bitmap[i][0], task_ids, complete_cnt,
                               g_ctrl_t[tid].task_id_map1[i], max_cnt);
        drain_completed_bitmap(&g_ctrl_t[tid].msg_bitmap[i][1], task_ids, complete_cnt,
                               g_ctrl_t[tid].task_id_map2[i], max_cnt);
    }
}

void dispatch_after_push_completed(int tid, int complete_cnt)
{
    (void)tid;
    (void)complete_cnt;
    platform_publish_counters();
}

uint32_t dispatch_executor_duration(uint32_t raw_duration)
{
    return (raw_duration > 10000U) ? (raw_duration / 10000U) : 1U;
}

int dispatch_stall_limit(void)
{
    return 10000;
}

static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = 0;

    dispatch_drain_completions(tid, task_id, &complete_cnt, DISPATCH_COMPLETE_BATCH);
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
    g_completed_subtask_cnt += complete_cnt;
    dispatch_after_push_completed(tid, complete_cnt);
}

static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    uint64_t block_mask = (uint64_t)((1ULL << platform_worker_block_dim()) - 1);
    uint16_t cnt;
    int sent = 0;

    free_bitmap &= block_mask;
    cnt = (uint16_t)__builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)) {
        return 0;
    }

    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        int core = (int)idx;
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        uint32_t raw_duration = g_basic_buf[task_id & RING_MASK].duration;
        uint32_t jitter_mask = g_basic_buf[task_id & RING_MASK].jitter_mask;

        g_executors[exe_type][core].tasks[slot] = task_id;
        if (dispatch_submit(ctrl, type, exe_type, task_id, core, slot, mask, raw_duration,
                            jitter_mask) != 0) {
            uint16_t one = task_id;
            batch_enqueue(&ctrl->ready_queue[type], &one, 1);
            break;
        }
        WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        sent++;
        free_bitmap &= ~mask;
    }
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;

    dispatch_tick_begin(tid);
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;
    uint64_t start_ns = platform_time_ns();
    int prev;
    int count;

    platform_dispatch_loop_enter(tid, start_ns);
    while (!atomic_load(&g_orch_is_done)) {
        total_sent += dispatch(tid);
        dispatch_poll(tid);
        spin_wait();
    }
    platform_dispatch_loop_phase2_begin((int)g_completed_cnt,
                                        (uint32_t)atomic_load_explicit(&g_task_id,
                                                                       memory_order_acquire));
    prev = g_completed_cnt;
    count = dispatch_stall_limit();
    while (atomic_load(&g_completed_cnt) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        total_sent += dispatch(tid);
        dispatch_poll(tid);
        spin_wait();
        if (prev == g_completed_cnt) {
            count--;
            if (count < 0) {
                MAIN_LOGF("[scheduler] stall timeout: completed_cnt=%u task_id=%u",
                          (unsigned)g_completed_cnt, (unsigned)g_task_id);
                platform_dispatch_stall_trace((int)g_completed_cnt,
                                              (uint32_t)atomic_load(&g_task_id), prev);
                break;
            }
        } else {
            count = dispatch_stall_limit();
        }
        prev = g_completed_cnt;
    }

    atomic_store(&g_is_done, true);
    (void)total_sent;
    {
        uint64_t end_ns = platform_time_ns();
        uint64_t elapsed_ns = end_ns - start_ns;

        MAIN_LOGF("[scheduler] task_cnt = %u", (unsigned)g_completed_cnt);
        MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
        MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",
                  (float)(g_completed_cnt * 1000.0 / elapsed_ns));
        platform_dispatch_loop_exit(tid, elapsed_ns);
    }
    return NULL;
}
