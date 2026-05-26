/*
 * complete_queue.c - Global CompleteQueue Definition
 *
 * Defines the single global CompleteQueue.
 */

#include "complete_queue.h"
#include "mpmc_queue.h"

mpmc_queue_t g_complete_queue;