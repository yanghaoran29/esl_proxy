/*
 * complete_queue.h - Global CompleteQueue for Task Completion Tracking
 *
 * Single global MPMC queue for recording completed task notifications.
 * Workers enqueue completion notifications, scheduler dequeues to update DAG state.
 * Naming follows Constitution XI: no dag_ prefix.
 */

#ifndef DAG_COMPLETE_QUEUE_H
#define DAG_COMPLETE_QUEUE_H

#include "mpmc_queue.h"

extern mpmc_queue_t g_complete_queue;

static inline mpmc_status_t complete_enqueue(const void *item) {
    return mpmc_enqueue(&g_complete_queue, item);
}

static inline mpmc_status_t complete_dequeue(void *item) {
    return mpmc_dequeue(&g_complete_queue, item);
}

#endif /* DAG_COMPLETE_QUEUE_H */