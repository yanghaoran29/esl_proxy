/*
 * ready_queue.c - Global ReadyQueue Matrix Definition
 *
 * Defines the 2D matrix of 12 MPMC queues.
 */

#include "ready_queue.h"
#include "mpmc_queue.h"

mpmc_queue_t g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT];