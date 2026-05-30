/*
 * run_qwen3_tensormap_mt.c - multi-threaded driver for the tensormap qwen3
 * orchestration.
 *
 * Build phase (main thread, single-threaded): aicpu_orchestration_entry()
 * constructs the whole DAG; roots land in the ready queue.
 *
 * Drain phase (concurrent):
 *   - N worker threads pop ready task ids (ready_dequeue), "execute" the proxy
 *     kernel (a no-op), and hand the id to the cutter (complete_enqueue).
 *   - 1 cutter thread pops completions (complete_dequeue) and complete_task()s
 *     them: mark COMPLETED, decrement each successor's predecessor count, and
 *     re-enqueue any that become ready. All dependency-state mutation lives in
 *     this single thread, so the only shared concurrency is the two lock-free
 *     BlkRing queues + the atomic done flag.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -pthread -I include -I cases \
 *       tests/run_qwen3_tensormap_mt.c src/mpmc_queue.c -o /tmp/run_qwen3_mt
 */

#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "qwen3_decode_tensormap.h"

#define NUM_WORKERS 8

static atomic_bool g_done = false;
static uint32_t g_total = 0;
static _Atomic uint64_t g_executed = 0; /* tasks "run" by workers (stats only) */

static void *worker_main(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        void *item;
        if (ready_dequeue(TASK_TYPE_CUBE, ORG_MODE_SINGLE, &item) == MPMC_OK) {
            /* proxy "execution" of task (uint16_t)(uintptr_t)item is a no-op */
            atomic_fetch_add_explicit(&g_executed, 1, memory_order_relaxed);
            while (complete_enqueue(item) != MPMC_OK) { /* spin (never full) */ }
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void *cutter_main(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_completed_cnt, memory_order_relaxed) < g_total) {
        void *item;
        if (complete_dequeue(&item) == MPMC_OK) {
            complete_task((uint16_t)(uintptr_t)item); /* mark + propagate + re-enqueue */
        } else {
            sched_yield();
        }
    }
    atomic_store_explicit(&g_done, true, memory_order_release);
    return NULL;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    mem_pool_init(&g_mem_pool, (void *)(uintptr_t)0x10000000u, (size_t)1 << 42);
    init_global_queues();

    aicpu_orchestration_entry(0);
    g_total = g_task_id;
    printf("built tasks = %u, workers = %d\n", g_total, NUM_WORKERS);

    double t0 = now_sec();
    pthread_t cutter;
    pthread_t workers[NUM_WORKERS];
    pthread_create(&cutter, NULL, cutter_main, NULL);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_create(&workers[i], NULL, worker_main, NULL);
    }
    pthread_join(cutter, NULL);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
    double t1 = now_sec();

    printf("executed (workers) = %llu\n", (unsigned long long)atomic_load(&g_executed));
    printf("completed (cutter) = %llu\n", (unsigned long long)atomic_load(&g_completed_cnt));
    printf("drain wall time    = %.3f ms\n", (t1 - t0) * 1e3);

    assert(atomic_load(&g_completed_cnt) == g_total);
    assert(atomic_load(&g_executed) == g_total);
    for (uint32_t i = 1; i <= g_total; i++) {
        task_state s = atomic_load_explicit(&g_state_buf[i & RING_MASK], memory_order_relaxed);
        assert(s.state == COMPLETED);
    }

    printf("\nESL_PROXY MT RUN OK: %u tasks scheduled to completion across %d worker threads + 1 cutter\n",
           g_total, NUM_WORKERS);
    return 0;
}
