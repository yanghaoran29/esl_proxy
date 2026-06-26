/*
 * platform_bringup.c — sim host bringup (runtime + dispatch bind).
 */
#include "runtime.h"
#include "platform.h"

#include "aicore_bridge.h"
#include "handshake.h"
#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "platform_config.h"
#include "platform_regs.h"

static EslRuntime g_sim_runtime;

/* The sim has no real AICore cores, so before running the shared handshake state machine
 * we pre-fill each worker's ack fields (mimicking what aicore_executor.cpp does on-device)
 * and install a non-zero fake register table, so esl_handshake_all_cores() completes. */
static void sim_handshake_prefill(EslRuntime *rt, uint64_t *reg_table, int reg_slots)
{
    int s;
    int i;

    for (s = 0; s < reg_slots; ++s) {
        reg_table[s] = 0x1000ULL + (uint64_t)s * 0x100ULL; /* any non-zero sentinel */
    }
    set_platform_regs((uint64_t)(uintptr_t)reg_table);

    for (i = 0; i < rt->worker_count; ++i) {
        int hal_idx = esl_worker_to_hal_reg_index(i);

        rt->workers[i].physical_core_id = (uint32_t)(hal_idx >= 0 ? hal_idx : 0);
        rt->workers[i].aicore_regs_ready = 1;
        rt->workers[i].aicore_done = (uint32_t)(i + 1);
    }
}

int platform_bringup(void)
{
    static EslFakeDispatchPayload g_sim_payload[PLATFORM_HOST_WORKER_COUNT * 2U];
    static uint64_t g_sim_reg_table[ESL_PROXY_PLATFORM_HAL_REG_SLOTS];

    esl_runtime_setup_host(&g_sim_runtime, g_sim_payload, PLATFORM_HOST_WORKER_COUNT);
    executors_init_slots_empty();
    sim_handshake_prefill(&g_sim_runtime, g_sim_reg_table,
                          (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS);
    if (esl_handshake_all_cores(&g_sim_runtime) != 0) {
        return -1;
    }
    dispatch_bind(&g_sim_runtime);
    return 0;
}

void platform_teardown(void)
{
    aicore_bridge_shutdown(&g_sim_runtime);
}

void platform_sched_stats_flush(void)
{
}
