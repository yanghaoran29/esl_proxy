/*
 * mpmc_queue.h - BlkRing MPMC Queue for DAG engine
 *
 * Lock-free Multi-Producer-Multi-Consumer queue using atomic slot states.
 * Slot state machine: EMPTY→FILL→COMPLETE→EMPTY (no CAS retry loops)
 *
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_MPMC_QUEUE_H
#define DAG_MPMC_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include "task.h"

/*
 * Slot states - atomic state machine
 * EMPTY=0 → FILL=1 → COMPLETE=2 → EMPTY=0
 */
typedef enum {
    SLOT_EMPTY    = 0,
    SLOT_FILL     = 1,
    SLOT_COMPLETE = 2,
} slot_state_t;

/*
 * Queue slot with atomic state
 * Producer writes when EMPTY, transitions to FILL
 * Consumer reads when FILL, transitions to COMPLETE then EMPTY
 */
typedef struct {
    void *data;
    _Atomic slot_state_t state;
} blkring_slot_t;

/*
 * MPMC Queue - circular buffer with atomic slot states
 * Uses producer_idx and consumer_idx for O(1) circular access
 */
typedef struct {
    blkring_slot_t *slots;
    uint32_t capacity;
    _Atomic uint32_t producer_idx;  /* Points to next EMPTY slot */
    _Atomic uint32_t consumer_idx; /* Points to next FILL slot */
} mpmc_queue_t;

/*
 * Return codes for queue operations
 */
typedef enum {
    MPMC_OK      = 0,
    MPMC_FULL    = -1,
    MPMC_EMPTY   = -2,
} mpmc_ret_t;

/* Default capacities */
#define READY_QUEUE_CAPACITY 1024
#define COMPLETE_QUEUE_CAPACITY 1024

/*
 * Circular index calculation
 * Uses modulo via bitwise AND (capacity must be power of 2)
 */
static inline uint32_t mpmc_idx(uint32_t pos, uint32_t capacity) {
    return pos & (capacity - 1);
}

/*
 * Initialize queue with given capacity
 */
static inline void mpmc_init(mpmc_queue_t *q, uint32_t capacity) {
    q->slots = calloc(capacity, sizeof(blkring_slot_t));
    q->capacity = capacity;
    atomic_store(&q->producer_idx, 0);
    atomic_store(&q->consumer_idx, 0);
}

/*
 * Atomic slot state load
 */
static inline slot_state_t slot_state_load(blkring_slot_t *slot) {
    return atomic_load_explicit(&slot->state, memory_order_acquire);
}

/*
 * Atomic slot state store
 */
static inline void slot_state_store(blkring_slot_t *slot, slot_state_t state) {
    atomic_store_explicit(&slot->state, state, memory_order_release);
}

/*
 * Enqueue item to queue (blkring_produce)
 * Returns: MPMC_OK on success, MPMC_FULL if no EMPTY slots
 *
 * Single CAS attempt per slot - no retry loops
 */
static inline mpmc_ret_t blkring_produce(mpmc_queue_t *q, void *item) {
    uint32_t prod_idx = atomic_load(&q->producer_idx);
    uint32_t cap = q->capacity;

    for (uint32_t attempt = 0; attempt < cap; attempt++) {
        uint32_t idx = mpmc_idx(prod_idx + attempt, cap);
        blkring_slot_t *slot = &q->slots[idx];

        slot_state_t expected = SLOT_EMPTY;
        if (atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, SLOT_FILL,
                memory_order_acq_rel, memory_order_acquire)) {
            slot->data = item;
            atomic_store(&q->producer_idx, prod_idx + attempt + 1);
            return MPMC_OK;
        }

        prod_idx = atomic_load(&q->producer_idx);
    }

    return MPMC_FULL;
}

/*
 * Dequeue item from queue (blkring_consume)
 * Returns: MPMC_OK on success, MPMC_EMPTY if no FILL slots
 *
 * Single CAS attempt per slot - no retry loops
 */
static inline mpmc_ret_t blkring_consume(mpmc_queue_t *q, void **item) {
    uint32_t cons_idx = atomic_load(&q->consumer_idx);
    uint32_t cap = q->capacity;

    for (uint32_t attempt = 0; attempt < cap; attempt++) {
        uint32_t idx = mpmc_idx(cons_idx + attempt, cap);
        blkring_slot_t *slot = &q->slots[idx];

        slot_state_t expected = SLOT_FILL;
        if (atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, SLOT_COMPLETE,
                memory_order_acq_rel, memory_order_acquire)) {
            *item = slot->data;
            slot_state_store(slot, SLOT_EMPTY);
            atomic_store(&q->consumer_idx, cons_idx + attempt + 1);
            return MPMC_OK;
        }

        cons_idx = atomic_load(&q->consumer_idx);
    }

    *item = NULL;
    return MPMC_EMPTY;
}

/*
 * Batch enqueue - returns count of items successfully enqueued
 */
static inline uint32_t blkring_produce_batch(mpmc_queue_t *q, void **items, uint32_t count) {
    uint32_t enqueued = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (blkring_produce(q, items[i]) == MPMC_OK) {
            enqueued++;
        } else {
            break;
        }
    }
    return enqueued;
}

/*
 * Batch dequeue - returns count of items successfully dequeued
 */
static inline uint32_t blkring_consume_batch(mpmc_queue_t *q, void **items, uint32_t count) {
    uint32_t dequeued = 0;
    for (uint32_t i = 0; i < count; i++) {
        void *item;
        if (blkring_consume(q, &item) == MPMC_OK) {
            items[i] = item;
            dequeued++;
        } else {
            break;
        }
    }
    return dequeued;
}

#endif /* DAG_MPMC_QUEUE_H */