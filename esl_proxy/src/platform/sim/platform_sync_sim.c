/*
 * platform_sync_sim.c — host CPU no-op shared-state sync stubs.
 */
#include "platform.h"

#include "conf.h"
#include "worker_map.h"

#include <stdlib.h>
#include <stdatomic.h>

void platform_publish_task_slot(uint16_t task_id)
{
    (void)task_id;
}

void platform_publish_predecessor_cnt(uint16_t task_id)
{
    (void)task_id;
}

void platform_publish_counters(void)
{
}

void platform_publish_atomic_u64(uint64_t *field)
{
    (void)field;
}

void platform_publish_u16(uint16_t *field)
{
    (void)field;
}

void platform_consume_task_slot(uint16_t task_id)
{
    (void)task_id;
}

void platform_consume_min_uncomplete(void)
{
}

void platform_invalidate_sched_snapshot(void)
{
}

void platform_queue_lock_prepare(queue_t *queue)
{
    (void)queue;
}

void platform_queue_unlock_publish(queue_t *queue)
{
    (void)queue;
}

uint32_t platform_worker_block_dim(void)
{
    return (uint32_t)ESL_PROXY_WORKER_BLOCK_DIM;
}

void platform_predecessor_ring_init(uint16_t **head_out)
{
    if (head_out != NULL) {
        *head_out = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)NODE_BUFF_SIZE);
    }
}

void platform_state_buf_init(void **buf_out, size_t *size_out)
{
    if (buf_out != NULL) {
        *buf_out = NULL;
    }
    if (size_out != NULL) {
        *size_out = 0;
    }
}

void platform_orch_done_notify(void)
{
}

void platform_advance_task_id(void)
{
    extern atomic_int g_task_id;
    (void)atomic_fetch_add_explicit(&g_task_id, 1, memory_order_relaxed);
}

void platform_scheduler_idle(void)
{
}
