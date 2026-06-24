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
void esl_singlethread_drive(void);
void init_predecessors(void);

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern task_state *g_state_buf;
extern _Atomic uint16_t g_predecessor_cnt[RING_SIZE];
extern _Atomic uint16_t g_commit_task_id;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern atomic_bool g_is_done;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static atomic_int g_thread_idx;
static atomic_bool g_init_done;
static atomic_bool g_init_failed;
static atomic_flag g_once = ATOMIC_FLAG_INIT;
static AicoreBridge g_bridge;
static uint64_t g_device_start_cycle;
static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];
static _Atomic uint32_t g_aiv_lane_pick[PLATFORM_MAX_BLOCKDIM];
#if ESL_PROXY_ENABLE_L2_SWIMLANE
static uint64_t g_hw_dispatch_ts[EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
#endif
static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];
volatile uint64_t *g_esl_stats_base;

/* ========================================================================== */
/* Runtime execute path                                                       */
/* ========================================================================== */

static int init_once(EslRuntime *runtime)
{
    if (atomic_flag_test_and_set_explicit(&g_once, memory_order_acquire)) {
        while (!atomic_load_explicit(&g_init_done, memory_order_acquire) &&
               !atomic_load_explicit(&g_init_failed, memory_order_acquire)) {
        }
        return atomic_load_explicit(&g_init_failed, memory_order_acquire) ? -1 : 0;
    }
    if (esl_platform_init(runtime, &g_bridge) != 0) {
        atomic_store_explicit(&g_init_failed, true, memory_order_release);
        return -1;
    }
    if (get_platform_regs() != 0) {
        if (esl_handshake_all_cores(runtime) != 0) {
            atomic_store_explicit(&g_init_failed, true, memory_order_release);
            return -1;
        }
    }
    atomic_store_explicit(&g_init_done, true, memory_order_release);
    return 0;
}

int32_t esl_aicpu_execute(EslRuntime *runtime)
{
    int idx;

    if (runtime == NULL) {
        return -1;
    }
    if (init_once(runtime) != 0) {
        return -1;
    }
    esl_onboard_invalidate_runtime(runtime);
    idx = atomic_fetch_add_explicit(&g_thread_idx, 1, memory_order_acq_rel);
    if (idx != 0) {
        return 0;
    }
    aicpu_orchestration_entry(0);
    esl_signal_orch_done();
    esl_singlethread_drive();
    esl_platform_shutdown(&g_bridge);
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
    rc = esl_aicpu_execute(runtime);
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
    esl_onboard_invalidate_before_poll();
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
                if (atomic_load_explicit(&g_ctrl_t[0].msg_bitmap[exe_type][slot],
                                         memory_order_acquire) &
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
#if ESL_PROXY_ENABLE_L2_SWIMLANE
                    uint64_t finish_ts = esl_onboard_sys_cnt();
                    ESL_SWIMLANE_AICPU_COMPLETE_TASK(phys, 0, reg_task, g_hw_dispatch_ts[exe_type][core][slot],
                                                     finish_ts);
#endif
                    (void)atomic_fetch_or_explicit(&g_ctrl_t[0].msg_bitmap[exe_type][slot], mask,
                                                   memory_order_release);
                    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
                    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
                }
            }
        }
    }
    esl_onboard_flush_after_poll();
    return 0;
}

