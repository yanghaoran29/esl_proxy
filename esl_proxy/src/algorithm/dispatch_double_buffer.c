/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#define _GNU_SOURCE

#include "dispatch.h"
#include "dispatch_spmd_mix.h"
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
#include <string.h>

#include <stdatomic.h>

extern struct task_desc g_basic_buf[RING_SIZE];

extern atomic_int g_task_id;
extern atomic_bool g_orch_is_done;
extern atomic_int g_completed_cnt;
extern atomic_bool g_is_done;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern _Atomic uint16_t g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

EslRuntime *g_runtime;

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

/* Cached COND-register pointer for a physical worker (mirrors dispatch_core_reg_addr
 * but returns the resolved pointer so the poll reads it without reg_offset()/base math
 * each spin). Falls back to the platform table when handshake hasn't cached one. */
static volatile uint32_t *dispatch_core_cond_ptr(int worker_id)
{
    volatile uint32_t *cond_ptr = esl_handshake_cond_ptr(worker_id);

    if (cond_ptr != NULL) {
        return cond_ptr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return NULL;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return NULL;
    }
    const uint64_t reg_addr = ((uint64_t *)table)[hal_idx];

    if (reg_addr == 0) {
        return NULL;
    }
    return platform_reg_cond_ptr(reg_addr);
}

static void dispatch_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                        uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (!platform_reg_task_finished(reg_addr, reg_task)) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
    if (!dispatch_mix_defer_slot_clear(exe_type, core, slot)) {
        g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
}

/* 完成一个已判定的 slot（不再校验 FIN——调用方已通过 running/pending 推断或直接 FIN 定夺）。
 * 用于双缓冲:同一物理核 2 个在飞时,单 COND 寄存器只留最新值,先发任务的 FIN 会被后发任务
 * 的 ACK/FIN 覆盖;看到后发(pending)事件即可推断先发(running)已完成(AICore 串行执行)。 */
