/*
 * mpmc_queue.c - Global queue definitions for DAG engine
 *
 * 12 ReadyQueues (3 task types × 4 org_modes) + 1 CompleteQueue
 * Default capacity 1024 per queue
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include "mpmc_queue.h"

/* Global ReadyQueues matrix: g_ready_queues[task_type][org_mode] */
mpmc_queue_t g_ready_queues[3][4];

/* Global CompleteQueue for task completion tracking */
mpmc_queue_t g_complete_queue;

/*
 * Get pointer to ReadyQueue for given task_type and org_mode
 * O(1) access via 2D array indexing
 */
static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode) {
    return &g_ready_queues[type][mode];
}

/*
 * Enqueue task to ReadyQueue by task_type and org_mode
 */
static inline mpmc_ret_t ready_enqueue(task_type_t type, org_mode_t mode, void *item) {
    return blkring_produce(&g_ready_queues[type][mode], item);
}

/*
 * Dequeue task from ReadyQueue by task_type and org_mode
 */
static inline mpmc_ret_t ready_dequeue(task_type_t type, org_mode_t mode, void **item) {
    return blkring_consume(&g_ready_queues[type][mode], item);
}

/*
 * Enqueue to CompleteQueue (global task completion tracking)
 */
static inline mpmc_ret_t complete_enqueue(void *item) {
    return blkring_produce(&g_complete_queue, item);
}

/*
 * Dequeue from CompleteQueue
 */
static inline mpmc_ret_t complete_dequeue(void **item) {
    return blkring_consume(&g_complete_queue, item);
}

/*
 * Initialize all global queues with default capacities
 * Called once at system startup
 */
static void init_global_queues(void) {
    for (int t = 0; t < 3; t++) {
        for (int m = 0; m < 4; m++) {
            mpmc_init(&g_ready_queues[t][m], READY_QUEUE_CAPACITY);
        }
    }
    mpmc_init(&g_complete_queue, COMPLETE_QUEUE_CAPACITY);
}

/*
 * Note: In a header-only library design, initialization would typically
 * happen via a separate init function called by the main program.
 * Global queue initialization deferred to explicit init call.
 */