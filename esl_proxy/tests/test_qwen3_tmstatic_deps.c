/*
 * test_qwen3_tmstatic_deps.c
 *
 * Compiles and RUNS cases/qwen3_decode_tensormap_static.h with recording stubs to
 * verify the hybrid wiring: intra-group dependencies come from the static
 * g{1,2,3}_dep blobs (recorded via dep_group_install), inter-group dependencies
 * come from the tensormap boundary lookups (recorded via succeed()).
 *
 * Build:
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -DUSE_TENSORMAP \
 *       -I include -I cases tests/test_qwen3_tmstatic_deps.c -o /tmp/test_qwen3_tmstatic
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Neutralize esl_proxy headers that need the full project; supply stubs. */
#define DAG_TASK_H
#define DAG_RING_BUF_H
#define DAG_MEM_POOL_H
#define DAG_MPMC_QUEUE_H
#define DEP_STATIC_H
#define ORCH_REWIRE_H
#define ORCH_BUILD_H
#define TASK_GROUP_TPL_H

typedef enum { BFLOAT16 = 2, FLOAT32 = 4 } dtype_t;

typedef struct {
    uint64_t base;
    uint32_t storage[2];
    uint32_t shapes[2];
    uint32_t offsets[2];
    uint32_t strides[2];
    dtype_t  dtype;
} Tensor;

static inline uint64_t tensor_base(Tensor t) { return t.base; }

static inline Tensor tensor_from_base(uint64_t base)
{
    Tensor t = {0};
    t.base = base;
    return t;
}

static inline Tensor tensor_make_2d(uint64_t base, uint32_t d0, uint32_t d1, dtype_t dtype)
{
    Tensor t;
    t.base = base;
    t.storage[0] = t.shapes[0] = d0;
    t.storage[1] = t.shapes[1] = d1;
    t.offsets[0] = t.offsets[1] = 0;
    t.strides[0] = d1;
    t.strides[1] = 1;
    t.dtype = dtype;
    return t;
}

static inline Tensor tensor_view(Tensor t, uint32_t row0, uint32_t nrows)
{
    t.offsets[0] += row0;
    t.shapes[0] = nrows;
    return t;
}

typedef enum { TASK_TYPE_CUBE = 0, TASK_TYPE_VECTOR = 1, TASK_TYPE_MIX = 2 } task_type_t;
typedef enum { ORG_MODE_SINGLE = 0, ORG_MODE_SPMD_SYNC = 2 } org_mode_t;

#define RING_SIZE 65536
#define RING_MASK (RING_SIZE - 1)

struct task_desc {
    task_type_t type;
    org_mode_t  mode;
    uint32_t    count;
    uint64_t    data[16];
    int64_t     scalar[32];
    uint16_t    tensor_cnt;
    uint16_t    scalar_cnt;
    uint16_t    duration;
};

static struct task_desc g_basic_buf[RING_SIZE];
static uint16_t g_task_id = 0;
static uint16_t g_min_uncomplete_task = 0;
static int32_t g_dur[RING_SIZE];

static inline void spin_wait(void) {}
static inline void add_input(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_output(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_inout(uint16_t tid, Tensor t) { (void)tid; (void)t; }
static inline void add_scalar(uint16_t tid, int64_t s) { (void)tid; (void)s; }
static inline void add_duration(uint16_t tid, int64_t d) { g_dur[tid] = (int32_t)d; }
static inline bool try_new_task(uint32_t id) { (void)id; return false; }
static inline void submit(uint16_t tid) { (void)tid; }

#define MAX_EDGES 1000000
static uint16_t g_edge_c[MAX_EDGES];
static uint16_t g_edge_p[MAX_EDGES];
static int g_edge_n = 0;
static int g_intra_n = 0;
static int g_inter_n = 0;

static inline void record_edge(uint16_t c, uint16_t p)
{
    if (g_edge_n < MAX_EDGES) {
        g_edge_c[g_edge_n] = c;
        g_edge_p[g_edge_n] = p;
        g_edge_n++;
    }
}

/* tensormap.h resolves an inter-group edge by calling succeed(consumer, producer). */
static inline bool succeed(uint16_t c, uint16_t p)
{
    record_edge(c, p);
    g_inter_n++;
    return true;
}

#include "tensormap.h"

static uint64_t g_alloc_bump = 0x100000u;
static inline Tensor alloc_tensors(uint32_t shape[], int dim, int bytes)
{
    uint64_t a = g_alloc_bump;
    uint64_t sz = (uint64_t)shape[0] * (uint64_t)shape[1] * (uint64_t)dim * (uint64_t)bytes;
    if (sz < 64u) sz = 64u;
    g_alloc_bump += (sz + 63u) & ~(uint64_t)63u;
    return tensor_make_2d(a, shape[0], shape[1], (dtype_t)bytes);
}

/* --- dep_static.h stub: record intra edges from the static blob ----------- */
typedef struct {
    uint16_t pred;
    uint16_t succ_cnt;
    uint16_t succ[4];
} dep_slot_tpl_t;

/* --- orch_rewire.h stub --------------------------------------------------- */
typedef enum { RW_IN = 0, RW_OUT, RW_INOUT, RW_SCALAR } rewire_kind_t;
typedef struct { uint8_t slot; uint8_t kind; uint8_t ref; } rewire_op_t;
static inline void rewire_group_apply(uint16_t base, const rewire_op_t *ops, size_t n_ops,
                                      const uint64_t *tensors, const int64_t *scalars)
{
    (void)base; (void)ops; (void)n_ops; (void)tensors; (void)scalars;
}

/* --- task_group_tpl.h stub ------------------------------------------------ */
#define TASK_GROUP_TPL_MAX 10
typedef struct {
    struct task_desc slots[TASK_GROUP_TPL_MAX];
    uint16_t n_slots;
} task_group_tpl_t;
static inline void task_group_tpl_apply(const task_group_tpl_t *tpl, uint16_t base_id)
{
    for (uint16_t i = 0; i < tpl->n_slots; i++) {
        struct task_desc *dst = &g_basic_buf[(base_id + i) & RING_MASK];
        dst->type = tpl->slots[i].type;
        dst->mode = tpl->slots[i].mode;
        dst->count = tpl->slots[i].count;
        g_dur[(base_id + i) & RING_MASK] = (int32_t)tpl->slots[i].duration;
    }
}

/* --- orch_build.h stub ---------------------------------------------------- */
static inline void task_claim_range(uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) g_task_id++;
}
static inline void dep_group_install(uint16_t base_id, const dep_slot_tpl_t *tpl, uint16_t n_slots)
{
    for (uint16_t i = 0; i < n_slots; i++) {
        for (uint16_t j = 0; j < tpl[i].succ_cnt; j++) {
            record_edge((uint16_t)(base_id + tpl[i].succ[j]), (uint16_t)(base_id + i));
            g_intra_n++;
        }
    }
}
static inline void orch_defer_submit(uint16_t tid) { (void)tid; }
static inline void orch_defer_batch_submit(uint16_t cnt, const uint16_t task_id[]) { (void)cnt; (void)task_id; }
static inline void orch_defer_root(uint16_t tid) { (void)tid; }
static inline void orch_build_flush(void) {}

#include "qwen3_decode_tensormap_static.h"

enum {
    DUR_RMSNORM = 23950, DUR_QPROJ = 26060, DUR_KPROJ = 18170, DUR_VPROJ = 17890,
    DUR_QKNORM = 13190, DUR_ROPE = 9480, DUR_ONLINE = 20820, DUR_OUTPROJ = 40750,
    DUR_SILU = 2820, DUR_DOWN = 72220,
};

static int producers_of(uint16_t c, uint16_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < g_edge_n; i++)
        if (g_edge_c[i] == c && n < cap) out[n++] = g_edge_p[i];
    return n;
}