static void dispatch_force_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                    uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] & mask) {
        return; /* 已完成 */
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
    if (!dispatch_mix_defer_slot_clear(exe_type, core, slot)) {
        g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
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
    dispatch_merge_msg_to_free(&g_ctrl_t[tid]);
}

/* 把 AICore 完成事件拉到 msg_bitmap，供 push_2_completed_queue 解码。 */
void dispatch_poll(int tid)
{
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int core = 0; core < n_cores; core++) {
            if (CORE_LANE(core) != tid) {
                continue;
            }
            const uint64_t mask = (uint64_t)1 << core;
            /* 收集本 (exe_type,core) 仍在飞、未完成、phys 合法的 slot。 */
            int bs[AIC_OSTD];
            int nb = 0;
            for (int slot = 0; slot < AIC_OSTD; slot++) {
                if (g_executors[exe_type][core].tasks[slot] == EXEC_SLOT_EMPTY) {
                    continue;
                }
                if (g_ctrl_t[tid].msg_bitmap[exe_type][slot] & mask) {
                    continue;
                }
                const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                if (phys < 0 || phys >= n_workers) {
                    continue;
                }
                if ((uint32_t)g_executors[exe_type][core].base[slot] == 0U) {
                    continue;
                }
                bs[nb++] = slot;
            }
            if (nb == 0) {
                continue;
            }
            const int phys0 = (int)g_executors[exe_type][core].block_idx[bs[0]];
            if (nb == 2 && (int)g_executors[exe_type][core].block_idx[bs[1]] == phys0) {
                /* 同一物理核 2 个在飞:COND 单寄存器只留最新值。仿照 simpler:读一次 COND,
                 * 按 seq 定 running(旧)/pending(新),用"看到 pending 事件即推断 running 完成"
                 * (串行执行)化解覆盖丢失 —— 见 simpler decide_slot_transition。 */
                const int s0 = bs[0];
                const int s1 = bs[1];
                const uint32_t b0 = (uint32_t)g_executors[exe_type][core].base[s0];
                const uint32_t b1 = (uint32_t)g_executors[exe_type][core].base[s1];
                int run_slot;
                int pend_slot;
                const uint32_t fwd01 = (b1 - b0) & (uint32_t)TASK_ID_MASK;

                if (fwd01 != 0U && fwd01 < (1U << 30)) {
                    run_slot = s0; /* b1 在 b0 之后 → s1 为 pending */
                    pend_slot = s1;
                } else {
                    run_slot = s1;
                    pend_slot = s0;
                }
                const uint32_t run_reg = (uint32_t)g_executors[exe_type][core].base[run_slot];
                const uint32_t pend_reg = (uint32_t)g_executors[exe_type][core].base[pend_slot];
                volatile uint32_t *cond_ptr = dispatch_core_cond_ptr(phys0);
                if (cond_ptr == NULL) {
                    continue;
                }
                const uint32_t cond = *cond_ptr;
                OUT_OF_ORDER_LOAD_BARRIER();
                const int id = EXTRACT_TASK_ID((uint64_t)cond);
                const int st = EXTRACT_TASK_STATE((uint64_t)cond);

                if (id == (int)pend_reg) {
                    /* 看到 pending 的 ACK/FIN ⇒ running 必已完成(串行)。仅在此刻(确有完成)
                     * 才解析 reg_addr 供 ack 使用。 */
                    const uint64_t reg_addr = dispatch_core_reg_addr(phys0);

                    dispatch_force_complete(exe_type, core, run_slot, reg_addr, run_reg);
                    if (st == TASK_FIN_STATE) {
                        dispatch_force_complete(exe_type, core, pend_slot, reg_addr, pend_reg);
                    }
                } else if (id == (int)run_reg && st == TASK_FIN_STATE) {
                    /* running 已 FIN 但 pending 仍在飞:瞬态,等 pending 事件统一收割。 */
                    (void)0;
                }
                /* 其余(running ACK / 无匹配):跳过,下轮再看。 */
            } else {
                /* 不同物理核(VECTOR 轮替 lane)或单个在飞:各 slot 独立判 FIN。 */
                for (int k = 0; k < nb; k++) {
                    const int slot = bs[k];
                    const uint32_t reg_task = (uint32_t)g_executors[exe_type][core].base[slot];
                    const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                    volatile uint32_t *cond_ptr = dispatch_core_cond_ptr(phys);

                    if (cond_ptr == NULL) {
                        continue;
                    }
                    const uint32_t cond = *cond_ptr;
                    OUT_OF_ORDER_LOAD_BARRIER();
                    if (EXTRACT_TASK_STATE((uint64_t)cond) == TASK_FIN_STATE &&
                        EXTRACT_TASK_ID((uint64_t)cond) == (int)reg_task) {
                        /* 单次读已确认 FIN → force_complete(不再二次读 COND)。 */
                        const uint64_t reg_addr = dispatch_core_reg_addr(phys);

                        dispatch_force_complete(exe_type, core, slot, reg_addr, reg_task);
                    }
                }
            }
        }
    }
}

static inline void push_2_completed_queue(int tid)
{
    uint16_t task_id[DISPATCH_COMPLETE_BATCH];
    int complete_cnt = dispatch_push_completed_slots(&g_ctrl_t[tid], task_id, DISPATCH_COMPLETE_BATCH);

    if (complete_cnt > 0) {
        batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
        atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_release);
        wmb();
    }
}

