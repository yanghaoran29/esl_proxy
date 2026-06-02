/*
 * orch_build.h - Single-threaded orchestration build-phase toolkit.
 *
 * Consolidates everything used to construct the static task graph (formerly the
 * separate dep_static.h / orch_rewire.h / task_group_tpl.h headers):
 *   - dep_slot_tpl_t + non-atomic intra-group dependency install,
 *   - rewire_op_t + rewire_group_apply (static tensor/scalar wiring blobs),
 *   - task_group_tpl_t + task_group_tpl_apply (task-group descriptor templates),
 *   - claim / deferred submit / flush helpers (implemented in orch_build.c).
 *
 * Phase 0: measure before worker threads (see main.c).
 * Phase 1: plain task claim + dep/macro install (no CAS retry loops).
 * Phase 3: defer batch_submit/submit/roots until orch_build_flush().
 *
 * The runtime macro/micro propagation layer stays in macro_group.h (used by the
 * worker path, not the build path).
 */

#ifndef ORCH_BUILD_H
#define ORCH_BUILD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "conf.h"
#include "ring_buf.h"
#include "task.h"

/* ===========================================================================
 * Intra-group dependency install (non-atomic; build-phase only).
 *
 * Equivalent to succeed(consumer, producer) without CAS on g_state_buf. Intended
 * for single-threaded orchestrator use while wiring fixed intra-group templates.
 * ========================================================================== */

typedef struct {
    uint16_t pred;
    uint16_t succ_cnt;
    uint16_t succ[4];
} dep_slot_tpl_t;

static inline void dep_group_memcpy(uint16_t base_id, const dep_slot_tpl_t *tpl,
                                  uint16_t n_slots)
{
    for (uint16_t i = 0; i < n_slots; i++) {
        uint16_t tid = (uint16_t)(base_id + i);
        int slot = (int)(tid & RING_MASK);

        atomic_store_explicit(&g_predecessor_buf[slot], tpl[i].pred,
                              memory_order_relaxed);

        task_state st =
            atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
        st.successor_cnt = tpl[i].succ_cnt;
        atomic_store_explicit(&g_state_buf[slot], st, memory_order_relaxed);

        for (uint16_t j = 0; j < tpl[i].succ_cnt; j++) {
            g_successor_buf[slot].successor[j] = (uint16_t)(base_id + tpl[i].succ[j]);
        }
    }
}

static inline void dep_install(uint16_t consumer, uint16_t producer)
{
    int prod_slot = (int)(producer & RING_MASK);
    int cons_slot = (int)(consumer & RING_MASK);

    task_state st =
        atomic_load_explicit(&g_state_buf[prod_slot], memory_order_relaxed);
    int idx = (int)st.successor_cnt;
    struct succ_list *ptr = &g_successor_buf[prod_slot];
    while (idx >= SUCC_NODE_CNT) {
        idx -= SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    ptr->successor[idx] = consumer;
    st.successor_cnt++;
    atomic_store_explicit(&g_state_buf[prod_slot], st, memory_order_relaxed);

    atomic_fetch_add_explicit(&g_predecessor_buf[cons_slot], 1,
                              memory_order_relaxed);
}

static inline void dep_install_bulk(const uint16_t *edges, size_t n_edges)
{
    for (size_t i = 0; i < n_edges; i += 2u) {
        dep_install(edges[i], edges[i + 1u]);
    }
}

static inline void dep_install_group_template(uint16_t base_id,
                                              const uint16_t (*rel_edges)[2],
                                              size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dep_install((uint16_t)(base_id + rel_edges[i][0]),
                    (uint16_t)(base_id + rel_edges[i][1]));
    }
}

/* ===========================================================================
 * Static tensor/scalar wiring blobs.
 * ========================================================================== */

typedef enum {
    RW_IN = 0,
    RW_OUT,
    RW_INOUT,
    RW_SCALAR,
} rewire_kind_t;

typedef struct {
    uint8_t slot;
    uint8_t kind;
    uint8_t ref;
} rewire_op_t;

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
        if (ops[i].kind == RW_SCALAR) {
            d->scalar[d->scalar_cnt++] = scalars[ops[i].ref];
        } else {
            d->data[d->tensor_cnt++] = tensors[ops[i].ref];
        }
    }
}

/* ===========================================================================
 * Task-group descriptor templates.
 *
 * Build a group's task_desc slots once (type/mode/duration/layout counts),
 * then per instance: task_claim_range + task_group_tpl_apply()
 * + rewire tensors/scalars (rewire_group_apply / add_*) + dep_group_install + submit.
 * ========================================================================== */

#ifndef TASK_GROUP_TPL_MAX
#define TASK_GROUP_TPL_MAX 10
#endif

typedef struct {
    struct task_desc slots[TASK_GROUP_TPL_MAX];
    uint16_t n_slots;
} task_group_tpl_t;

