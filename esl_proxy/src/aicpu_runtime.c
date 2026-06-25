/* aicpu_runtime.c — onboard AICPU functional layer (CANN entry, bridge, orchestration). */
#define _GNU_SOURCE

#include "aicpu_runtime.h"
#include "onboard_config.h"
#include "tools.h"
#include "onboard_log.h"
#include "dlog_pub.h"
#include "aicpu_bridge.h"
#include "kernel_args.h"
#include "conf.h"
#include "dispatch.h"
#include "ring_buf.h"
#include "task.h"
#include "cutter.h"
#include "mem_pool.h"
#include "l2_swimlane/esl_swimlane_aicpu_c.h"
#include "esl_swimlane_aicpu_onboard.h"
#include "onboard/onboard_crosscore_sync.h"
#include "onboard/onboard_trace.h"
#include "spin.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ORCH_CASE
#define ORCH_CASE paged_attention_unroll_manual_scope.h
#endif
#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)

#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096

void aicpu_orchestration_entry(uint64_t orch_args);
void esl_signal_orch_done(void);
void init_predecessors(void);

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern task_state *g_state_buf;
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern atomic_bool g_is_done;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static atomic_int g_thread_idx;
static atomic_int g_finished_count;
static atomic_int g_workers_ready;
static atomic_bool g_init_done;
static atomic_bool g_init_failed;
static atomic_flag g_once = ATOMIC_FLAG_INIT;
static AicoreBridge g_bridge;
static uint64_t g_device_start_cycle;
static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];
static _Atomic uint32_t g_aiv_lane_pick[PLATFORM_MAX_BLOCKDIM];
ESL_SWIMLANE_AICPU_DISPATCH_TS_STORAGE;
static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];
volatile uint64_t *g_esl_stats_base;

/* ========================================================================== */
/* Runtime execute path                                                       */
/* ========================================================================== */

static int init_once(EslRuntime *runtime, int thread)
{
    if (atomic_flag_test_and_set_explicit(&g_once, memory_order_acquire)) {
        esl_onboard_trace(thread, ESL_TRACE_INIT_ONCE_WAIT, 0, 0, 0);
        while (!atomic_load_explicit(&g_init_done, memory_order_acquire) &&
               !atomic_load_explicit(&g_init_failed, memory_order_acquire)) {
        }
        esl_onboard_trace(thread, ESL_TRACE_INIT_DONE, 1, 0, 0);
        return atomic_load_explicit(&g_init_failed, memory_order_acquire) ? -1 : 0;
    }
    esl_onboard_trace(thread, ESL_TRACE_INIT_ONCE_LEADER, 0, 0, 0);
    esl_onboard_trace(thread, ESL_TRACE_INIT_PLATFORM, 0, 0, 0);
    if (esl_platform_init(runtime, &g_bridge) != 0) {
        LOG_ERROR("init_once: esl_platform_init failed");
        atomic_store_explicit(&g_init_failed, true, memory_order_release);
        return -1;
    }
    if (get_platform_regs() != 0) {
        esl_onboard_trace(thread, ESL_TRACE_INIT_HANDSHAKE, 0, 0, 0);
        if (esl_handshake_all_cores(runtime) != 0) {
            LOG_ERROR("init_once: esl_handshake_all_cores failed");
            atomic_store_explicit(&g_init_failed, true, memory_order_release);
            return -1;
        }
    }
    atomic_store_explicit(&g_init_done, true, memory_order_release);
    esl_onboard_trace(thread, ESL_TRACE_INIT_DONE, 0, 0, 0);
    return 0;
}

