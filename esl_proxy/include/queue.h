
#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "conf.h"
#include "executor.h"
#include "task.h"

typedef struct queue {
    uint64_t cnt;
    uint64_t head;
    uint64_t tail;
    uint16_t tasks[RING_SIZE];
} queue_t;

// TODO: atomic protect
static inline bool batch_dequeue(queue_t *queue, uint16_t *item, uint16_t n)
{
    if (queue->cnt < n)
        return false;
    /* FIX: dequeue must read from head and advance head; the original read from
     * tail (where enqueue writes), so it returned uninitialized slots and ran
     * tail away. Tracked in docs/swimlane.md. */
    memcpy(item, &queue->tasks[queue->head], n * sizeof(uint16_t));
    queue->head += n;
    queue->cnt -= n;
    return true;
}

// TODO: RING LOOP
static inline bool batch_enqueue(queue_t *queue, uint16_t *item, uint16_t n)
{
    if ((RING_SIZE - queue->cnt) < n)
        return false;
    memcpy(&queue->tasks[queue->tail], item, n * sizeof(uint16_t));
    queue->tail += n;
    queue->cnt += n;
    return true;
}

#endif
