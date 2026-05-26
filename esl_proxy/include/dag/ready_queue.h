/*
 * ready_queue.h - 2D ReadyQueue Matrix for Task Dispatch
 *
 * Global 2D matrix of MPMC queues indexed by (task_type, org_mode).
 * 3 task types × 4 org modes = 12 queues total.
 * Naming follows Constitution XI: no dag_ prefix.
 */

#ifndef DAG_READY_QUEUE_H
#define DAG_READY_QUEUE_H

#include "mpmc_queue.h"
#include "task.h"

#define TASK_TYPE_COUNT 3
#define ORG_MODE_COUNT  4

extern mpmc_queue_t g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT];

static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode) {
    return &g_ready_queues[type][mode];
}

static inline mpmc_status_t ready_enqueue(task_type_t type, org_mode_t mode, const void *item) {
    return mpmc_enqueue(&g_ready_queues[type][mode], item);
}

static inline mpmc_status_t ready_dequeue(task_type_t type, org_mode_t mode, void *item) {
    return mpmc_dequeue(&g_ready_queues[type][mode], item);
}

#endif /* DAG_READY_QUEUE_H */