static void esl_pack_dispatch_input(EslOnboardDispatchInput *in, uint16_t task_id)
{
    const struct task_desc *td = &g_basic_buf[task_id & RING_MASK];
    uint16_t i;

    memset(in, 0, sizeof(*in));
    in->task.id = task_id;
    in->task.type = (uint16_t)td->type;
    in->task.mode = (uint16_t)td->mode;
    in->task.tensor_cnt = td->tensor_cnt;
    in->task.scalar_cnt = td->scalar_cnt;
    in->task.duration = td->duration;
    in->task.jitter_mask = td->jitter_mask;
    in->task.index = td->index;
    in->task.count = td->count;
    in->task.kernel = (uint64_t)(uintptr_t)td->kernel;

    for (i = 0; i < td->tensor_cnt && i < ESL_ONBOARD_MAX_TENSOR_ARGS; ++i) {
        in->tensor_addrs[i] = td->data[i];
    }
    for (i = 0; i < td->scalar_cnt && i < ESL_ONBOARD_MAX_SCALAR_ARGS; ++i) {
        in->scalars[i] = td->scalar[i];
    }
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
                                int exe_type)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = esl_pick_phys_worker(core, exe_type);
    g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;
    if (bridge != NULL && bridge->runtime != NULL && phys >= bridge->runtime->worker_count) {
        (void)atomic_fetch_or_explicit(&g_ctrl_t[0].msg_bitmap[exe_type][slot],
                                       (uint64_t)1 << core, memory_order_release);
        g_executors[exe_type][core].base[slot] = 0;
        return 0;
    }
    EslOnboardDispatchInput din;

    esl_pack_dispatch_input(&din, task_id);
    const uint64_t reg_addr = core_reg_addr(phys);
    if (reg_addr == 0) {
        return -1;
    }
    {
        const uint32_t reg_task = esl_next_reg_task_id(phys);
        esl_dispatch_payload_prepare(phys, reg_task, &din);
        g_executors[exe_type][core].base[slot] = reg_task;
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, 0);
#if ESL_PROXY_ENABLE_L2_SWIMLANE
        g_hw_dispatch_ts[exe_type][core][slot] = esl_onboard_sys_cnt();
#endif
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
    cache_flush_range(g_predecessors, sizeof(g_predecessors));
    cache_flush_range(g_successor_buf, sizeof(g_successor_buf));
    cache_flush_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    cache_flush_range(g_state_buf, sizeof(task_state) * RING_SIZE);
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
}

void esl_onboard_invalidate_shared_before_worker(void)
{
    cache_invalidate_range(&g_task_id, sizeof(g_task_id));
    cache_invalidate_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_invalidate_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_invalidate_range(&g_is_done, sizeof(g_is_done));
    cache_invalidate_range(g_basic_buf, sizeof(g_basic_buf));
    cache_invalidate_range(g_predecessors, sizeof(g_predecessors));
    cache_invalidate_range(g_successor_buf, sizeof(g_successor_buf));
    cache_invalidate_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    cache_invalidate_range(g_state_buf, sizeof(task_state) * RING_SIZE);
    cache_invalidate_range(&g_commit_task_id, sizeof(g_commit_task_id));
    cache_invalidate_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_invalidate_range(g_executors, sizeof(g_executors));
}

void esl_onboard_invalidate_before_poll(void)
{
    cache_invalidate_range(g_executors, sizeof(g_executors));
    cache_invalidate_range(g_ctrl_t, sizeof(g_ctrl_t));
}

void esl_onboard_flush_after_cutter(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    cache_flush_range(g_state_buf, sizeof(task_state) * RING_SIZE);
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
}

void esl_onboard_flush_after_dispatch(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(g_executors, sizeof(g_executors));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
}

void esl_onboard_flush_after_poll(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(g_executors, sizeof(g_executors));
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
#if ESL_PROXY_ENABLE_L2_SWIMLANE
    if (bridge != NULL && bridge->runtime != NULL) {
        int n = bridge->runtime->worker_count;
        int cores[RUNTIME_MAX_WORKER];
        int i;

        if (n > RUNTIME_MAX_WORKER) {
            n = RUNTIME_MAX_WORKER;
        }
        for (i = 0; i < n; ++i) {
            cores[i] = i;
        }
        ESL_SWIMLANE_AICPU_FLUSH(0, cores, n);
    }
#endif
    aicore_bridge_shutdown(bridge);
}

/* ========================================================================== */
/* Orchestration case (ORCH_CASE env / build flag)                            */
/* ========================================================================== */

#include INCLUDE_FILE(ORCH_CASE)
