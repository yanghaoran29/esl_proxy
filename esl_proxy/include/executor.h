/*
 * executor.h - Executor Type Definition
 *
 * Defines the executor type used by Dispatch for task execution.
 * The executor runs tasks in its 2-slot PING PONG cache.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DAG_EXECUTOR_H
#define DAG_EXECUTOR_H

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>


/*
 * Executor type
 * Contains 2 slots for task caching with PING PONG selection
 */
typedef struct executor {
    uint16_t tasks[2];            /* Slot occupancy (0=empty, 1=occupied) */
    uint16_t index[2];
    uint64_t base[2];
} executor_t;

#endif /* DAG_EXECUTOR_H */