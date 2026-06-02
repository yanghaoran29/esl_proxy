/*
 * test_qwen3_manual_deps.c — count dependency edges from cases/qwen3_decode.h.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic \
 *       -I include -I cases tests/test_qwen3_manual_deps.c \
 *       -o /tmp/test_qwen3_manual
 */

#define _POSIX_C_SOURCE 199309L

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define DAG_TASK_H
#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H

typedef enum { BFLOAT16 = 2, FLOAT32 = 4 } dtype_t;
typedef uint64_t Tensor;

#define RING_SIZE 65536
#define RING_MASK (RING_SIZE - 1)

typedef enum { TASK_TYPE_CUBE = 0, TASK_TYPE_VECTOR = 1, TASK_TYPE_MIX = 2 } task_type_t;
typedef enum { ORG_MODE_SINGLE = 0, ORG_MODE_SPMD_SYNC = 2 } org_mode_t;

struct task_desc {
    task_type_t type;
    org_mode_t mode;
    uint32_t count;
};

static struct task_desc g_basic_buf[RING_SIZE];
static uint16_t g_task_id = 0;

static int g_succeed_n = 0;
static int g_batch_succeed_n = 0;

static inline void spin_wait(void) {}
static inline void add_input(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_output(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_inout(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_scalar(uint16_t tid, int64_t s) { (void)tid; (void)s; }
static inline bool try_new_task(uint32_t id) { (void)id; return false; }
static inline void ready_enqueue(task_type_t type, org_mode_t mode, uint16_t tid)
{
    (void)type;
    (void)mode;
    (void)tid;
}

static inline bool batch_succeed(uint16_t cnt, uint16_t task_id[], uint16_t target)
{
    (void)task_id;
    (void)target;
    g_batch_succeed_n += (int)cnt;
    return true;
}

static inline bool succeed(uint16_t task_id, uint16_t target)
{
    (void)task_id;
    (void)target;
    g_succeed_n++;
    return true;
}

static inline void submit(uint16_t tid) { (void)tid; }
static inline void batch_submit(uint16_t cnt, uint16_t task_id[])
{
    (void)cnt;
    (void)task_id;
}

static uint64_t g_alloc_bump = 0x100000u;
static inline Tensor alloc_tensors(uint32_t shape[], int dim, int bytes)
{
    uint64_t a = g_alloc_bump;
    uint64_t sz = (uint64_t)shape[0] * (uint64_t)shape[1] * (uint64_t)dim * (uint64_t)bytes;
    if (sz < 64u)
        sz = 64u;
    g_alloc_bump += (sz + 63u) & ~(uint64_t)63u;
    return a;
}

static inline void add_duration(uint16_t tid, int64_t d) { (void)tid; (void)d; }

#include "qwen3_decode.h"

int main(void)
{
    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    aicpu_orchestration_entry(0);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    const double orch_us =
        (double)(t1.tv_sec - t0.tv_sec) * 1e6 +
        (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
    const int total_edges = g_succeed_n + g_batch_succeed_n;

    printf("case=qwen3_decode.h\n");
    printf("tasks=%u\n", (unsigned)g_task_id);
    printf("succeed=%d batch_succeed=%d total_edges=%d\n", g_succeed_n,
           g_batch_succeed_n, total_edges);
    printf("orchestration_time=%.0f us\n", orch_us);

    assert(g_task_id == 522u);
    assert(g_succeed_n == 870); /* 786 - 6 (drop v_proj->qk_norm) + 90 (softmax->online) */
    assert(g_batch_succeed_n == 18); /* 6 tiles * batch_succeed(3,...) */
    assert(total_edges == 888);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
