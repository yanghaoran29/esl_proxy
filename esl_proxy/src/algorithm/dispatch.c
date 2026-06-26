/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "handshake.h"
#include "runtime.h"

#include "cutter.h"
#include "executor.h"
#include "log.h"
#include "memory_barrier.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"

#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"

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

EslRuntime *g_runtime;

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];

static uint64_t dispatch_core_reg_addr(int worker_id)
{
    uint64_t reg_addr = esl_handshake_reg_addr(worker_id);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return 0;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
}

static void dispatch_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                        uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (!platform_reg_task_finished(reg_addr, reg_task)) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
}

static uint32_t dispatch_next_reg_task_id(int phys)
{
    uint32_t seq;
    uint32_t reg_id;

    if (phys < 0 || phys >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    seq = ++g_core_dispatch_seq[phys];
    reg_id = seq & (uint32_t)TASK_ID_MASK;
    if (reg_id >= (uint32_t)AICORE_EXIT_SIGNAL) {
        g_core_dispatch_seq[phys] = seq + ((uint32_t)AICORE_EXIT_SIGNAL - reg_id);
        reg_id = (uint32_t)(g_core_dispatch_seq[phys] & (uint32_t)TASK_ID_MASK);
    }
    return reg_id;
}

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
        }
    }
    set_mix(tid);
}

/* 把 AICore 完成事件拉到 msg_bitmap，供 dispatch_drain_completions 解码。 */
void dispatch_poll(int tid)
{
    (void)tid;
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n_cores; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                uint64_t mask = (uint64_t)1 << core;
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
                    continue;
                }
                const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                if (phys < 0 || phys >= n_workers) {
                    continue;
                }
                const uint32_t reg_task = (uint32_t)g_executors[exe_type][core].base[slot];
                if (reg_task == 0U) {
                    continue;
                }
                const uint64_t reg_addr = dispatch_core_reg_addr(phys);
                if (reg_addr != 0 && platform_reg_task_finished(reg_addr, reg_task)) {
                    dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
                }
            }
        }
    }
}

/* 向 AICore 下发一个任务。bridge 拒绝（返回非 0）时回退 free_bitmap。 */
int dispatch_submit(ctrl_t *ctrl, int type, int exe_type, uint16_t task_id, int core, int slot,
                    uint64_t mask)
{
    {
        const uint16_t task_slot = (uint16_t)(task_id & RING_MASK);

        cache_invalidate_range(&g_basic_buf[task_slot], sizeof(g_basic_buf[task_slot]));
        if (task_id < RING_SIZE) {
            cache_invalidate_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
        }
        cache_invalidate_range(&g_predecessor_cnt[task_slot], sizeof(g_predecessor_cnt[task_slot]));
    }
    g_executors[exe_type][core].idx = (uint8_t)slot;

    if (slot == 1) {
        ctrl->task_id_map2[type][core] = task_id;
    } else {
        ctrl->task_id_map1[type][core] = task_id;
    }

    // Clear the free bit for this core/slot combination (mark as busy)
    ctrl->free_bitmap[type][slot] &= ~mask;

    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = platform_pick_phys_worker(core, exe_type);
    g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;

    int rc = 0;
    if (g_runtime != NULL && phys >= g_runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= (uint64_t)1 << core;
        g_executors[exe_type][core].base[slot] = 0;
    } else {
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);
        if (reg_addr == 0) {
            rc = -1;
        } else {
            const uint32_t reg_task = dispatch_next_reg_task_id(phys);
            esl_dispatch_payload_prepare(g_runtime, phys, reg_task, task_id, 0);
            g_executors[exe_type][core].base[slot] = reg_task;
            ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
            wmb();
            write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, reg_task);
            /* sim: write_reg records FIN synchronously; onboard: HW completes later via poll */
            dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
        }
    }
    if (rc != 0) {
        ctrl->free_bitmap[type][slot] |= mask;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
    return rc;
}

/* 核心动作二——回收已完成任务。扫描 EXE_TYPE_CNT × AIC_OSTD 各 slot 的
 * msg_bitmap，把每个置位 bit 通过 task_id_map1/2 反查回 task_id，最多追加
 * max_cnt 个到 task_ids[]。对应 msg_bitmap bit 由 drain_completed_bitmap 清除。 */
void dispatch_drain_completions(int tid, uint16_t *task_ids, int *complete_cnt, int max_cnt)
{
    for (int i = 0; i < EXE_TYPE_CNT && *complete_cnt < max_cnt; i++) {
        drain_completed_bitmap(&g_ctrl_t[tid].msg_bitmap[i][0], task_ids, complete_cnt,
                               g_ctrl_t[tid].task_id_map1[i], max_cnt);
        drain_completed_bitmap(&g_ctrl_t[tid].msg_bitmap[i][1], task_ids, complete_cnt,
                               g_ctrl_t[tid].task_id_map2[i], max_cnt);
    }
}

uint32_t dispatch_executor_duration(uint32_t raw_duration)
{
    return (raw_duration > 10000U) ? (raw_duration / 10000U) : 1U;
}

// TODO: add counter for spmd
static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = 0;

    /* 回收完成：解码 msg_bitmap → task_ids，入 completed_queue */
    dispatch_drain_completions(tid, task_id, &complete_cnt, DISPATCH_COMPLETE_BATCH);
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_release);
    wmb();
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    // Check both slots - slot is free if neither slot 0 nor slot 1 has been sent a task
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    uint64_t block_mask = (uint64_t)((1ULL << AIC_CNT) - 1);
    int sent = 0;
    free_bitmap &= block_mask;
    int cnt = __builtin_popcountll(free_bitmap);
    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &cnt)){
        return 0;
    }
    
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        // Determine which slot to use - prefer slot 0 if it's not busy
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        int core = (int)idx;
        /* 下发任务；被拒绝时回退入队此单任务并结束本轮。 */
        if (dispatch_submit(ctrl, type, exe_type, task_id, core, slot, mask) != 0) {
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

    /* 本轮读取共享状态前 acquire 调度 counter */
    atomic_thread_fence(memory_order_acquire);
    get_free_exe(tid);
    push_2_completed_queue(tid);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    // atomic_store(&g_is_done, true);
    // return NULL;
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;
    uint64_t start_ns = get_time_ns();
    
    while (!atomic_load(&g_orch_is_done)) {
        total_sent += dispatch(tid);
        /* 把硬件 AICore 完成事件拉到 msg_bitmap，供下一轮 drain */
        dispatch_poll(tid);
        spin_wait();
    }
    while (atomic_load(&g_completed_cnt) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        total_sent += dispatch(tid);
        /* 把硬件 AICore 完成事件拉到 msg_bitmap，供下一轮 drain */
        dispatch_poll(tid);
        spin_wait();
    }
    
    atomic_store(&g_is_done, true);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    platform_dispatch_loop_exit(tid, elapsed_ns);  // flush final stats (onboard)

    return NULL;
}
