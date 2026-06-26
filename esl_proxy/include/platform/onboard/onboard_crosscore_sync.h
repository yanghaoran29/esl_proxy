/*
 * Cross-AICPU-core cache sync for shared scheduler state (non-coherent GM).
 */
#ifndef ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H
#define ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H

#include <stdint.h>
#include <stdatomic.h>

#include "dispatch.h"
#include "ring_buf.h"
#include "conf.h"

typedef struct queue queue_t;

#ifdef __cplusplus
extern "C" {
#endif

#include "memory_barrier.h"
#include "task.h"

void cache_flush_range(const void *addr, size_t size);
void cache_invalidate_range(const void *addr, size_t size);

extern atomic_int g_task_id;
extern atomic_int g_min_uncomplete_task;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern struct task_desc g_basic_buf[RING_SIZE];
extern struct predecessor_list g_predecessors[RING_SIZE];
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern uint16_t g_commit_task_id;
extern struct ring_buf g_predecessor_ring;

static inline void esl_onboard_publish_predecessor_cnt(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
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

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_CROSSCORE_SYNC_H */
