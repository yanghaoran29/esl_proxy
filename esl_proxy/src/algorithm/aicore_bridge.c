/* aicore_bridge.c — AICore dispatch/poll (sim + onboard shared). */
#define _GNU_SOURCE

#include "aicore_bridge.h"

#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "handshake.h"
#include "memory_barrier.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "swimlane_aicpu.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];

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

static void aicore_bridge_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                             uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (!platform_reg_task_finished(reg_addr, reg_task)) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    {
        uint64_t *field = &g_ctrl_t[0].msg_bitmap[exe_type][slot];

        cache_flush_range((const void *)field, sizeof(uint64_t));
        OUT_OF_ORDER_STORE_BARRIER();
    }
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
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

void aicore_bridge_shutdown(EslRuntime *runtime)
{
    if (runtime != NULL) {
        esl_shutdown_all_cores(runtime);
    }
}

int aicore_bridge_poll_completions(EslRuntime *runtime, int dispatch_tid)
{
    (void)dispatch_tid;
    if (runtime == NULL) {
        return 0;
    }
    const int n_workers = runtime->worker_count;
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
                        _fin = platform_reg_task_finished(_reg_addr, reg_task);
                    }
                    if (_fin) {
                        aicore_bridge_mark_slot_complete(exe_type, core, slot, _reg_addr,
                                                         reg_task);
                    }
                }
            }
        }
    }
    return 0;
}

int aicore_bridge_dispatch_task(EslRuntime *runtime, int dispatch_tid, uint16_t task_id, int core,
                                int slot, int exe_type, uint32_t block_idx)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = platform_pick_phys_worker(core, exe_type);
    g_executors[exe_type][core].block_idx[slot] = (uint16_t)phys;
    if (runtime != NULL && phys >= runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= (uint64_t)1 << core;
        g_executors[exe_type][core].base[slot] = 0;
        return 0;
    }
    const uint64_t reg_addr = core_reg_addr(phys);
    if (reg_addr == 0) {
        return -1;
    }
    {
        const uint32_t reg_task = esl_next_reg_task_id(phys);
        esl_dispatch_payload_prepare(runtime, phys, reg_task, task_id, block_idx);
        g_executors[exe_type][core].base[slot] = reg_task;
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys, ESL_AICPU_ROLE_DISPATCH);
        write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, reg_task);
        /* sim: write_reg records FIN synchronously; onboard: HW completes later via poll */
        aicore_bridge_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
    }
    return 0;
}
