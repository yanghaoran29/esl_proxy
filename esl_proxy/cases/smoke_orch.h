/*
 * Minimal onboard smoke orchestration (4-task chain, no Tensor / mem_pool).
 * Build: ORCH_CASE=smoke_orch.h
 */
#ifndef ESL_PROXY_SMOKE_ORCH_H
#define ESL_PROXY_SMOKE_ORCH_H

#include <stdint.h>

#include "ring_buf.h"

#define DUR_SMOKE_A 1000U
#define DUR_SMOKE_B 2000U
#define DUR_SMOKE_C 1500U
#define DUR_SMOKE_D 1200U

void aicpu_orchestration_entry(const uint64_t orch_args)
{
    uint16_t t0;
    uint16_t t1;
    uint16_t t2;
    uint16_t t3;
    uint16_t pred[1];

    (void)orch_args;

    t0 = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_acquire);
    new_task(t0, TASK_TYPE_CUBE, 1, DUR_SMOKE_A, 0U);
    esl_onboard_advance_task_id();

    t1 = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_acquire);
    new_task(t1, TASK_TYPE_VECTOR, 1, DUR_SMOKE_B, 0U);
    pred[0] = t0;
    add_predecessors(t1, pred, 1, 0);
    esl_onboard_advance_task_id();

    t2 = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_acquire);
    new_task(t2, TASK_TYPE_CUBE, 1, DUR_SMOKE_C, 0U);
    pred[0] = t1;
    add_predecessors(t2, pred, 1, 0);
    esl_onboard_advance_task_id();

    t3 = (uint16_t)atomic_load_explicit(&g_task_id, memory_order_acquire);
    new_task(t3, TASK_TYPE_VECTOR, 1, DUR_SMOKE_D, 0U);
    pred[0] = t2;
    add_predecessors(t3, pred, 1, 0);
    esl_onboard_advance_task_id();
}

#endif /* ESL_PROXY_SMOKE_ORCH_H */
