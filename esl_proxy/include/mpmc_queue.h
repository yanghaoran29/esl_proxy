/*
 * mpmc_queue.h - Global ready queues (orchestration -> dispatch hand-off)
 *
 * One ready queue per task type. Orchestration (submit/batch_submit in
 * ring_buf.h) enqueues a task id when it becomes ready; the dispatch workers
 * drain it into their per-thread ready_queue. Access is guarded by a per-type
 * spin flag (the underlying queue_t ops are not yet lock-free).
 */

#ifndef MPMC_QUEUE_H
#define MPMC_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>

#include "queue.h"
#include "task.h"

extern queue_t g_ready_queue[TASK_TYPE_CNT];
extern atomic_flag g_ready_lock[TASK_TYPE_CNT];

static inline void ready_lock(int type)
{
    while (atomic_flag_test_and_set_explicit(&g_ready_lock[type], memory_order_acquire)) {
        atomic_thread_fence(memory_order_seq_cst);
    }
}

static inline void ready_unlock(int type)
{
    atomic_flag_clear_explicit(&g_ready_lock[type], memory_order_release);
}

static inline void ready_enqueue(task_type_t type, org_mode_t mode, uint16_t task_id)
{
    (void)mode;
    ready_lock(type);
    batch_enqueue(&g_ready_queue[type], &task_id, 1);
    ready_unlock(type);
}

/* Drain up to `max` ready ids of `type` into `out`; returns the count moved. */
static inline uint16_t ready_drain(task_type_t type, uint16_t *out, uint16_t max)
{
    uint16_t n;
    ready_lock(type);
    uint64_t avail = g_ready_queue[type].cnt;
    n = avail > max ? max : (uint16_t)avail;
    if (n > 0) {
        batch_dequeue(&g_ready_queue[type], out, n);
    }
    ready_unlock(type);
    return n;
}

#endif /* MPMC_QUEUE_H */
