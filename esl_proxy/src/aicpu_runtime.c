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

static uint64_t core_reg_addr(int core)
{
    uint64_t reg_addr = esl_handshake_reg_addr(core);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();

    if (table == 0) {
        return 0;
    }
    return ((uint64_t *)table)[core];
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
    const int n = bridge->runtime->worker_count;
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n && core < AIC_CNT; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & ((uint64_t)1 << core)) {
                    continue;
                }
                const int phys = esl_phys_worker(core, exe_type);
                if (phys >= n) {
                    continue;
                }
                if (esl_hw_poll_fin(core_reg_addr(phys), task_id)) {
                    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= ((uint64_t)1 << core);
                    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
                    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
                }
            }
        }
    }
    return 0;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
                                int exe_type)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = esl_phys_worker(core, exe_type);
    if (bridge != NULL && bridge->runtime != NULL && phys >= bridge->runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= ((uint64_t)1 << core);
        return 0;
    }
    const uint32_t raw = g_basic_buf[task_id & RING_MASK].duration;
    esl_dispatch_payload_prepare(phys, task_id, raw);
    const uint64_t reg_addr = core_reg_addr(phys);
    if (reg_addr == 0) {
        return -1;
    }
    esl_hw_dispatch_reg(reg_addr, task_id);
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
    cache_flush_range(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_flush_range(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_flush_range(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
}

void esl_onboard_invalidate_shared_before_worker(void)
{
    cache_invalidate_range(&g_task_id, sizeof(g_task_id));
    cache_invalidate_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_invalidate_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_invalidate_range(&g_is_done, sizeof(g_is_done));
    cache_invalidate_range(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_invalidate_range(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_invalidate_range(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
    cache_invalidate_range(g_ctrl_t, sizeof(g_ctrl_t));
}

void esl_onboard_flush_after_cutter(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
}

void esl_onboard_flush_after_dispatch(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
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
        runtime->worker_count = ESL_PROXY_FAKE_AICORE_COUNT;
    }
    uint64_t fake_addr = 0;
    if (runtime != NULL) {
        fake_addr = runtime->func_id_to_addr_[0];
    }
    if (aicore_bridge_init(bridge, runtime, fake_addr) != 0) {
        return -1;
    }
    dispatch_set_aicore_bridge(bridge);
    return 0;
}

void esl_platform_shutdown(AicoreBridge *bridge)
{
    aicore_bridge_shutdown(bridge);
}

/* ========================================================================== */
/* Orchestration case (ORCH_CASE env / build flag)                            */
/* ========================================================================== */

#include INCLUDE_FILE(ORCH_CASE)
