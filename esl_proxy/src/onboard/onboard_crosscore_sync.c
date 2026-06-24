/*
 * Cross-AICPU-core cache sync for onboard cutter / dispatch / orchestrator.
 */
#ifdef ESL_PROXY_ONBOARD

#include "onboard/onboard_crosscore_sync.h"
#include "queue.h"
#include "aicpu_bridge.h"
#include "ring_buf.h"
#include "cutter.h"
#include "dispatch.h"

#ifdef __aarch64__
#define ESL_ONBOARD_STORE_BARRIER() __asm__ __volatile__("dmb ishst" ::: "memory")
#else
#define ESL_ONBOARD_STORE_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

extern atomic_int g_task_id;
extern int g_min_uncomplete_task;
extern int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern struct ring_buf g_predecessor_ring;

void esl_onboard_publish_queue(queue_t *queue)
{
    if (queue != NULL) {
        cache_flush_range(queue, sizeof(queue_t));
        ESL_ONBOARD_STORE_BARRIER();
    }
}

void esl_onboard_queue_lock_prepare(queue_t *queue)
{
    if (queue != NULL) {
        cache_invalidate_range(queue, sizeof(queue_t));
    }
}

void esl_onboard_queue_unlock_publish(queue_t *queue)
{
    esl_onboard_publish_queue(queue);
}

void esl_onboard_publish_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    if (task_id < RING_SIZE) {
        cache_flush_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
    ESL_ONBOARD_STORE_BARRIER();
}

void esl_onboard_publish_predecessor_cnt(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
    ESL_ONBOARD_STORE_BARRIER();
}

void esl_onboard_publish_counters(void)
{
    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
    cache_flush_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(&g_predecessor_ring.tail, sizeof(g_predecessor_ring.tail));
    ESL_ONBOARD_STORE_BARRIER();
}

void esl_onboard_publish_atomic_u64(uint64_t *field)
{
    if (field != NULL) {
        cache_flush_range((const void *)field, sizeof(uint64_t));
        ESL_ONBOARD_STORE_BARRIER();
    }
}

void esl_onboard_publish_u16(uint16_t *field)
{
    if (field != NULL) {
        cache_flush_range((const void *)field, sizeof(uint16_t));
        ESL_ONBOARD_STORE_BARRIER();
    }
}

void esl_onboard_consume_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_invalidate_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    if (task_id < RING_SIZE) {
        cache_invalidate_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_invalidate_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
}

void esl_onboard_consume_min_uncomplete(void)
{
    cache_invalidate_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
}

void esl_onboard_invalidate_sched_snapshot(void)
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

void esl_onboard_advance_task_id(void)
{
    const uint16_t finished = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_relaxed);

    esl_onboard_publish_task_slot(finished);
    atomic_fetch_add_explicit(&g_task_id, 1, memory_order_release);
    esl_onboard_publish_counters();
}

#endif /* ESL_PROXY_ONBOARD */
