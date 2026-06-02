/*
 * test_qwen3_static_deps.c — validates task-group template + static intra deps +
 * macro/micro inter-group wiring in cases/qwen3_decode_static.h.
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic \
 *       -I include -I cases tests/test_qwen3_static_deps.c \
 *       -o /tmp/test_qwen3_static
 */

#define _POSIX_C_SOURCE 199309L

#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DAG_TASK_H
#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H
#define MACRO_GROUP_H
#define ORCH_BUILD_H

typedef enum { BFLOAT16 = 2, FLOAT32 = 4 } dtype_t;
typedef uint64_t Tensor;

#define RING_SIZE 65536
#define RING_MASK (RING_SIZE - 1)
#define AIC_CNT 60

typedef enum { TASK_TYPE_CUBE = 0, TASK_TYPE_VECTOR = 1, TASK_TYPE_MIX = 2 } task_type_t;
typedef enum { ORG_MODE_SINGLE = 0, ORG_MODE_SPMD_SYNC = 2 } org_mode_t;

struct task_desc {
    uint16_t       id;          /* ring-buffer task id */
    task_type_t    type;        /* CUBE / VECTOR / MIX */
    org_mode_t     mode;        /* SINGLE / GROUP / SPMD_SYNC / SPMD_ASYNC */
    void          *kernel;      /* device kernel entry, NULL if unset */
    uint32_t       index;       /* SPMD base block index */
    uint32_t       count;       /* SPMD instance count (block_num) */
    uint64_t       data[16];    /* tensor addresses (Tensor handles) */
    int64_t        scalar[32];  /* scalar kernel arguments */
    uint16_t       tensor_cnt;  /* number of valid data[] entries */
    uint16_t       scalar_cnt;  /* number of valid scalar[] entries */
    uint16_t       duration;    /* estimated kernel cycles (low 16 bits) */
};

static struct task_desc g_basic_buf[RING_SIZE];
static uint16_t g_task_id = 0;

/* ---- orch_rewire.h content (merged into orch_build.h) — inline stub ---- */
typedef enum { RW_IN = 0, RW_OUT, RW_INOUT, RW_SCALAR } rewire_kind_t;
typedef struct { uint8_t slot; uint8_t kind; uint8_t ref; } rewire_op_t;
static inline void rewire_group_apply(uint16_t base, const rewire_op_t *ops, size_t n_ops,
                                      const uint64_t *tensors, const int64_t *scalars)
{
    uint16_t cur = 0xffu;
    struct task_desc *d = NULL;
    for (size_t i = 0; i < n_ops; i++) {
        if (ops[i].slot != cur) {
            cur = ops[i].slot;
            d = &g_basic_buf[(base + cur) & RING_MASK];
            d->tensor_cnt = 0;
            d->scalar_cnt = 0;
        }
        if (ops[i].kind == RW_SCALAR)
            d->scalar[d->scalar_cnt++] = scalars[ops[i].ref];
        else
            d->data[d->tensor_cnt++] = tensors[ops[i].ref];
    }
}

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

static int g_dep_memcpy_n = 0;
static int g_macro_succeed_n = 0;
static int g_micro_succeed_n = 0;
static int g_dep_install_n = 0;

/* Same totals as cases/qwen3_decode.h: 618 intra + 180 macro inter + 90 direct
 * v_proj->rope = 888. */
enum {
    QW3_EXPECT_TASKS = 522,
    QW3_EXPECT_DEP_MEMCPY = 102, /* 6 + 90 + 6 groups */
    QW3_EXPECT_INTRA_EDGES = 618,
    QW3_EXPECT_INTER_EDGES = 180,
};

static uint16_t g_macro_entry_micro[128];
static uint16_t g_micro_exit_to_macro[RING_SIZE];

static inline void macro_ring_init(void)
{
    for (int i = 0; i < 128; i++)
        g_macro_entry_micro[i] = 0;
    for (int i = 0; i < (int)RING_SIZE; i++)
        g_micro_exit_to_macro[i] = 0;
}

static inline void macro_group_bind(uint16_t macro_id, uint16_t entry_micro,
                                    uint16_t exit_micro)
{
    g_macro_entry_micro[macro_id] = entry_micro;
    g_micro_exit_to_macro[exit_micro] = macro_id;
}