static int first_tid_with_dur(int32_t d)
{
    for (int t = 1; t <= (int)g_task_id; t++)
        if (g_dur[t] == d) return t;
    return -1;
}

static int last_tid_with_dur(int32_t d)
{
    int last = -1;
    for (int t = 1; t <= (int)g_task_id; t++)
        if (g_dur[t] == d) last = t;
    return last;
}

int main(void)
{
    static uint16_t pr[4096];
    int n, i, nq, nk, nv, nqn, nvp, no;

    aicpu_orchestration_entry(0);

    printf("tasks=%u edges=%d (intra=%d inter=%d)\n", (unsigned)g_task_id, g_edge_n,
           g_intra_n, g_inter_n);

    assert(g_task_id == 522u);
    assert(g_edge_n < MAX_EDGES);
    /* Intra via static blobs: 30 + 540 + 48 = 618. */
    assert(g_intra_n == 618);
    /* Inter via tensormap: G1->G2 90*3 (qk_norm x2 + v_proj) + G2->G3 sum(cur_valid)=90. */
    assert(g_inter_n == 270 + 90);

    /* G1 intra: qk_norm reads only q/k_proj, so it depends on q_proj + k_proj (no v_proj). */
    int qk = first_tid_with_dur(DUR_QKNORM);
    assert(qk >= 0);
    n = producers_of((uint16_t)qk, pr, 4096);
    nq = nk = nv = 0;
    for (i = 0; i < n; i++) {
        if (g_dur[pr[i]] == DUR_QPROJ) nq++;
        else if (g_dur[pr[i]] == DUR_KPROJ) nk++;
        else if (g_dur[pr[i]] == DUR_VPROJ) nv++;
    }
    printf("qk_norm[t%d]: producers=%d (q=%d k=%d v=%d)\n", qk, n, nq, nk, nv);
    assert(n == 2 && nq == 1 && nk == 1 && nv == 0);

    /* G1->G2 inter: rope depends on its tile's qk_norm (x2: q+k norm) and v_proj. */
    int rope = first_tid_with_dur(DUR_ROPE);
    assert(rope >= 0);
    n = producers_of((uint16_t)rope, pr, 4096);
    nqn = nvp = 0;
    for (i = 0; i < n; i++) {
        if (g_dur[pr[i]] == DUR_QKNORM) nqn++;
        else if (g_dur[pr[i]] == DUR_VPROJ) nvp++;
    }
    printf("rope[t%d]: producers=%d (qk_norm=%d v_proj=%d)\n", rope, n, nqn, nvp);
    assert(n == 3 && nqn == 2 && nvp == 1);

    /* G2->G3 inter: first-tile out_proj fans in 16 online_softmax (its tile rows). */
    int op = first_tid_with_dur(DUR_OUTPROJ);
    assert(op >= 0);
    n = producers_of((uint16_t)op, pr, 4096);
    no = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_ONLINE) no++;
    printf("out_proj[t%d]: producers=%d (online_softmax=%d)\n", op, n, no);
    assert(n == 16 && no == 16);

    /* Last tile out_proj: cur_valid = 90 - 80 = 10 online_softmax producers. */
    int lop = last_tid_with_dur(DUR_OUTPROJ);
    n = producers_of((uint16_t)lop, pr, 4096);
    no = 0;
    for (i = 0; i < n; i++) if (g_dur[pr[i]] == DUR_ONLINE) no++;
    printf("out_proj[t%d]: producers=%d (online_softmax=%d)\n", lop, n, no);
    assert(n == 10 && no == 10);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
