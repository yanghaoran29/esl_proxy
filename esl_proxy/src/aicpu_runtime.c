/* aicpu_runtime.c — onboard AICPU functional layer (CANN entry, bridge, orchestration). */
#define _GNU_SOURCE

#include "aicpu_runtime.h"
#include "aicpu_bridge.h"
#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "dlog_pub.h"
#include "kernel_args.h"
#include "mem_pool.h"
#include "onboard/onboard_crosscore_sync.h"
#include "memory_barrier.h"
#include "onboard/onboard_trace.h"
#include "onboard_config.h"
#include "onboard_log.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane/swimlane_aicpu.h"
#include "task.h"
#include "tools.h"

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
static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];
volatile uint64_t *g_esl_stats_base;

/* ========================================================================== */
/* Runtime execute path                                                       */
/* ========================================================================== */

/* 单次初始化：平台、握手与 AICore bridge，多线程仅 leader 执行。 */
static int init_once(EslRuntime *runtime, int thread) {
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

/* AICPU 主执行入口：按线程角色运行 cutter / dispatch / orch 并协调收尾。 */
int32_t esl_aicpu_execute(EslRuntime *runtime) {
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

/* 将调度统计与诊断信息写入 device_wall GM 并刷 cache。 */
void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
    uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete) {
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

/* CANN AICPU init 内核入口：解析 KernelArgs、初始化日志与 swimlane 基址。 */
__attribute__((visibility("default"))) int esl_aicpu_init(void *arg) {
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

/* CANN AICPU exec 内核入口：设置寄存器表与统计基址，调用 esl_aicpu_execute。 */
__attribute__((visibility("default"))) int esl_aicpu_exec(void *arg) {
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
    LOG_ERROR("esl_aicpu_exec: start");
    rc = esl_aicpu_execute(runtime);
    LOG_ERROR("esl_aicpu_exec: rc=%d", rc);
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

/* 解析物理 worker 对应的 AICore 握手寄存器基址（握手表优先，HAL 表兜底）。 */
static uint64_t core_reg_addr(int worker_id) {
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

/* 为指定物理核生成下一个寄存器 task id（避开 EXIT 保留值）。 */
static uint32_t esl_next_reg_task_id(int phys) {
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

/* 初始化 AICore bridge 状态（runtime、fake kernel 地址）。 */
int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr) {
    if (bridge == NULL || runtime == NULL) {
        return -1;
    }
    bridge->runtime = runtime;
    bridge->fake_kernel_addr = fake_kernel_addr;
    bridge->initialized = 1;
    return 0;
}

/* 关闭 AICore bridge 并向所有核发送 shutdown 信号。 */
void aicore_bridge_shutdown(AicoreBridge *bridge) {
    if (bridge != NULL && bridge->initialized) {
        if (bridge->runtime != NULL) {
            esl_shutdown_all_cores(bridge->runtime);
        }
        bridge->initialized = 0;
    }
}

/* 轮询各核 COND 寄存器，将已 FIN 的 slot 标记完成并更新 msg_bitmap。 */
int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid) {
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
                {
                    const uint64_t _reg_addr = core_reg_addr(phys);
                    int _fin = 0;

                    if (_reg_addr != 0) {
                        const uint64_t _cond = read_reg(_reg_addr, REG_ID_COND);

                        OUT_OF_ORDER_LOAD_BARRIER();
                        _fin = (EXTRACT_TASK_STATE(_cond) == TASK_FIN_STATE &&
                                  EXTRACT_TASK_ID(_cond) == (int)reg_task)
                                     ? 1
                                     : 0;
                    }
                    if (_fin) {
                        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
                        esl_onboard_publish_atomic_u64(&g_ctrl_t[0].msg_bitmap[exe_type][slot]);
                        g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
                        g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
                    }
                }
            }
        }
    }
    return 0;
}

/* 从 ring buffer 打包任务描述与 payload 为 AICore dispatch 输入。 */
static void esl_pack_dispatch_input(EslOnboardDispatchInput *in, uint16_t task_id, uint32_t block_idx) {
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

/* 向指定 AICore 下发任务：写 GM payload 并 kick DATA_MAIN_BASE 寄存器。 */
int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
    int exe_type, uint32_t block_idx) {
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
        esl_dispatch_payload_prepare(bridge->runtime, phys, reg_task, &din);
        g_executors[exe_type][core].base[slot] = reg_task;
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
        write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, reg_task);
    }
    return 0;
}

/* 使 AICPU 侧对 GM 中 EslRuntime 的读取看到 host 最新写入。 */
void esl_onboard_invalidate_runtime(void *runtime) {
    if (runtime != NULL) {
        cache_invalidate_range(runtime, sizeof(EslRuntime));
    }
}

/* orch 完成后将共享 DAG/任务状态刷回 GM，供 cutter/dispatch 线程可见。 */
void esl_onboard_flush_shared_after_orch(void) {
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

/* 初始化 onboard 平台子系统：内存池、executor、bridge 与 swimlane。 */
int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge) {
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

/* 平台收尾：刷 swimlane 记录并关闭 AICore bridge。 */
void esl_platform_shutdown(AicoreBridge *bridge) {
    ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(bridge);
    aicore_bridge_shutdown(bridge);
}

#include INCLUDE_FILE(ORCH_CASE)
