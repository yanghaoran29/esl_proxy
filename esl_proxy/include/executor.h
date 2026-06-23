/*
 * executor.h - Executor Type Definition
 *
 * Defines the executor type used by Dispatch for task execution.
 * The executor runs tasks in its 2-slot PING PONG cache.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "conf.h"

/*
 * Sentinel for an unoccupied executor slot. Task ids are dense in [0, RING_SIZE)
 * so id 0 is a VALID task id and cannot double as "empty" — the HW completion
 * poll would otherwise never poll task 0 and the chain rooted at it would hang.
 */
#define EXEC_SLOT_EMPTY ((uint16_t)0xFFFF)

/*
 * Executor
 */
typedef struct executor {
    uint8_t idx;
    uint16_t tasks[AIC_OSTD];
    uint16_t block_idx[AIC_OSTD];
    uint16_t duration[AIC_OSTD];
    uint64_t base[AIC_OSTD];
} executor_t;

/*
 * executor_init - Initialize all executors
 */
void executor_init(void);

/*
 * executor_worker - Worker thread for executor timing
 */
void* executor_worker(void *arg);

/*
 * Global executor array - EXE_TYPE_CNT x AIC_CNT
 */
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

#endif /* EXECUTOR_H */
