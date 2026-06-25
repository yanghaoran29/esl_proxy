/*
 * Cross-AICPU-core cache sync for shared scheduler state (non-coherent GM).
 */
#ifndef ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H
#define ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H

#include <stdint.h>
#include <stdatomic.h>

#include "conf.h"

typedef struct queue queue_t;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESL_PROXY_ONBOARD

#include "memory_barrier.h"
#include "task.h"

void cache_flush_range(const void *addr, size_t size);
void cache_invalidate_range(const void *addr, size_t size);

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct task_payload g_task_payload[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern struct ring_buf g_predecessor_ring;

static inline void esl_onboard_publish_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    cache_flush_range(&g_task_payload[slot], sizeof(g_task_payload[slot]));
    if (task_id < RING_SIZE) {
        cache_flush_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
    OUT_OF_ORDER_STORE_BARRIER();
}

static inline void esl_onboard_publish_predecessor_cnt(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
    OUT_OF_ORDER_STORE_BARRIER();
}

static inline void esl_onboard_publish_counters(void)
{
    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_commit_task_id, sizeof(g_commit_task_id));
    cache_flush_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(&g_predecessor_ring.tail, sizeof(g_predecessor_ring.tail));
    OUT_OF_ORDER_STORE_BARRIER();
}

static inline void esl_onboard_publish_atomic_u64(uint64_t *field)
{
    if (field != NULL) {
        cache_flush_range((const void *)field, sizeof(uint64_t));
        OUT_OF_ORDER_STORE_BARRIER();
    }
}

static inline void esl_onboard_publish_u16(uint16_t *field)
{
    if (field != NULL) {
        cache_flush_range((const void *)field, sizeof(uint16_t));
        OUT_OF_ORDER_STORE_BARRIER();
    }
}

static inline void esl_onboard_consume_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_invalidate_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    if (task_id < RING_SIZE) {
        cache_invalidate_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_invalidate_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
}

static inline void esl_onboard_consume_min_uncomplete(void)
{
    cache_invalidate_range(&g_min_uncomplete_task, sizeof(g_min_uncomplete_task));
}

static inline void esl_onboard_invalidate_sched_snapshot(void)
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

static inline void esl_onboard_advance_task_id(void)
{
    const uint16_t finished = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_relaxed);

    esl_onboard_publish_task_slot(finished);
    atomic_fetch_add_explicit(&g_task_id, 1, memory_order_release);
    esl_onboard_publish_counters();
}

#else

static inline void esl_onboard_publish_task_slot(uint16_t task_id)
{
    (void)task_id;
}

static inline void esl_onboard_publish_predecessor_cnt(uint16_t task_id)
{
    (void)task_id;
}

static inline void esl_onboard_publish_counters(void)
{
}

static inline void esl_onboard_publish_atomic_u64(uint64_t *field)
{
    (void)field;
}

static inline void esl_onboard_publish_u16(uint16_t *field)
{
    (void)field;
}

static inline void esl_onboard_consume_task_slot(uint16_t task_id)
{
    (void)task_id;
}

static inline void esl_onboard_consume_min_uncomplete(void)
{
}

static inline void esl_onboard_invalidate_sched_snapshot(void)
{
}

static inline void esl_onboard_advance_task_id(void)
{
    extern atomic_int g_task_id;
    (void)atomic_fetch_add_explicit(&g_task_id, 1, memory_order_relaxed);
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H */
