/*
 * test_qwen3_macro_deps.c
 *
 * Validates macro/micro two-tier wiring in cases/qwen3_decode_macro.h:
 *   - 522 micro tasks, 534 intra-group deps via dep_group_memcpy
 *   - 180 inter-group macro_succeed edges, zero micro cross-group succeed
 *   - 102 macro groups with exit/entry bindings
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic \
 *       -I include -I cases tests/test_qwen3_macro_deps.c \
 *       -o /tmp/test_qwen3_macro
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DAG_TASK_H
#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H
#define DEP_STATIC_H
#define MACRO_GROUP_H

typedef enum { BFLOAT16 = 2, FLOAT32 = 4 } dtype_t;
typedef uint64_t Tensor;

#define RING_SIZE 65536
#define RING_MASK (RING_SIZE - 1)
#define AIC_CNT 60

typedef enum { TASK_TYPE_CUBE = 0, TASK_TYPE_VECTOR = 1, TASK_TYPE_MIX = 2 } task_type_t;
typedef enum { ORG_MODE_SINGLE = 0, ORG_MODE_SPMD_SYNC = 2 } org_mode_t;

struct task_desc {
    task_type_t type;
    org_mode_t mode;
    uint32_t count;
};

struct succ_list {
    uint16_t successor[64];
    struct succ_list *next;
};

static struct task_desc g_basic_buf[RING_SIZE];
static uint16_t g_task_id = 0;

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

#define MAX_MACRO_EDGES 512
static uint16_t g_macro_c[MAX_MACRO_EDGES];
static uint16_t g_macro_p[MAX_MACRO_EDGES];
static int g_macro_edge_n = 0;
static int g_macro_succeed_n = 0;
static int g_micro_succeed_n = 0;
static int g_dep_memcpy_n = 0;
static int g_macro_bind_n = 0;

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
    g_macro_bind_n++;
}

static inline void macro_gate_micro_entry(uint16_t micro_id, uint16_t gate_pred)
{
    (void)micro_id;
    (void)gate_pred;
}

static inline void macro_succeed(uint16_t macro_consumer, uint16_t macro_producer)
{
    if (g_macro_edge_n < MAX_MACRO_EDGES) {
        g_macro_c[g_macro_edge_n] = macro_consumer;
        g_macro_p[g_macro_edge_n] = macro_producer;
        g_macro_edge_n++;
    }
    g_macro_succeed_n++;
}

static inline void macro_enqueue_roots(uint16_t micro_id) { (void)micro_id; }

typedef struct {
    uint16_t pred;
    uint16_t succ_cnt;
    uint16_t succ[4];
} dep_slot_tpl_t;

static inline void dep_group_memcpy(uint16_t base_id, const dep_slot_tpl_t *tpl,
                                    uint16_t n_slots)
{
    (void)base_id;
    (void)tpl;
    (void)n_slots;
    g_dep_memcpy_n++;
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

#include "qwen3_decode_macro.h"

int main(void)
{
    macro_ring_init();
    aicpu_orchestration_entry(0);

    printf("micro_tasks=%u macro_groups=%d\n", (unsigned)g_task_id,
           g_macro_bind_n);
    printf("dep_memcpy_groups=%d macro_succeed=%d micro_succeed=%d macro_edges=%d\n",
           g_dep_memcpy_n, g_macro_succeed_n, g_micro_succeed_n, g_macro_edge_n);

    assert(g_task_id == QW3_TOTAL_TASKS);
    assert(g_macro_bind_n == (int)(QW3_NUM_TILES + QW3_USER_BATCH + QW3_NUM_TILES));
    assert(g_dep_memcpy_n == (int)(QW3_NUM_TILES + QW3_USER_BATCH + QW3_NUM_TILES));
    assert(g_micro_succeed_n == 0);
    assert(g_macro_succeed_n == 180);
    assert(g_macro_edge_n == 180);

    assert(g_micro_exit_to_macro[TID_QKNORM(0)] == MACRO_G1(0));
    assert(g_micro_exit_to_macro[TID_ONLINE(0)] == MACRO_G2(0));
    assert(g_micro_exit_to_macro[TID_DOWNRES(0)] == MACRO_G3(0));
    assert(g_macro_entry_micro[MACRO_G2(0)] == TID_ROPE(0));
    assert(g_macro_entry_micro[MACRO_G3(0)] == TID_OUTPROJ(0));

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