static int dispatch_publish_block(ctrl_t *ctrl, int exe_type, int queue_type, uint16_t task_id,
                                  uint32_t block_idx, int core, int slot)
{
    const uint64_t mask = (uint64_t)1 << core;
    int rc = 0;

    if (g_runtime != NULL && (int)g_executors[exe_type][core].block_idx[slot] >= g_runtime->worker_count) {
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
        g_executors[exe_type][core].base[slot] = 0;
        return 0;
    }
    {
        const int phys = (int)g_executors[exe_type][core].block_idx[slot];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);

        if (reg_addr == 0) {
            rc = -1;
        } else {
            EslPublishHandle pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, block_idx);

            if (pub.reg_task_id == 0U) {
                rc = -1;
            } else {
                pub.reg_addr = reg_addr;
                g_executors[exe_type][core].base[slot] = pub.reg_task_id;
                ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
                wmb();
                esl_publish_subtask_to_core(pub);
                dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, pub.reg_task_id);
            }
        }
    }
    if (rc != 0) {
        ctrl->free_bitmap[queue_type][slot] |= mask;
        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    }
    return rc;
}

/* Split prepare for batched range-publish (see dispatch.c counterpart). */
static int dispatch_publish_block_prepare(ctrl_t *ctrl, int exe_type, int queue_type, uint16_t task_id,
                                          uint32_t block_idx, int core, int slot,
                                          EslPublishHandle *out, int *fast)
{
    const uint64_t mask = (uint64_t)1 << core;

    *fast = 0;
    if (g_runtime != NULL && (int)g_executors[exe_type][core].block_idx[slot] >= g_runtime->worker_count) {
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[exe_type][slot] |= mask;
        g_executors[exe_type][core].base[slot] = 0;
        *fast = 1;
        return 0;
    }
    {
        const int phys = (int)g_executors[exe_type][core].block_idx[slot];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);
        EslPublishHandle pub;

        if (reg_addr == 0) {
            goto fail;
        }
        pub = esl_prepare_subtask_to_core(g_runtime, phys, task_id, block_idx);
        if (pub.reg_task_id == 0U) {
            goto fail;
        }
        pub.reg_addr = reg_addr;
        g_executors[exe_type][core].base[slot] = pub.reg_task_id;
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
        *out = pub;
        return 0;
    }
fail:
    ctrl->free_bitmap[queue_type][slot] |= mask;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
    return -1;
}

