#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "log.h"
#include "manager.h"
#include "mem_pool.h"
#include "qwen3_decode.h"
#include "swimlane.h"

#define MEM_POOL_BYTES (512UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t cutter_threads[CUTTER_THREAD_CNT];
    pthread_t manager_thread;

#if WORKER_LOG
    const char *log_env = getenv("WORKER_LOG");
    if (log_env != NULL && log_env[0] == '1')
        g_worker_log = 1;
#endif

    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();

    SWIM_INIT();
    SWIM_LANE(SL_ORCH, LANE_ORCH, "orchestrator");
    SWIM_LANE(SL_MANAGER, LANE_MANAGER, "manager");
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) SWIM_LANE(SL_DISPATCH(i), LANE_DISPATCH, "dispatch");
    for (int i = 0; i < CUTTER_THREAD_CNT; i++)   SWIM_LANE(SL_CUTTER(i),   LANE_CUTTER,   "cutter");
    for (int i = 0; i < SL_EXEC_COUNT; i++)        SWIM_LANE(8 + i,          LANE_EXEC,     "executor");

    pthread_create(&manager_thread, NULL, manager_worker, &g_mem_pool);

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker,
                       (void *)(intptr_t)i);
    }

    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_create(&cutter_threads[i], NULL, cutter_worker,
                       (void *)(intptr_t)i);
    }

    SWIM_PHASE_BEGIN(SL_ORCH, PH_ORCH_SUBMIT);
    aicpu_orchestration_entry(0);
    SWIM_PHASE_END(SL_ORCH);

#ifdef ESL_SWIMLANE
    /* Let the worker threads drain for a bounded window, then snapshot the
     * swimlane and exit cleanly. Off builds keep the original return-immediately
     * behavior. The window is spin-measured off the swimlane clock to avoid
     * pulling in extra POSIX timing headers here. */
    {
        const uint64_t t0 = SWIM_NOW();
        const uint64_t budget = swim_freq() / 2; /* ~500 ms */
        while (SWIM_NOW() - t0 < budget) {
            /* spin: give dispatch/cutter/manager time to make progress */
        }
        SWIM_DUMP("outputs/swimlane_records.json");
        SWIM_SHUTDOWN();
    }
#endif

    return 0;
}
