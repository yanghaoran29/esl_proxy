/*
 * run_qwen3_topology.c - full spec thread topology for the tensormap qwen3 DAG.
 *
 * Realizes the 4 worker roles from specs/ as real threads, wired through the
 * lock-free MPMC queues (src/mpmc_queue.c) plus a per-executor inbox:
 *
 *   build (main)  --ready_q-->  DISPATCH x2  --inbox-->  EXECUTOR x120
 *                                                            |
 *                                                        complete_q
 *                                                            v
 *                                                       CUTTER x2  --ready_q--> (loop)
 *   MANAGER x1: mem_pool when2free processing (no-op here; nothing registered).
 *
 *   - dispatch: ready_dequeue a task id, hand it to an idle executor inbox.
 *   - executor: run the proxy kernel (no-op), then complete_enqueue the id.
 *     (1-slot inbox; a simplification of the spec's 2-slot PING-PONG executor.)
 *   - cutter: complete_dequeue, complete_task() — mark COMPLETED, decrement each
 *     successor's predecessor, re-enqueue any that become ready. predecessor and
 *     g_completed_cnt are atomic, so two cutters are safe; g_state_buf[t] is
 *     written only by the single cutter that processes t.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -pthread -I include -I cases \
 *       tests/run_qwen3_topology.c src/mpmc_queue.c -o /tmp/run_topo
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

#define MANAGER_CNT  1
#define DISPATCH_CNT 2
#define EXECUTOR_CNT 120
#define CUTTER_CNT   2

#define INBOX_EMPTY 0xFFFFFFFFu

static _Atomic uint32_t g_inbox[EXECUTOR_CNT]; /* dispatch -> executor slot */
static atomic_bool g_done = false;
static uint32_t g_total = 0;
static _Atomic uint64_t g_executed = 0;

static void *manager_main(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        mem_pool_process_when2free(&g_mem_pool);
        sched_yield();
    }
    return NULL;
}

static void *dispatch_main(void *arg) {
    (void)arg;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        void *item;
        if (ready_dequeue(TASK_TYPE_CUBE, ORG_MODE_SINGLE, &item) != MPMC_OK) {
            sched_yield();
            continue;
        }
        uint32_t tid = (uint32_t)(uintptr_t)item;
        for (;;) { /* place into the first idle executor inbox */
            int placed = 0;
            for (int e = 0; e < EXECUTOR_CNT; e++) {
                uint32_t expected = INBOX_EMPTY;
                if (atomic_compare_exchange_strong_explicit(
                        &g_inbox[e], &expected, tid,
                        memory_order_acq_rel, memory_order_relaxed)) {
                    placed = 1;
                    break;
                }
            }
            if (placed) break;
            sched_yield(); /* all busy; a held task is never complete, so !done */
        }
    }
    return NULL;
}

static void *executor_main(void *arg) {
    int e = (int)(intptr_t)arg;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        uint32_t t = atomic_load_explicit(&g_inbox[e], memory_order_acquire);
        if (t == INBOX_EMPTY) {
            sched_yield();
            continue;
        }
        /* proxy "execution" of task t is a no-op */
        atomic_store_explicit(&g_inbox[e], INBOX_EMPTY, memory_order_release); /* idle */
        while (complete_enqueue((void *)(uintptr_t)t) != MPMC_OK) { /* spin */ }
        atomic_fetch_add_explicit(&g_executed, 1, memory_order_relaxed);
    }
    return NULL;
}

static void *cutter_main(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_completed_cnt, memory_order_relaxed) < g_total) {
        void *item;
        if (complete_dequeue(&item) == MPMC_OK) {
            complete_task((uint16_t)(uintptr_t)item);
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
    for (int e = 0; e < EXECUTOR_CNT; e++) {
        atomic_store_explicit(&g_inbox[e], INBOX_EMPTY, memory_order_relaxed);
    }

    aicpu_orchestration_entry(0);
    g_total = g_task_id;
    printf("built tasks = %u  |  topology: %d manager + %d dispatch + %d executor + %d cutter\n",
           g_total, MANAGER_CNT, DISPATCH_CNT, EXECUTOR_CNT, CUTTER_CNT);

    double t0 = now_sec();
    pthread_t manager, dispatch[DISPATCH_CNT], cutter[CUTTER_CNT], executor[EXECUTOR_CNT];
    pthread_create(&manager, NULL, manager_main, NULL);
    for (int i = 0; i < CUTTER_CNT; i++) pthread_create(&cutter[i], NULL, cutter_main, NULL);
    for (int i = 0; i < EXECUTOR_CNT; i++) {
        pthread_create(&executor[i], NULL, executor_main, (void *)(intptr_t)i);
    }
    for (int i = 0; i < DISPATCH_CNT; i++) pthread_create(&dispatch[i], NULL, dispatch_main, NULL);

    for (int i = 0; i < DISPATCH_CNT; i++) pthread_join(dispatch[i], NULL);
    for (int i = 0; i < EXECUTOR_CNT; i++) pthread_join(executor[i], NULL);
    for (int i = 0; i < CUTTER_CNT; i++) pthread_join(cutter[i], NULL);
    pthread_join(manager, NULL);
    double t1 = now_sec();

    printf("executed (executors) = %llu\n", (unsigned long long)atomic_load(&g_executed));
    printf("completed (cutters)  = %llu\n", (unsigned long long)atomic_load(&g_completed_cnt));
    printf("drain wall time      = %.3f ms\n", (t1 - t0) * 1e3);

    assert(atomic_load(&g_executed) == g_total);
    assert(atomic_load(&g_completed_cnt) == g_total);
    for (uint32_t i = 1; i <= g_total; i++) {
        task_state s = atomic_load_explicit(&g_state_buf[i & RING_MASK], memory_order_relaxed);
        assert(s.state == COMPLETED);
    }

    printf("\nESL_PROXY TOPOLOGY RUN OK: %u tasks through %d-stage pipeline "
           "(manager/dispatch/executor/cutter)\n", g_total, 4);
    return 0;
}
