/*
 * platform_init.c — sim host platform init (runtime layout + algorithm init + handshake).
 */
#include "platform.h"

#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "handshake.h"
#include "mem_pool.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "ring_buf.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void init_predecessors(void);

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static EslFakeDispatchPayload g_sim_payload[PLATFORM_HOST_WORKER_COUNT * 2U];
static uint64_t g_sim_reg_table[ESL_PROXY_PLATFORM_HAL_REG_SLOTS];
static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

static void sim_runtime_setup(EslRuntime *rt, EslFakeDispatchPayload *payload, int worker_count)
{
    int i;

    if (rt == NULL || payload == NULL || worker_count <= 0 ||
        worker_count > RUNTIME_MAX_WORKER) {
        return;
    }

    memset(rt, 0, sizeof(*rt));
    rt->worker_count = worker_count;
    rt->aicpu_thread_num = CUTTER_THREAD_CNT + DISPATCH_THREAD_CNT + 1;

    for (i = 0; i < worker_count; ++i) {
        rt->workers[i].task =
            (uint64_t)(uintptr_t)(payload + (size_t)i * 2U);
        rt->workers[i].core_type = (i < AIC_CNT) ? 0 : 1;
    }
}

void platform_handshake_aicore_bootstrap(EslRuntime *rt)
{
    int s;
    int i;

    if (rt == NULL) {
        return;
    }

    for (s = 0; s < (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS; ++s) {
        g_sim_reg_table[s] = 0x1000ULL + (uint64_t)s * 0x100ULL;
    }
    set_platform_regs((uint64_t)(uintptr_t)g_sim_reg_table);

    for (i = 0; i < rt->worker_count; ++i) {
        int hal_idx = esl_worker_to_hal_reg_index(i);

        rt->workers[i].physical_core_id = (uint32_t)(hal_idx >= 0 ? hal_idx : 0);
        rt->workers[i].aicore_regs_ready = 1;
        rt->workers[i].aicore_done = (uint32_t)(i + 1);
    }
}

int esl_platform_init(EslRuntime *runtime)
{
    if (runtime == NULL) {
        return -1;
    }

    sim_runtime_setup(runtime, g_sim_payload, PLATFORM_HOST_WORKER_COUNT);
    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    init_predecessors();
    init_ctrl_t();
    executors_init_slots_empty();

    if (esl_handshake_start(runtime) != 0) {
        return -1;
    }
    g_runtime = runtime;
    return 0;
}

void esl_platform_shutdown(EslRuntime *runtime)
{
    if (runtime != NULL) {
        esl_shutdown_all_cores(runtime);
    }
}

void platform_dispatch_loop_exit(int tid, uint64_t elapsed_ns)
{
    (void)tid;
    (void)elapsed_ns;
}
