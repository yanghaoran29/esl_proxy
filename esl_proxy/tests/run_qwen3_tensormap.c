/*
 * run_qwen3_tensormap.c - single-process driver that runs the tensormap qwen3
 * orchestration to completion.
 *
 * Build phase: aicpu_orchestration_entry() constructs the whole DAG; tensormap
 * discovers each task's producers and succeed() wires the edges; roots land in
 * the ready queue. Drain phase: pop ready tasks, "execute" (a no-op for the
 * proxy), and complete_task() propagates completion to successors until the
 * queue drains. Verifies every task reached COMPLETED.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -I include -I cases \
 *       tests/run_qwen3_tensormap.c src/mpmc_queue.c -o /tmp/run_qwen3_tm
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "qwen3_decode_tensormap.h"

int main(void) {
    /* The proxy never dereferences task buffers, so hand the pool a huge virtual
     * span: alloc_tensors then returns unique addresses (the DAG/tensormap keys)
     * without using real memory. */
    mem_pool_init(&g_mem_pool, (void *)(uintptr_t)0x10000000u, (size_t)1 << 42);
    init_global_queues();

    aicpu_orchestration_entry(0);
    uint32_t total = g_task_id;
    printf("built tasks = %u\n", total);

    void *item;
    uint64_t done = 0;
    while (ready_dequeue(TASK_TYPE_CUBE, ORG_MODE_SINGLE, &item) == MPMC_OK) {
        complete_task((uint16_t)(uintptr_t)item); /* proxy execution is a no-op */
        done++;
    }
    printf("completed   = %llu\n", (unsigned long long)done);

    assert(done == total);
    for (uint32_t i = 1; i <= total; i++) {
        task_state s = atomic_load_explicit(&g_state_buf[i & RING_MASK], memory_order_relaxed);
        assert(s.state == COMPLETED);
    }

    printf("\nESL_PROXY RUN OK: scheduled all %u tasks to completion (single-process)\n", total);
    return 0;
}
