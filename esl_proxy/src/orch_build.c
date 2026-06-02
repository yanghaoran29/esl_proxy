/*
 * orch_build.c - Build-phase orchestration helpers (claim, dep, defer submit).
 */

#include "orch_build.h"

#include <stdatomic.h>

#include "macro_group.h"
#include "ring_buf.h"

static orch_defer_ent_t g_defer_buf[ORCH_DEFER_MAX];
static uint16_t g_defer_n;

void orch_build_begin(void)
{
    g_defer_n = 0;
}

void orch_build_end(void)
{
    atomic_thread_fence(memory_order_release);
}

void orch_defer_submit(uint16_t task_id)
{
    if (g_defer_n < ORCH_DEFER_MAX) {
        g_defer_buf[g_defer_n].kind = ORCH_DEFER_SUBMIT;
        g_defer_buf[g_defer_n].task_id = task_id;
        g_defer_n++;
    }
}

void orch_defer_batch_submit(uint16_t cnt, const uint16_t task_id[])
{
    for (uint16_t i = 0; i < cnt; i++)
        orch_defer_submit(task_id[i]);
}

void orch_defer_root(uint16_t task_id)
{
    if (g_defer_n < ORCH_DEFER_MAX) {
        g_defer_buf[g_defer_n].kind = ORCH_DEFER_ROOT;
        g_defer_buf[g_defer_n].task_id = task_id;
        g_defer_n++;
    }
}

#ifdef USE_TENSORMAP
void orch_defer_tm_submit(uint16_t task_id)
{
    if (g_defer_n < ORCH_DEFER_MAX) {
        g_defer_buf[g_defer_n].kind = ORCH_DEFER_TM_SUBMIT;
        g_defer_buf[g_defer_n].task_id = task_id;
        g_defer_n++;
    }
}

__attribute__((weak)) void orch_flush_tm_submit(uint16_t task_id)
{
    (void)task_id;
}
#endif

void orch_build_flush(void)
{
#if ORCH_USE_DEFER_SUBMIT
    for (uint16_t i = 0; i < g_defer_n; i++) {
        const orch_defer_ent_t *e = &g_defer_buf[i];
        if (e->kind == ORCH_DEFER_ROOT) {
            macro_enqueue_roots(e->task_id);
            continue;
        }
#ifdef USE_TENSORMAP
        if (e->kind == ORCH_DEFER_TM_SUBMIT) {
            orch_flush_tm_submit(e->task_id);
            continue;
        }
#endif
        uint16_t left = (uint16_t)atomic_fetch_sub_explicit(
            &g_predecessor_buf[e->task_id & RING_MASK], 1, memory_order_relaxed);
        if (left == 1)
            ready_enqueue(g_basic_buf[e->task_id & RING_MASK].type,
                          g_basic_buf[e->task_id & RING_MASK].mode, e->task_id);
    }
    g_defer_n = 0;
#endif
}

void task_claim_range(uint16_t n)
{
    const uint16_t first = (uint16_t)(g_task_id + 1u);
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t id = (uint16_t)(first + i);
        const int slot = (int)(id & RING_MASK);
        task_state st = { .state = PENDING, .task_id = id, .successor_cnt = 0 };

        atomic_store_explicit(&g_state_buf[slot], st, memory_order_relaxed);
        atomic_store_explicit(&g_predecessor_buf[slot], 0, memory_order_relaxed);
    }
    g_task_id = (uint16_t)(first + n - 1u);
}

#if ORCH_BUILD_PHASE

void dep_group_install(uint16_t base_id, const dep_slot_tpl_t *tpl, uint16_t n_slots)
{
    for (uint16_t i = 0; i < n_slots; i++) {
        const uint16_t tid = (uint16_t)(base_id + i);
        const int slot = (int)(tid & RING_MASK);

        atomic_store_explicit(&g_predecessor_buf[slot], tpl[i].pred,
                              memory_order_relaxed);

        task_state st =
            atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
        st.successor_cnt = tpl[i].succ_cnt;
        atomic_store_explicit(&g_state_buf[slot], st, memory_order_relaxed);

        for (uint16_t j = 0; j < tpl[i].succ_cnt; j++)
            g_successor_buf[slot].successor[j] = (uint16_t)(base_id + tpl[i].succ[j]);
    }
}

void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer)
{
    const int prod_slot = (int)(macro_producer & MACRO_RING_MASK);
    const int cons_slot = (int)(macro_consumer & MACRO_RING_MASK);

    task_state st = g_macro_state_buf[prod_slot];
    int idx = (int)st.successor_cnt;
    struct succ_list *ptr = &g_macro_successor_buf[prod_slot];

    while (idx >= SUCC_NODE_CNT) {
        idx -= SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    ptr->successor[idx] = macro_consumer;
    st.successor_cnt++;
    g_macro_state_buf[prod_slot] = st;

    atomic_fetch_add_explicit(&g_macro_predecessor_buf[cons_slot], 1,
                              memory_order_relaxed);
}

void macro_install_bulk(const uint16_t *edges, size_t n_edges)
{
    for (size_t i = 0; i < n_edges; i += 2u)
        macro_succeed_build(edges[i], edges[i + 1u]);
}

void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred)
{
    atomic_store_explicit(&g_predecessor_buf[micro_id & RING_MASK], gate_pred,
                          memory_order_relaxed);
}

#else /* !ORCH_BUILD_PHASE */

void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer)
{
    macro_succeed(macro_consumer, macro_producer);
}

void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred)
{
    macro_gate_micro_entry(micro_id, gate_pred);
}

void macro_install_bulk(const uint16_t *edges, size_t n_edges)
{
    for (size_t i = 0; i < n_edges; i += 2u)
        macro_succeed(edges[i], edges[i + 1u]);
}

#endif /* ORCH_BUILD_PHASE */
