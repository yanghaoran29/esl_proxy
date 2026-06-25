/* aicpu_platform_init.c — onboard platform bring-up (memory pool, bridge, swimlane). */
#define _GNU_SOURCE

#include "aicore_bridge.h"
#include "aicpu_runtime.h"
#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "mem_pool.h"
#include "onboard_config.h"
#include "ring_buf.h"
#include "runtime.h"
#include "swimlane_aicpu.h"

#include <stddef.h>
#include <stdint.h>

#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096

extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];

void init_predecessors(void);

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
