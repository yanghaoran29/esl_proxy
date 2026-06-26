/*
 * sched_sync.h - Scheduler snapshot cache sync for shared AICPU state.
 *
 * invalidate_sched_snapshot(): drop this core's cached view of the shared
 *   scheduler counters/queues before reading them (onboard non-coherent GM).
 * publish_counters(): flush this core's writes to the shared scheduler
 *   counters so other cores can observe them.
 *
 * sim backend: cache_* are no-ops + compiler barrier, so these are no-ops.
 * onboard backend: cache_* perform dc civac / dc cvac by cache line.
 */
#ifndef ESL_PROXY_ALGORITHM_SCHED_SYNC_H
#define ESL_PROXY_ALGORITHM_SCHED_SYNC_H

#include <stdatomic.h>
#include <stdint.h>

#include "conf.h"
#include "dispatch.h"
#include "queue.h"
#include "ring_buf.h"
#include "platform.h"

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern uint16_t g_commit_task_id;
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

static inline void publish_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    if (task_id < RING_SIZE) {
        cache_flush_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
}

static inline void invalidate_sched_snapshot(void)
{
    cache_invalidate_range(&g_task_id, sizeof(g_task_id));
    cache_invalidate_range(&g_commit_task_id, sizeof(g_commit_task_id));
    cache_invalidate_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
    cache_invalidate_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_invalidate_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_invalidate_range(g_predecessor_cnt, sizeof(g_predecessor_cnt));
    for (int i = 0; i < TASK_TYPE_CNT; i++) {
        cache_invalidate_range(&g_ctrl_t[0].ready_queue[i], sizeof(queue_t));
    }
    cache_invalidate_range(&g_ctrl_t[0].completed_queue, sizeof(queue_t));
}

static inline void publish_counters(void)
{
    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
    cache_flush_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(&g_predecessor_ring.tail, sizeof(g_predecessor_ring.tail));
}

/* advance_task_id: publish the just-finished task slot, bump g_task_id, then
 * publish counters. Replaces the old platform_advance_task_id() wrapper.
 * sim: cache_* are no-ops → atomic_fetch_add only. onboard: full flush. */
static inline void advance_task_id(void)
{
    const uint16_t finished = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_relaxed);

    publish_task_slot(finished);
    atomic_fetch_add_explicit(&g_task_id, 1, memory_order_release);
    publish_counters();
}

#endif /* ESL_PROXY_ALGORITHM_SCHED_SYNC_H */