int32_t esl_aicpu_execute(EslRuntime *runtime)
{
    int idx;
    int finished;

    extern uint16_t g_completed_task_cnt;

    if (runtime == NULL) {
        LOG_ERROR("esl_aicpu_execute: null runtime");
        return -1;
    }
    esl_onboard_trace(-1, ESL_TRACE_EXEC_ENTER, 0, 0, 0);
    if (init_once(runtime, -1) != 0) {
        LOG_ERROR("esl_aicpu_execute: init_once failed");
        return -1;
    }
    esl_onboard_invalidate_runtime(runtime);
    idx = atomic_fetch_add_explicit(&g_thread_idx, 1, memory_order_acq_rel);
    LOG_ERROR("AICPU thread %d enter execute", idx);

    if (idx < ESL_PROXY_AICPU_THREAD_NUM) {
        if (atomic_fetch_add_explicit(&g_workers_ready, 1, memory_order_acq_rel) + 1 ==
            ESL_PROXY_AICPU_THREAD_NUM) {
            atomic_thread_fence(memory_order_release);
        } else {
            while (atomic_load_explicit(&g_workers_ready, memory_order_acquire) <
                   ESL_PROXY_AICPU_THREAD_NUM) {
                spin_wait();
            }
        }
        esl_onboard_trace(idx, ESL_TRACE_WORKER_BARRIER, (uint64_t)g_workers_ready, 0, 0);
    }

    switch (idx) {
    case ESL_AICPU_ROLE_CUTTER:
        esl_onboard_trace(idx, ESL_TRACE_CUTTER_START, 0, 0, 0);
        esl_onboard_trace(idx, ESL_TRACE_CUTTER_PRE_CALL, 0, 0, 0);
        cutter_loop_run();
        esl_onboard_trace(idx, ESL_TRACE_CUTTER_DONE, (uint64_t)g_completed_task_cnt, 0, 0);
        break;
    case ESL_AICPU_ROLE_DISPATCH:
        esl_onboard_trace(idx, ESL_TRACE_DISPATCH_START, 0, 0, 0);
        esl_onboard_trace(idx, ESL_TRACE_DISPATCH_PRE_CALL, 0, 0, 0);
        dispatch_loop_run(0);
        esl_onboard_trace(idx, ESL_TRACE_DISPATCH_DONE,
                          (uint64_t)g_completed_cnt,
                          (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire), 0);
        break;
    case ESL_AICPU_ROLE_ORCH:
        esl_onboard_trace(idx, ESL_TRACE_ORCH_START, 0, 0, 0);
        ESL_SWIMLANE_AICPU_SET_ORCH_THREAD(idx);
        esl_onboard_trace(idx, ESL_TRACE_ORCH_PRE_CALL, 0, 0, 0);
        esl_onboard_trace(idx, ESL_TRACE_ORCH_IN_ENTRY, 0, 0, 0);
        aicpu_orchestration_entry(0);
        esl_onboard_trace(idx, ESL_TRACE_ORCH_DONE,
                          (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire),
                          (uint64_t)g_subtask_cnt, 0);
        esl_signal_orch_done();
        esl_onboard_trace(idx, ESL_TRACE_SIGNAL_ORCH_DONE, 0, 0, 0);
        break;
    default:
        esl_onboard_trace(idx, ESL_TRACE_SPARE_WAIT, 0, 0, 0);
        while (atomic_load_explicit(&g_finished_count, memory_order_acquire) <
               ESL_PROXY_AICPU_THREAD_NUM) {
            spin_wait();
        }
        esl_onboard_trace(idx, ESL_TRACE_SPARE_EXIT, 0, 0, 0);
        return 0;
    }

    finished = atomic_fetch_add_explicit(&g_finished_count, 1, memory_order_acq_rel) + 1;
    esl_onboard_trace(idx, ESL_TRACE_FINISHED_BARRIER, (uint64_t)finished, 0, 0);
    if (finished == ESL_PROXY_AICPU_THREAD_NUM) {
        esl_onboard_trace(idx, ESL_TRACE_SHUTDOWN, 0, 0, 0);
        esl_platform_shutdown(&g_bridge);
    }
    esl_onboard_trace(idx, ESL_TRACE_EXEC_RETURN, (uint64_t)finished, 0, 0);
    return 0;
}

void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
                     uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete)
{
    if (g_esl_stats_base != NULL) {
        g_esl_stats_base[0] = task_cnt;
        g_esl_stats_base[1] = subtask_cnt;
        g_esl_stats_base[2] = completed_cnt;
        g_esl_stats_base[4] = commit;
        g_esl_stats_base[5] = ready_cube;
        g_esl_stats_base[6] = ready_vec;
        g_esl_stats_base[7] = min_uncomplete;
        cache_flush_range((const void *)g_esl_stats_base, 8 * sizeof(uint64_t));
    }
}

/* ========================================================================== */
/* CANN kernel entry points                                                   */
/* ========================================================================== */