static inline void tpl_touch_slot(task_group_tpl_t *tpl, int off)
{
    if ((uint16_t)(off + 1) > tpl->n_slots)
        tpl->n_slots = (uint16_t)(off + 1);
}

static inline void tpl_set_type(task_group_tpl_t *tpl, int off, task_type_t type)
{
    tpl->slots[off].type = type;
    tpl_touch_slot(tpl, off);
}

static inline void tpl_set_block_num(task_group_tpl_t *tpl, int off, uint32_t count)
{
    tpl->slots[off].mode = ORG_MODE_SPMD_SYNC;
    tpl->slots[off].count = count;
    tpl_touch_slot(tpl, off);
}

static inline void tpl_add_input(task_group_tpl_t *tpl, int off, Tensor t)
{
    struct task_desc *d = &tpl->slots[off];
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    d->data[d->tensor_cnt++] = tensor_base(t);
#else
    d->data[d->tensor_cnt++] = (uint64_t)t;
#endif
    tpl_touch_slot(tpl, off);
}

static inline void tpl_add_output(task_group_tpl_t *tpl, int off, Tensor t)
{
    struct task_desc *d = &tpl->slots[off];
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    d->data[d->tensor_cnt++] = tensor_base(t);
#else
    d->data[d->tensor_cnt++] = (uint64_t)t;
#endif
    tpl_touch_slot(tpl, off);
}

static inline void tpl_add_inout(task_group_tpl_t *tpl, int off, Tensor t)
{
    struct task_desc *d = &tpl->slots[off];
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    d->data[d->tensor_cnt++] = tensor_base(t);
#else
    d->data[d->tensor_cnt++] = (uint64_t)t;
#endif
    tpl_touch_slot(tpl, off);
}

static inline void tpl_add_scalar(task_group_tpl_t *tpl, int off, int64_t s)
{
    struct task_desc *d = &tpl->slots[off];
    d->scalar[d->scalar_cnt++] = s;
    tpl_touch_slot(tpl, off);
}

static inline void tpl_add_duration(task_group_tpl_t *tpl, int off, int64_t d)
{
    tpl->slots[off].duration = (uint16_t)d;
    tpl_touch_slot(tpl, off);
}

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
        /* skip data[] / scalar[] — filled by rewire_group_apply or add_* */
        memcpy((char *)dst + tail_off, (const char *)src + tail_off, tail_bytes);
    }
}

static inline void tpl_patch_tensor(uint16_t base_id, int off, int data_idx, Tensor t)
{
#if defined(USE_TENSORMAP) && !defined(TENSORMAP_WHOLE_BUFFER)
    g_basic_buf[(base_id + (uint16_t)off) & RING_MASK].data[data_idx] = tensor_base(t);
#else
    g_basic_buf[(base_id + (uint16_t)off) & RING_MASK].data[data_idx] = (uint64_t)t;
#endif
}

static inline void tpl_patch_scalar(uint16_t base_id, int off, int scalar_idx, int64_t v)
{
    g_basic_buf[(base_id + (uint16_t)off) & RING_MASK].scalar[scalar_idx] = v;
}

static inline void tpl_rewire_begin(uint16_t task_id)
{
    struct task_desc *d = &g_basic_buf[task_id & RING_MASK];
    d->tensor_cnt = 0;
    d->scalar_cnt = 0;
}

/* ===========================================================================
 * Build-phase claim / deferred submit / flush (implemented in orch_build.c).
 * ========================================================================== */

#ifndef ORCH_DEFER_MAX
#define ORCH_DEFER_MAX 512
#endif

typedef enum {
    ORCH_DEFER_SUBMIT    = 0,
    ORCH_DEFER_ROOT      = 1,
    ORCH_DEFER_TM_SUBMIT = 2,
} orch_defer_kind_t;

typedef struct {
    orch_defer_kind_t kind;
    uint16_t          task_id;
} orch_defer_ent_t;

void orch_build_begin(void);
void orch_build_end(void);
void orch_build_flush(void);

void task_claim_range(uint16_t n);

void orch_defer_submit(uint16_t task_id);
void orch_defer_batch_submit(uint16_t cnt, const uint16_t task_id[]);
void orch_defer_root(uint16_t task_id);
#ifdef USE_TENSORMAP
void orch_defer_tm_submit(uint16_t task_id);
void orch_flush_tm_submit(uint16_t task_id);
#endif

#if ORCH_BUILD_PHASE
void dep_group_install(uint16_t base_id, const dep_slot_tpl_t *tpl, uint16_t n_slots);
void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer);
void macro_install_bulk(const uint16_t *edges, size_t n_edges);
void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred);
#else
#define dep_group_install     dep_group_memcpy
void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer);
void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred);
void macro_install_bulk(const uint16_t *edges, size_t n_edges);
#endif

#endif /* ORCH_BUILD_H */
