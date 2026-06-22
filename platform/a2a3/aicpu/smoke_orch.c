/*
 * Smoke orchestration — compiled as C for onboard AICPU.
 */
#include <stdint.h>
#include <stdatomic.h>

#include "ring_buf.h"

#define DUR_SMOKE_A 1000
#define DUR_SMOKE_B 2000
#define DUR_SMOKE_C 1500
#define DUR_SMOKE_D 1200

static void smoke_orch_run(uint64_t orch_args)
{
    (void)orch_args;

    uint16_t t0 = (uint16_t)atomic_load(&g_task_id);
    new_task(t0, TASK_TYPE_CUBE, 1, DUR_SMOKE_A);
    atomic_fetch_add(&g_task_id, 1);

    uint16_t t1 = (uint16_t)atomic_load(&g_task_id);
    new_task(t1, TASK_TYPE_VECTOR, 1, DUR_SMOKE_B);
    {
        uint16_t pred[] = {t0};
        add_predecessors(t1, pred, 1, 0);
    }
    atomic_fetch_add(&g_task_id, 1);

    uint16_t t2 = (uint16_t)atomic_load(&g_task_id);
    new_task(t2, TASK_TYPE_CUBE, 1, DUR_SMOKE_C);
    {
        uint16_t pred[] = {t1};
        add_predecessors(t2, pred, 1, 0);
    }
    atomic_fetch_add(&g_task_id, 1);

    uint16_t t3 = (uint16_t)atomic_load(&g_task_id);
    new_task(t3, TASK_TYPE_VECTOR, 1, DUR_SMOKE_D);
    {
        uint16_t pred[] = {t2};
        add_predecessors(t3, pred, 1, 0);
    }
    atomic_fetch_add(&g_task_id, 1);
}

void aicpu_orchestration_entry(const uint64_t orch_args)
{
    smoke_orch_run(orch_args);
}
