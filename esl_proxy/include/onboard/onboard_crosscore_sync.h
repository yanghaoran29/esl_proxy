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

void esl_onboard_queue_lock_prepare(queue_t *queue);
void esl_onboard_queue_unlock_publish(queue_t *queue);

void esl_onboard_publish_queue(queue_t *queue);
void esl_onboard_publish_task_slot(uint16_t task_id);
void esl_onboard_publish_predecessor_cnt(uint16_t task_id);
void esl_onboard_publish_counters(void);
void esl_onboard_publish_atomic_u64(uint64_t *field);
void esl_onboard_publish_u16(uint16_t *field);

void esl_onboard_consume_task_slot(uint16_t task_id);
void esl_onboard_consume_min_uncomplete(void);
void esl_onboard_invalidate_sched_snapshot(void);

void esl_onboard_advance_task_id(void);

#else

static inline void esl_onboard_queue_lock_prepare(queue_t *queue)
{
    (void)queue;
}

static inline void esl_onboard_queue_unlock_publish(queue_t *queue)
{
    (void)queue;
}

static inline void esl_onboard_publish_queue(queue_t *queue)
{
    (void)queue;
}

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
