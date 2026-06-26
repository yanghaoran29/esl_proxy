/* aicpu_platform_init.c — onboard platform bring-up (memory pool, bridge, swimlane). */
#define _GNU_SOURCE

#include "aicore_bridge.h"
#include "aicpu_runtime.h"
#include "conf.h"
#include "executor.h"
#include "mem_pool.h"
#include "onboard_config.h"
#include "dispatch.h"
#include "ring_buf.h"
#include "runtime.h"
#include "swimlane_aicpu.h"

#include <stddef.h>
#include <stdint.h>

#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096

static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];

void init_predecessors(void);

int esl_platform_init(EslRuntime *runtime)
{
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();
    mem_pool_init(&g_mem_pool, ONBOARD_POOL_BASE, ONBOARD_POOL_SIZE);
    mem_pool_init_fifo(&g_mem_pool, g_onboard_when2free, ONBOARD_WHEN2FREE_CAP);
    executors_init_slots_empty();
    if (runtime != NULL) {
        runtime->worker_count = ESL_PROXY_ONBOARD_WORKER_COUNT;
    }
    dispatch_bind(runtime);
    ESL_SWIMLANE_AICPU_INIT(ESL_PROXY_ONBOARD_WORKER_COUNT);
    return 0;
}

void esl_platform_shutdown(EslRuntime *runtime)
{
    ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(runtime);
    aicore_bridge_shutdown(runtime);
}