static inline void macro_gate_micro_entry(uint16_t micro_id, uint16_t gate_pred)
{
    (void)micro_id;
    (void)gate_pred;
}

static inline void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer)
{
    (void)macro_consumer;
    (void)macro_producer;
    g_macro_succeed_n++;
}

static inline void macro_succeed(uint16_t macro_consumer, uint16_t macro_producer)
{
    macro_succeed_build(macro_consumer, macro_producer);
}

static inline void macro_enqueue_roots(uint16_t micro_id) { (void)micro_id; }

typedef struct {
    uint16_t pred;
    uint16_t succ_cnt;
    uint16_t succ[4];
} dep_slot_tpl_t;

static inline void dep_group_install(uint16_t base_id, const dep_slot_tpl_t *tpl,
                                     uint16_t n_slots)
{
    (void)base_id;
    (void)tpl;
    (void)n_slots;
    g_dep_memcpy_n++;
}

/* single non-atomic micro edge (used for the v_proj->rope direct edge). */
static inline void dep_install(uint16_t consumer, uint16_t producer)
{
    (void)consumer;
    (void)producer;
    g_dep_install_n++;
}

static inline void orch_build_begin(void) {}
static inline void orch_build_end(void) {}
static inline void orch_build_flush(void) {}

static inline void task_claim_range(uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        g_task_id++;
}

static inline void orch_defer_submit(uint16_t tid) { (void)tid; }
static inline void orch_defer_batch_submit(uint16_t cnt, const uint16_t task_id[])
{
    (void)cnt;
    (void)task_id;
}
static inline void orch_defer_root(uint16_t tid) { (void)tid; }

static inline void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred)
{
    (void)micro_id;
    (void)gate_pred;
}

static inline void dep_group_memcpy(uint16_t base_id, const dep_slot_tpl_t *tpl,
                                    uint16_t n_slots)
{
    dep_group_install(base_id, tpl, n_slots);
}

static inline bool succeed(uint16_t c, uint16_t p)
{
    (void)c;
    (void)p;
    g_micro_succeed_n++;
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

/* ---- task_group_tpl.h content (merged into orch_build.h) — inline stub ---- */
#define TASK_GROUP_TPL_MAX 10
typedef struct {
    struct task_desc slots[TASK_GROUP_TPL_MAX];
    uint16_t n_slots;
} task_group_tpl_t;

static inline void task_group_tpl_apply(const task_group_tpl_t *tpl, uint16_t base_id)
{
    enum {
        meta_bytes = offsetof(struct task_desc, data),
        tail_off   = offsetof(struct task_desc, tensor_cnt),
        tail_bytes = sizeof(struct task_desc) - tail_off,
    };
    for (uint16_t i = 0; i < tpl->n_slots; i++) {
        const struct task_desc *src = &tpl->slots[i];
        struct task_desc *dst = &g_basic_buf[(base_id + i) & RING_MASK];
        memcpy(dst, src, meta_bytes);
        memcpy((char *)dst + tail_off, (const char *)src + tail_off, tail_bytes);
    }
}

#include "qwen3_decode_static.h"

int main(void)
{
    struct timespec t0, t1;

    macro_ring_init();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    aicpu_orchestration_entry(0);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    const double orch_us =
        (double)(t1.tv_sec - t0.tv_sec) * 1e6 +
        (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;

    printf("micro_tasks=%u dep_group=%d v_proj->rope=%d\n", (unsigned)g_task_id,
           g_dep_memcpy_n, g_dep_install_n);
    printf("inter_edges=%d total_edges=%d\n", g_macro_succeed_n,
           QW3_EXPECT_INTRA_EDGES + g_macro_succeed_n + g_dep_install_n);
    printf("orchestration_time=%.0f us\n", orch_us);

    assert(g_task_id == QW3_EXPECT_TASKS);
    assert(g_dep_memcpy_n == QW3_EXPECT_DEP_MEMCPY);
    assert(g_micro_succeed_n == 0);
    assert(g_macro_succeed_n == QW3_EXPECT_INTER_EDGES);
    /* one direct v_proj->rope micro edge per attention batch. */
    assert(g_dep_install_n == QW3_USER_BATCH);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