__attribute__((visibility("default"))) int simpler_aicpu_init(void *arg)
{
    KernelArgs *k_args;

    init_log_switch();
    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    g_device_start_cycle = esl_onboard_sys_cnt();
    if (k_args->device_wall_data_base != 0) {
        *(uint64_t *)(uintptr_t)k_args->device_wall_data_base = 0;
    }
    ESL_SWIMLANE_AICPU_SET_BASE(k_args->l2_swimlane_data_base);
    ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(k_args->l2_swimlane_aicore_rotation_table);
    ESL_SWIMLANE_AICPU_SET_ENABLED(ESL_SWIMLANE_IS_FLAG_ON(k_args->enable_profiling_flag));
    LOG_INFO_V0("%s", "esl_proxy AICPU Init");
    return 0;
}

__attribute__((visibility("default"))) int simpler_aicpu_exec(void *arg)
{
    KernelArgs *k_args;
    EslRuntime *runtime;
    int rc;
    uint64_t my_end;

    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    runtime = (EslRuntime *)(uintptr_t)k_args->runtime_args;
    if (runtime == NULL) {
        LOG_ERROR("%s", "Invalid runtime_args: null pointer");
        return -1;
    }
    ESL_SWIMLANE_AICPU_SET_BASE(k_args->l2_swimlane_data_base);
    ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(k_args->l2_swimlane_aicore_rotation_table);
    ESL_SWIMLANE_AICPU_SET_ENABLED(ESL_SWIMLANE_IS_FLAG_ON(k_args->enable_profiling_flag));
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    set_platform_regs(k_args->regs);
    g_esl_stats_base = (volatile uint64_t *)(uintptr_t)k_args->device_wall_data_base;
    esl_onboard_trace_set_base(g_esl_stats_base);
    LOG_ERROR("simpler_aicpu_exec: start");
    rc = esl_aicpu_execute(runtime);
    LOG_ERROR("simpler_aicpu_exec: rc=%d", rc);
    if (rc != 0) {
        LOG_ERROR("esl_aicpu_execute failed rc=%d", rc);
        return rc;
    }
    my_end = esl_onboard_sys_cnt();
    if (g_esl_stats_base != NULL && my_end > g_device_start_cycle) {
        g_esl_stats_base[3] = (uint64_t)(cycles_to_us(my_end - g_device_start_cycle) * 1000.0);
        cache_flush_range((const void *)g_esl_stats_base, 4 * sizeof(uint64_t));
    }
    return rc;
}

/* ========================================================================== */
/* AICore bridge (dispatch.c)                                                 */
/* ========================================================================== */

static uint64_t core_reg_addr(int worker_id)
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
    /* Fallback: worker_id is NOT a HAL table index for AIV (HAL has 25 AIC slots
     * before the 50 AIV entries; runtime uses 24+48). */
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
}

static int esl_pick_phys_worker(int core, int exe_type)
{
    if (exe_type == 0) {
        return core;
    }
    uint32_t lane = atomic_fetch_add_explicit(&g_aiv_lane_pick[core], 1U, memory_order_relaxed) %
                    (uint32_t)PLATFORM_AIV_CORES_PER_BLOCKDIM;
    return ESL_PROXY_ONBOARD_BLOCK_DIM + core * PLATFORM_AIV_CORES_PER_BLOCKDIM + (int)lane;
}

static uint32_t esl_next_reg_task_id(int phys)
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

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr)
{
    if (bridge == NULL || runtime == NULL) {
        return -1;
    }
    bridge->runtime = runtime;
    bridge->fake_kernel_addr = fake_kernel_addr;
    bridge->initialized = 1;
    return 0;
}

void aicore_bridge_shutdown(AicoreBridge *bridge)
{
    if (bridge != NULL && bridge->initialized) {
        if (bridge->runtime != NULL) {
            esl_shutdown_all_cores(bridge->runtime);
        }
        bridge->initialized = 0;
    }
}

int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid)
{
    (void)dispatch_tid;
    if (bridge == NULL || !bridge->initialized) {
        return 0;
    }
    const int n_workers = bridge->runtime->worker_count;
    const int n_cores = ESL_PROXY_ONBOARD_BLOCK_DIM;
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n_cores && core < AIC_CNT; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                uint64_t mask = (uint64_t)1 << core;
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] &
                    mask) {
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
                if (esl_hw_poll_fin(core_reg_addr(phys), reg_task)) {
                    ESL_SWIMLANE_AICPU_COMPLETE_TASK(
                        phys, ESL_AICPU_ROLE_DISPATCH, reg_task,
                        ESL_SWIMLANE_AICPU_DISPATCH_TS(g_hw_dispatch_ts, exe_type, core, slot));
                    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
                    esl_onboard_publish_atomic_u64(&g_ctrl_t[0].msg_bitmap[exe_type][slot]);
                    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
                    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
                }
            }
        }
    }
    return 0;
}

