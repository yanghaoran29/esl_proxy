/*
 * macro_group.h - Macro/micro two-tier dependency layer.
 *
 * Macro nodes (group-level) use g_macro_* buffers; micro tasks inside a group
 * use the existing ring_buf dependency fields. Cross-group edges are macro_succeed
 * only; intra-group edges are installed via dep_group_memcpy().
 */

#ifndef MACRO_GROUP_H
#define MACRO_GROUP_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#include "conf.h"
#include "mpmc_queue.h"
#include "ring_buf.h"
#include "task.h"

#ifndef MACRO_RING_SIZE
#define MACRO_RING_SIZE 128
#endif
#define MACRO_RING_MASK (MACRO_RING_SIZE - 1)
#define MACRO_HALF_RING_SIZE (MACRO_RING_SIZE / 2)

extern _Atomic uint16_t g_macro_predecessor_buf[MACRO_RING_SIZE];
extern task_state g_macro_state_buf[MACRO_RING_SIZE];
extern struct succ_list g_macro_successor_buf[MACRO_RING_SIZE];
extern struct succ_list g_macro_successor_exp_buf[MACRO_HALF_RING_SIZE];
extern uint16_t g_macro_entry_micro[MACRO_RING_SIZE];
extern uint16_t g_micro_exit_to_macro[RING_SIZE];

static inline void macro_ring_init(void)
{
    for (size_t i = 0; i < MACRO_RING_SIZE; i++) {
        g_macro_successor_buf[i].next = &g_macro_successor_exp_buf[i % MACRO_HALF_RING_SIZE];
        g_macro_entry_micro[i] = 0;
    }
    for (size_t i = 0; i < MACRO_HALF_RING_SIZE; i++) {
        g_macro_successor_exp_buf[i].next = NULL;
    }
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_micro_exit_to_macro[i] = 0;
    }
}

static inline void macro_group_bind(uint16_t macro_id, uint16_t entry_micro,
                                    uint16_t exit_micro)
{
    g_macro_entry_micro[macro_id & MACRO_RING_MASK] = entry_micro;
    g_micro_exit_to_macro[exit_micro & RING_MASK] = macro_id;
}

static inline void macro_gate_micro_entry(uint16_t micro_id, uint16_t gate_pred)
{
    atomic_store_explicit(&g_predecessor_buf[micro_id & RING_MASK], gate_pred,
                          memory_order_relaxed);
}

static inline void macro_succeed(uint16_t macro_consumer, uint16_t macro_producer)
{
    int prod_slot = (int)(macro_producer & MACRO_RING_MASK);
    int cons_slot = (int)(macro_consumer & MACRO_RING_MASK);

    task_state st =
        atomic_load_explicit(&g_macro_state_buf[prod_slot], memory_order_relaxed);
    int idx = (int)st.successor_cnt;
    struct succ_list *ptr = &g_macro_successor_buf[prod_slot];
    while (idx >= SUCC_NODE_CNT) {
        idx -= SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    ptr->successor[idx] = macro_consumer;
    st.successor_cnt++;
    atomic_store_explicit(&g_macro_state_buf[prod_slot], st, memory_order_relaxed);

    atomic_fetch_add_explicit(&g_macro_predecessor_buf[cons_slot], 1,
                              memory_order_relaxed);
}

static inline void macro_release_entry(uint16_t macro_id, uint16_t *rq_buf,
                                       uint16_t *ready_cnt)
{
    uint16_t entry = g_macro_entry_micro[macro_id & MACRO_RING_MASK];
    if (entry == 0)
        return;
    uint16_t left = (uint16_t)atomic_fetch_sub_explicit(
        &g_predecessor_buf[entry & RING_MASK], 1, memory_order_relaxed);
    if (left == 1 && *ready_cnt < AIC_CNT) {
        rq_buf[(*ready_cnt)++] = entry;
    }
}

static inline void macro_propagate_complete(uint16_t macro_id, uint16_t *rq_buf,
                                            uint16_t *ready_cnt)
{
    int idx = (int)(macro_id & MACRO_RING_MASK);
    task_state st =
        atomic_load_explicit(&g_macro_state_buf[idx], memory_order_relaxed);
    uint16_t succ_cnt = (uint16_t)st.successor_cnt;
    for (uint16_t j = 0; j < succ_cnt; j++) {
        uint16_t succ_id = g_macro_successor_buf[idx].successor[j];
        uint16_t left = (uint16_t)atomic_fetch_sub_explicit(
            &g_macro_predecessor_buf[succ_id & MACRO_RING_MASK], 1,
            memory_order_relaxed);
        if (left == 1) {
            macro_release_entry(succ_id, rq_buf, ready_cnt);
        }
    }
}

static inline void macro_on_micro_exit(uint16_t micro_id, uint16_t *rq_buf,
                                       uint16_t *ready_cnt)
{
    uint16_t macro_id = g_micro_exit_to_macro[micro_id & RING_MASK];
    if (macro_id != 0) {
        macro_propagate_complete(macro_id, rq_buf, ready_cnt);
    }
}

static inline void macro_enqueue_roots(uint16_t micro_id)
{
    ready_enqueue(g_basic_buf[micro_id & RING_MASK].type,
                  g_basic_buf[micro_id & RING_MASK].mode, micro_id);
}

#endif /* MACRO_GROUP_H */
