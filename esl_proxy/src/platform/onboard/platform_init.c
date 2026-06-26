/* platform_init.c — onboard platform bring-up (memory pool, swimlane, handshake). */
#define _GNU_SOURCE

#include "platform.h"

#include "aicpu_runtime.h"
#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "executor.h"
#include "handshake.h"
#include "mem_pool.h"
#include "onboard_config.h"
#include "ring_buf.h"
#include "runtime.h"
#include "swimlane_aicpu.h"
#include "task.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096

static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];

extern task_state *g_state_buf;
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;
extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void init_predecessors(void);

void platform_handshake_aicore_bootstrap(EslRuntime *runtime)
{
    (void)runtime;
}

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
    g_runtime = runtime;
    ESL_SWIMLANE_AICPU_INIT(ESL_PROXY_ONBOARD_WORKER_COUNT);

    esl_onboard_trace(-1, ESL_TRACE_INIT_HANDSHAKE, 0, 0, 0);
    if (esl_handshake_start(runtime) != 0) {
        return -1;
    }
    return 0;
}

void esl_platform_shutdown(EslRuntime *runtime)
{
    ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(runtime);
    if (runtime != NULL) {
        esl_shutdown_all_cores(runtime);
    }
}

void platform_dispatch_loop_exit(int tid, uint64_t elapsed_ns)
{
    (void)tid;
    (void)elapsed_ns;
    int end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    int first_uncomp = -1;
    int n_uncomp = 0;
    int i;

    for (i = 0; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            if (first_uncomp < 0) {
                first_uncomp = i;
            }
            n_uncomp++;
        }
    }
    {
        uint64_t pred0 = (first_uncomp >= 0) ? (uint64_t)g_predecessor_cnt[first_uncomp] : 0;
        uint64_t rqc = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE].cnt;
        uint64_t rqv = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_VECTOR].cnt;

        esl_write_stats((uint64_t)end, (uint64_t)g_subtask_cnt, (uint64_t)g_completed_cnt,
                        ((uint64_t)(uint32_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire)),
                        (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                        (rqc & 0xffffffffULL) | (rqv << 32));
    }
}
