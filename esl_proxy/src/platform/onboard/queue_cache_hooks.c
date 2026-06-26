#include "platform.h"
#include "queue.h"

void platform_queue_lock_prepare(queue_t *queue)
{
    if (queue != NULL) {
        cache_invalidate_range(queue, sizeof(*queue));
    }
}

void platform_queue_unlock_publish(queue_t *queue)
{
    if (queue != NULL) {
        cache_flush_range(queue, sizeof(*queue));
    }
}