static void esl_pack_dispatch_input(EslOnboardDispatchInput *in, uint16_t task_id, uint32_t block_idx)
{
    const struct task_desc *td = &g_basic_buf[task_id & RING_MASK];
    const struct task_payload *pay = &g_task_payload[task_id & RING_MASK];

    memset(in, 0, sizeof(*in));
    in->task.id = task_id;
    in->task.type = (uint16_t)td->type;
    in->task.mode = (uint16_t)td->mode;
    in->task.tensor_cnt = pay->tensor_cnt;
    in->task.scalar_cnt = pay->scalar_cnt;
    in->task.duration = td->duration;
    in->task.jitter_mask = td->jitter_mask;
    in->task.index = block_idx;
    in->task.count = td->count;
    in->task.kernel = (uint64_t)(uintptr_t)td->kernel;
    in->tensors = pay->tensors;
    in->scalars = pay->scalars;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
                                int exe_type, uint32_t block_idx)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = esl_pick_phys_worker(core, exe_type);
    g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;
    if (bridge != NULL && bridge->runtime != NULL && phys >= bridge->runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= (uint64_t)1 << core;
        g_executors[exe_type][core].base[slot] = 0;
        return 0;
    }
    EslOnboardDispatchInput din;

    esl_pack_dispatch_input(&din, task_id, block_idx);
    const uint64_t reg_addr = core_reg_addr(phys);
    if (reg_addr == 0) {
        return -1;
    }
    {
        const uint32_t reg_task = esl_next_reg_task_id(phys);
        esl_dispatch_payload_prepare(phys, reg_task, &din);
        g_executors[exe_type][core].base[slot] = reg_task;
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
        ESL_SWIMLANE_AICPU_RECORD_DISPATCH_TS(g_hw_dispatch_ts, exe_type, core, slot);
        esl_hw_dispatch_reg(reg_addr, reg_task);
    }
    return 0;
}

/* ========================================================================== */
/* Shared-memory cache sync (aicpu_bridge.h)                                  */
/* ========================================================================== */

void esl_onboard_invalidate_runtime(void *runtime)
{
    if (runtime != NULL) {
        cache_invalidate_range(runtime, sizeof(EslRuntime));
    }
}

void esl_onboard_flush_shared_after_orch(void)
{
    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(g_basic_buf, sizeof(g_basic_buf));
    cache_flush_range(g_task_payload, sizeof(g_task_payload));
    cache_flush_range(g_predecessors, sizeof(g_predecessors));
    cache_flush_range(g_successor_buf, sizeof(g_successor_buf));
    cache_flush_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    cache_flush_range(g_state_buf, sizeof(task_state) * RING_SIZE);
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
}

/* ========================================================================== */
/* Platform init / shutdown                                                   */
/* ========================================================================== */

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge)
{
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();
    mem_pool_init(&g_mem_pool, ONBOARD_POOL_BASE, ONBOARD_POOL_SIZE);
    mem_pool_init_fifo(&g_mem_pool, g_onboard_when2free, ONBOARD_WHEN2FREE_CAP);
    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        for (int c = 0; c < AIC_CNT; c++) {
            for (int s = 0; s < AIC_OSTD; s++) {
                g_executors[t][c].tasks[s] = EXEC_SLOT_EMPTY;
            }
        }
    }
    esl_dispatch_payload_init(runtime);
    if (runtime != NULL) {
        runtime->worker_count = ESL_PROXY_ONBOARD_WORKER_COUNT;
    }
    uint64_t fake_addr = 0;
    if (runtime != NULL) {
        fake_addr = runtime->func_id_to_addr_[0];
    }
    if (aicore_bridge_init(bridge, runtime, fake_addr) != 0) {
        return -1;
    }
    dispatch_set_aicore_bridge(bridge);
    ESL_SWIMLANE_AICPU_INIT(ESL_PROXY_ONBOARD_WORKER_COUNT);
    return 0;
}

void esl_platform_shutdown(AicoreBridge *bridge)
{
    ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(bridge);
    aicore_bridge_shutdown(bridge);
}

/* ========================================================================== */
/* Orchestration case (ORCH_CASE env / build flag)                            */
/* ========================================================================== */

#include INCLUDE_FILE(ORCH_CASE)
