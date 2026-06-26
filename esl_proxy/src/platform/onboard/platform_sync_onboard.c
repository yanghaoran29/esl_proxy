/*
 * platform_sync_onboard.c — onboard cache flush/invalidate for shared scheduler state.
 */
#include "platform.h"

#include "aicore_bridge.h"
#include "onboard_cache_hooks.h"
#include "conf.h"
#include "dispatch.h"
#include "memory_barrier.h"
#include "onboard_config.h"
#include "onboard_crosscore_sync.h"
#include "ring_buf.h"
#include "spin.h"
#include "task.h"
#include "worker_map.h"

#include <stdarg.h>
#include <stdatomic.h>

extern atomic_int g_task_id;

void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

void platform_publish_task_slot(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);

    cache_flush_range(&g_basic_buf[slot], sizeof(g_basic_buf[slot]));
    if (task_id < RING_SIZE) {
        cache_flush_range(&g_predecessors[task_id], sizeof(g_predecessors[task_id]));
    }
    cache_flush_range(&g_predecessor_cnt[slot], sizeof(g_predecessor_cnt[slot]));
    OUT_OF_ORDER_STORE_BARRIER();
}

void platform_publish_predecessor_cnt(uint16_t task_id)
{
    esl_onboard_publish_predecessor_cnt(task_id);
}

void platform_publish_atomic_u64(uint64_t *field)
{
    esl_onboard_publish_atomic_u64(field);
}

void platform_publish_u16(uint16_t *field)
{
    esl_onboard_publish_u16(field);
}

void platform_consume_task_slot(uint16_t task_id)
{
    esl_onboard_consume_task_slot(task_id);
}

void platform_consume_min_uncomplete(void)
{
    esl_onboard_consume_min_uncomplete();
}

void platform_queue_lock_prepare(queue_t *queue)
{
    if (queue != NULL) {
        cache_invalidate_range(queue, sizeof(queue_t));
    }
}

void platform_queue_unlock_publish(queue_t *queue)
{
    if (queue != NULL) {
        cache_flush_range(queue, sizeof(queue_t));
        OUT_OF_ORDER_STORE_BARRIER();
    }
}

void platform_predecessor_ring_init(uint16_t **head_out)
{
    static uint16_t predecessor_storage[NODE_BUFF_SIZE];

    if (head_out != NULL) {
        *head_out = predecessor_storage;
    }
}

void platform_state_buf_init(void **buf_out, size_t *size_out)
{
    static task_state state_storage[RING_SIZE];

    if (buf_out != NULL) {
        *buf_out = state_storage;
    }
    if (size_out != NULL) {
        *size_out = sizeof(state_storage);
    }
}

void platform_orch_done_notify(void)
{
    esl_onboard_flush_shared_after_orch();
}

void platform_scheduler_idle(void)
{
    spin_wait();
}

void platform_main_log_vwrite(int line, const char *fmt, va_list args)
{
    (void)line;
    (void)fmt;
    (void)args;
}