static int send_task_mix(ctrl_t *ctrl)
{
    int sent = 0;
    int cl_core[AIC_CNT];
    int cl_slot[AIC_CNT];
    int ncl = 0;
    int used = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        int slot;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        if (dispatch_mix_cluster_idle(ctrl, core, &slot)) {
            cl_core[ncl] = core;
            cl_slot[ncl] = slot;
            ncl++;
        }
    }

    while (used < ncl) {
        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t start;
        int n;
        EslPublishHandle pubs[3 * AIC_CNT];
        int phys_arr[3 * AIC_CNT];
        int np = 0;
        int fail = 0;
        int b = 0;

        if (!batch_dequeue(&g_shared_ready[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
            break;
        }
        n = dispatch_spmd_claim_range(one, ncl - used, &start);
        if (n <= 0) {
            continue;
        }
        for (; b < n; b++) {
            int core = cl_core[used];
            int slot = cl_slot[used];

            used++;
            dispatch_mix_occupy_cluster(ctrl, core, slot, one, start + (uint32_t)b);
            if (dispatch_mix_prepare_cluster(ctrl, core, slot, one, start + (uint32_t)b, pubs, phys_arr, &np) != 0) {
                dispatch_mix_release_cluster(ctrl, core, slot);
                fail = 1;
                break;
            }
            sent++;
        }
        dispatch_mix_flush(pubs, phys_arr, np);
        if (fail) {
            dispatch_spmd_rewind(one, start + (uint32_t)n, start + (uint32_t)b);
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            break;
        }
        if (dispatch_spmd_has_remaining(one)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
        }
    }
    return sent;
}

// TODO: Work Stealing
static inline int send_task(ctrl_t *ctrl, int type)
{
    int exe_type = type;

    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    int cnt = __builtin_popcountll(free_bitmap);

    if (cnt <= 0) {
        WORKER_LOGF("send,free_cnt,%d", cnt);
        return 0;
    }
    uint16_t task_ids[AIC_CNT];
    if (!batch_dequeue(&g_shared_ready[type], task_ids, &cnt)) {
        return 0;
    }

    int sent = 0;
    for (int i = 0; i < cnt; i++) {
        uint16_t task_id = task_ids[i];
        int avail = __builtin_popcountll(free_bitmap);
        uint32_t start;
        int n;

        if (avail <= 0) {
            batch_enqueue(&g_shared_ready[type], &task_ids[i], (uint16_t)(cnt - i));
            break;
        }
        n = dispatch_spmd_claim_range(task_id, avail, &start);
        if (n <= 0) {
            continue;
        }

        struct {
            int core;
            int slot;
            EslPublishHandle h;
        } pend[AIC_CNT];
        int np = 0;
        int fail = 0;
        int b = 0;

        for (; b < n; b++) {
            uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
            uint64_t mask = (uint64_t)0x1 << idx;
            int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
            int core = (int)idx;
            EslPublishHandle h;
            int fast = 0;

            g_executors[exe_type][core].tasks[slot] = task_id;
            g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
            g_executors[exe_type][core].idx = (uint8_t)slot;
            if (slot == 1) {
                ctrl->task_id_map2[type][idx] = task_id;
            } else {
                ctrl->task_id_map1[type][idx] = task_id;
            }
            ctrl->free_bitmap[type][slot] &= ~mask;
            g_executors[exe_type][core].block_idx[slot] = (uint16_t)platform_pick_phys_worker(core, exe_type);
            free_bitmap &= ~mask;

            if (dispatch_publish_block_prepare(ctrl, exe_type, type, task_id, start + (uint32_t)b, core, slot, &h,
                                               &fast) != 0) {
                fail = 1;
                break;
            }
            sent++;
            if (!fast) {
                pend[np].core = core;
                pend[np].slot = slot;
                pend[np].h = h;
                np++;
            }
        }

        if (np > 0) {
            wmb();
            for (int p = 0; p < np; p++) {
                esl_publish_subtask_to_core(pend[p].h);
                dispatch_mark_slot_complete(exe_type, pend[p].core, pend[p].slot, pend[p].h.reg_addr,
                                            pend[p].h.reg_task_id);
            }
        }

        if (fail) {
            dispatch_spmd_rewind(task_id, start + (uint32_t)n, start + (uint32_t)b);
            batch_enqueue(&g_shared_ready[type], &task_ids[i], (uint16_t)(cnt - i));
            break;
        }
        if (dispatch_spmd_has_remaining(task_id)) {
            batch_enqueue(&g_shared_ready[type], &task_id, 1);
        }
    }
    return sent;
}

static int dispatch_prefetch(ctrl_t *ctrl, int type)
{
    int exe_type = type;
    int sent = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        /* 24 核完整测试:core 0 也参与预取。 */
        /* 找出恰好一个 busy slot 和一个 free slot。 */
        int busy_slot = -1;
        int free_slot = -1;
        for (int s = 0; s < AIC_OSTD; s++) {
            if (g_executors[exe_type][core].tasks[s] != EXEC_SLOT_EMPTY) {
                busy_slot = (busy_slot < 0) ? s : -2;
            } else {
                free_slot = (free_slot < 0) ? s : -2;
            }
        }
        if (busy_slot < 0 || free_slot < 0) {
            continue; /* 不是恰好一忙一闲 */
        }
        const uint64_t mask = (uint64_t)1 << core;
        if (!(ctrl->free_bitmap[type][free_slot] & mask)) {
            continue; /* free slot 必须真正标记为空闲 */
        }
        /* gate:首任务必须已被 AICore ACK。 */
        const uint32_t reg_busy = (uint32_t)g_executors[exe_type][core].base[busy_slot];
        const int phys = (int)g_executors[exe_type][core].block_idx[busy_slot];
        const uint64_t reg_addr = dispatch_core_reg_addr(phys);
        if (reg_addr == 0 || !platform_reg_task_acked(reg_addr, reg_busy)) {
            continue;
        }
        /* 取一个就绪任务。 */
        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t block_idx;

        if (!batch_dequeue(&g_shared_ready[type], &one, &cnt1) || cnt1 < 1) {
            break;
        }
        if (!dispatch_spmd_claim_block(one, &block_idx)) {
            break;
        }
        {
            g_executors[exe_type][core].idx = (uint8_t)free_slot;
            if (free_slot == 1) {
                ctrl->task_id_map2[type][core] = one;
            } else {
                ctrl->task_id_map1[type][core] = one;
            }
            ctrl->free_bitmap[type][free_slot] &= ~mask;
            g_executors[exe_type][core].tasks[free_slot] = one;
            g_executors[exe_type][core].duration[free_slot] = g_basic_buf[one & RING_MASK].duration;
            const int phys = platform_pick_phys_worker(core, exe_type);
            g_executors[exe_type][core].block_idx[free_slot] = (uint16_t)phys;
            if (dispatch_publish_block(ctrl, exe_type, type, one, block_idx, core, free_slot) != 0) {
                batch_enqueue(&g_shared_ready[type], &one, 1);
                break;
            }
            if (dispatch_spmd_has_remaining(one)) {
                batch_enqueue(&g_shared_ready[type], &one, 1);
            }
        }
        WORKER_LOGF("prefetch,task_id,%u,core,%d,slot,%d,type,%d,block,%u", one, core, free_slot,
                    type, (unsigned)block_idx);
        sent++;
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
    total_sent += send_task_mix(&g_ctrl_t[tid]);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += dispatch_prefetch(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

static void dispatch_publish_final_stats(uint64_t elapsed_ns)
{
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
    uint64_t rqc = (uint64_t)g_shared_ready[TASK_TYPE_CUBE].cnt;
    uint64_t rqv = (uint64_t)g_shared_ready[TASK_TYPE_VECTOR].cnt;

    platform_stats_publish((uint64_t)end, (uint64_t)g_subtask_cnt, (uint64_t)g_completed_cnt,
                           ((uint64_t)(uint32_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire)),
                           (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                           (rqc & 0xffffffffULL) | (rqv << 32), elapsed_ns);
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;

    int total_sent = 0;
    uint64_t start_ns = get_time_ns();

#if ESL_ORCH_FIRST
    /* Orchestrator-first model: wait for the orchestrator to fully finish before
     * dispatching (see cutter_worker). The loop below then falls straight into
     * the drain loop since g_orch_is_done is already set. */
    while (!atomic_load_explicit(&g_orch_is_done, memory_order_acquire)) {
        spin_wait();
    }
#endif

    /* Poll BEFORE dispatch (simpler's Phase-1 completion poll ahead of Phase-4
     * dispatch): harvest FINs first so dispatch()'s get_free_exe merges them and
     * re-dispatches the freed cores in the SAME iteration, instead of carrying the
     * completion across the spin to next iteration. */
    while (!atomic_load_explicit(&g_orch_is_done, memory_order_relaxed)) {
        dispatch_poll(tid);
        total_sent += dispatch(tid);
        spin_wait();
    }
    while (atomic_load_explicit(&g_completed_cnt, memory_order_acquire) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        dispatch_poll(tid);
        total_sent += dispatch(tid);
        spin_wait();
    }

    atomic_store_explicit(&g_is_done, true, memory_order_release);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[scheduler] lane %d dispatched %d subtasks", tid, total_sent);
    MAIN_LOGF("[scheduler] task_cnt = %u", g_completed_cnt);
    MAIN_LOGF("[scheduler] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[scheduler] task_tp = %f MTasks/s",(float)(g_completed_cnt * 1000.0 / elapsed_ns));
    if (tid == 0) {
        dispatch_publish_final_stats(elapsed_ns);
    }

    return NULL;
}
