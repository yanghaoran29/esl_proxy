/*
 * cmp_qwen3.c - build + drain the qwen3 orchestration for performance comparison.
 *
 * Compiled twice (once per case header) via -DCASE_HEADER / -DCASE_NAME:
 *   manual    : cases/qwen3_decode.h            (hand-tracked succeed())
 *   tensormap : cases/qwen3_decode_tensormap.h  (auto dep discovery)
 *
 * Reports: task count, dependency-edge count, orchestration (build) time, and a
 * single-process drain (does the DAG schedule to completion, and how long).
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include CASE_HEADER

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

int main(void) {
    mem_pool_init(&g_mem_pool, (void *)(uintptr_t)0x10000000u, (size_t)1 << 42);
    init_global_queues();

    double b0 = now_ms();
    aicpu_orchestration_entry(0);
    double b1 = now_ms();

    uint32_t tasks = g_task_id;
    uint32_t edges = g_succ_pool_n;

    /* BUILD line first (flushed) so orchestration cost is captured even if a
     * corrupt graph makes the drain loop forever. */
    printf("BUILD %s tasks=%u edges=%u build_ms=%.4f\n", CASE_NAME, tasks, edges, b1 - b0);
    fflush(stdout);

#ifndef NO_DRAIN
    double d0 = now_ms();
    uint64_t done = 0;
    uint64_t cap = (uint64_t)tasks * 10u + 1000u; /* guard against corrupt-graph loops */
    void *item;
    while (done < cap && ready_dequeue(TASK_TYPE_CUBE, ORG_MODE_SINGLE, &item) == MPMC_OK) {
        complete_task((uint16_t)(uintptr_t)item);
        done++;
    }
    double d1 = now_ms();

    uint32_t incomplete = 0;
    for (uint32_t i = 1; i <= tasks; i++) {
        task_state s = atomic_load_explicit(&g_state_buf[i & RING_MASK], memory_order_relaxed);
        if (s.state != COMPLETED) incomplete++;
    }
    const char *verdict = (incomplete == 0 && done == tasks) ? "COMPLETE" : "DID-NOT-COMPLETE";
    printf("DRAIN %s drain_ms=%.4f drained=%llu incomplete=%u %s\n", CASE_NAME,
           d1 - d0, (unsigned long long)done, incomplete, verdict);
#endif
    return 0;
}
