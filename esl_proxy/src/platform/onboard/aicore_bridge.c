/* aicore_bridge.c — onboard dispatch payload, bridge poll/dispatch. */
#define _GNU_SOURCE

#include "aicore_bridge.h"
#include "onboard_cache_hooks.h"
#include "handshake.h"
#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "memory_barrier.h"
#include "onboard_config.h"
#include "onboard_log.h"
#include "platform.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "onboard_crosscore_sync.h"
#include "ring_buf.h"
#include "swimlane_aicpu.h"
#include "task.h"
#include "tools.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];

extern task_state *g_state_buf;
extern struct node_list g_successor_buf[RING_SIZE];

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
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
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

static void esl_pack_dispatch_input(EslOnboardDispatchInput *in, uint16_t task_id, uint32_t block_idx)
{
    const struct task_desc *td = &g_basic_buf[task_id & RING_MASK];

    memset(in, 0, sizeof(*in));
    in->task.id = task_id;
    in->task.type = (uint16_t)td->type;
    in->task.mode = (uint16_t)td->mode;
    in->task.tensor_cnt = td->tensor_cnt;
    in->task.scalar_cnt = td->scalar_cnt;
    in->task.duration = td->duration;
    in->task.jitter_mask = td->jitter_mask;
    in->task.index = block_idx;
    in->task.count = td->count;
    in->task.kernel = (uint64_t)(uintptr_t)td->kernel;
    in->data = td->data;
    in->scalars = td->scalar;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
                                int exe_type, uint32_t block_idx)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = platform_pick_phys_worker(core, exe_type);
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

void esl_onboard_invalidate_runtime(void *runtime)
{
    if (runtime != NULL) {
        cache_invalidate_range(runtime, sizeof(EslRuntime));
    }
}

void esl_onboard_flush_shared_after_orch(void)
{
    extern atomic_int g_task_id;
    extern atomic_bool g_orch_is_done;

    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(g_basic_buf, sizeof(g_basic_buf));
    cache_flush_range(g_predecessors, sizeof(g_predecessors));
    cache_flush_range(g_successor_buf, sizeof(g_successor_buf));
    cache_flush_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    cache_flush_range(g_state_buf, sizeof(task_state) * RING_SIZE);
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
}
