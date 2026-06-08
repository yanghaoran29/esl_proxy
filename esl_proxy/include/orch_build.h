/*
 * orch_build.h - Single-threaded static orchestration build-phase helpers.
 *
 *   dep_install: build-phase edge wiring (no succeed CAS).
 *   defer submit/root + flush: static cases enqueue after graph build.
 *
 * Runtime macro propagation stays in macro_group.h.
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

#ifndef ORCH_DEFER_MAX
#define ORCH_DEFER_MAX 512
#endif

typedef enum {
    ORCH_DEFER_SUBMIT = 0,
    ORCH_DEFER_ROOT   = 1,
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
void orch_defer_root(uint16_t task_id);

void macro_succeed_build(uint16_t macro_consumer, uint16_t macro_producer);
void macro_gate_micro_build(uint16_t micro_id, uint16_t gate_pred);

#endif /* ORCH_BUILD_H */
