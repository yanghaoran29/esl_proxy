/*
 * platform_bringup.c — onboard AICPU bringup hooks and dispatch stats flush.
 */
#include "platform.h"

#include "aicpu_runtime.h"
#include "conf.h"
#include "dispatch.h"
#include "ring_buf.h"
#include "task.h"

#include <stdatomic.h>

extern task_state *g_state_buf;
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern int g_subtask_cnt;
extern int g_completed_subtask_cnt;
extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

int platform_bringup(void)
{
    return 0;
}

void platform_teardown(void)
{
}

void platform_sched_stats_flush(void)
{
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
                        ((uint64_t)(uint32_t)g_commit_task_id) |
                            ((uint64_t)(uint32_t)g_completed_subtask_cnt << 32),
                        (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                        (rqc & 0xffffffffULL) | (rqv << 32));
    }
}
