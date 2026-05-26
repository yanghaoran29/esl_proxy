/*
 * mpmc_queue.h - Lock-free Multi-Producer-Multi-Consumer Queue
 *
 * Bounded circular buffer queue with C11 atomics for lock-free concurrent access.
 * Supports single-item and batch operations.
 * Naming follows Constitution XI: no dag_ prefix on types/functions.
 */

#ifndef DAG_MPMC_QUEUE_H
#define DAG_MPMC_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MPMC_OK    = 0,
    MPMC_FULL  = 1,
    MPMC_EMPTY = 2,
} mpmc_status_t;

typedef struct {
    void *buffer;
    size_t capacity;
    size_t elem_size;
    _Atomic size_t head;
    _Atomic size_t tail;
} mpmc_queue_t;

static inline int mpmc_init(mpmc_queue_t *q, size_t capacity, size_t elem_size) {
    q->buffer = malloc(capacity * elem_size);
    q->capacity = capacity;
    q->elem_size = elem_size;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    return 0;
}

static inline size_t mpmc_idx(mpmc_queue_t *q, size_t pos) {
    return pos % q->capacity;
}

static inline mpmc_status_t mpmc_enqueue(mpmc_queue_t *q, const void *item) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail - head >= q->capacity) {
        return MPMC_FULL;
    }

    char *slot = (char *)q->buffer + mpmc_idx(q, tail) * q->elem_size;
    memcpy(slot, item, q->elem_size);

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return MPMC_OK;
}

static inline mpmc_status_t mpmc_dequeue(mpmc_queue_t *q, void *item) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (tail == head) {
        return MPMC_EMPTY;
    }

    char *slot = (char *)q->buffer + mpmc_idx(q, head) * q->elem_size;
    memcpy(item, slot, q->elem_size);

    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return MPMC_OK;
}

static inline size_t mpmc_enqueue_batch(mpmc_queue_t *q, const void *items, size_t count) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    size_t avail = q->capacity - (tail - head);

    size_t to_enq = (count < avail) ? count : avail;
    const char *src = (const char *)items;

    for (size_t i = 0; i < to_enq; i++) {
        char *slot = (char *)q->buffer + mpmc_idx(q, tail + i) * q->elem_size;
        memcpy(slot, src + i * q->elem_size, q->elem_size);
    }

    atomic_store_explicit(&q->tail, tail + to_enq, memory_order_release);
    return to_enq;
}

static inline size_t mpmc_dequeue_batch(mpmc_queue_t *q, void *items, size_t count) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    size_t avail = tail - head;

    size_t to_deq = (count < avail) ? count : avail;
    char *dst = (char *)items;

    for (size_t i = 0; i < to_deq; i++) {
        char *slot = (char *)q->buffer + mpmc_idx(q, head + i) * q->elem_size;
        memcpy(dst + i * q->elem_size, slot, q->elem_size);
    }

    atomic_store_explicit(&q->head, head + to_deq, memory_order_release);
    return to_deq;
}

static inline size_t mpmc_size(mpmc_queue_t *q) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    return tail - head;
}

#endif /* DAG_MPMC_QUEUE_H */