# Implementation Plan: MPMC Queue

**Branch**: `010-mpmc-queue` | **Date**: 2026-05-26 | **Spec**: [link](spec.md)

**Input**: Lock-free MPMC queue with 2D ReadyQueue matrix and CompleteQueue for task dispatch

## Summary

A bounded multi-producer-multi-consumer (MPMC) queue using C11 atomics for lock-free concurrent access. Circular buffer provides O(1) enqueue/dequeue. Supports batch operations. 2D ReadyQueue matrix (task_type × org_mode) for task dispatch. Global CompleteQueue for recording task completions.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: Standard C library only (`<stdint.h>`, `<stdatomic.h>`, `<stdbool.h>`, `<stddef.h>`, `<string.h>`)

**Storage**: Circular buffer in memory with fixed capacity

**Testing**: Unit tests via dependency injection

**Target Platform**: Cross-platform (Linux/macOS)

**Project Type**: Header-only C library for DAG scheduling

**Performance Goals**:
- O(1) enqueue and dequeue
- Support 4+ producers and 4+ consumers concurrently
- Batch operations process 10+ items per call
- 12 ReadyQueues + 1 CompleteQueue

**Constraints**:
- Bounded queue with configurable capacity
- C11 atomics only (no mutexes in hot path)
- All inputs assumed valid (Trust the Caller)
- Naming per Constitution XI (no redundant prefixes)
- Header-only library design
- 3 task types × 4 org modes = 12 ReadyQueues
- Single global CompleteQueue

**Scale/Scope**:
- Queue capacity: 100-10000 (configurable)
- 12 user stories covering MPMC + ReadyQueue + CompleteQueue

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard only; `_Generic`, atomics, `restrict` |
| Callback-Based Async | Completion via atomic bits; function pointers |
| DAG-Based Task Scheduling | DAG structure; Work-Stealing scheduler |
| Zero-Copy Task Data Flow | Buffer descriptors in Ring Buffers |
| Lock-Free Concurrency | C11 atomics only; no mutexes in hot paths |
| No Blocking in Hot Paths | No sync I/O; async waits with continuation |
| Deterministic Scheduling | Same DAG+inputs → same results |
| Testability | Dependency injection via function pointers |
| Header-Only Library | `static inline` functions for API |
| Trust the Caller | No validation; undefined on invalid input |
| Concise Naming | No redundant prefixes; concise names |

## Project Structure

### Source Code (include/dag/)

```text
include/dag/
├── mpmc_queue.h     # MPMC Queue API (static inline functions)
├── mpmc_queue.c     # Global queue definitions
├── ready_queue.h    # 2D ReadyQueue matrix API
└── ready_queue.c    # Global ReadyQueue matrix definition
```

**Header-Only Enforcement**: All API in headers with `static inline`. Only .c file for global instance definition.

## Phase 1: Design

### mpmc_queue.h - Core MPMC Queue API

```c
#ifndef DAG_MPMC_QUEUE_H
#define DAG_MPMC_QUEUE_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

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

static inline int mpmc_init(mpmc_queue_t *q, size_t capacity, size_t elem_size);
static inline size_t mpmc_idx(mpmc_queue_t *q, size_t pos);
static inline mpmc_status_t mpmc_enqueue(mpmc_queue_t *q, const void *item);
static inline mpmc_status_t mpmc_dequeue(mpmc_queue_t *q, void *item);
static inline size_t mpmc_enqueue_batch(mpmc_queue_t *q, const void *items, size_t count);
static inline size_t mpmc_dequeue_batch(mpmc_queue_t *q, void *items, size_t count);
static inline size_t mpmc_size(mpmc_queue_t *q);

#endif
```

### ready_queue.h - 2D ReadyQueue Matrix API

```c
#ifndef DAG_READY_QUEUE_H
#define DAG_READY_QUEUE_H

#include "mpmc_queue.h"
#include "task.h"

/* 2D matrix: task_type × org_mode = 3 × 4 = 12 queues */
extern mpmc_queue_t g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT];

static inline mpmc_queue_t *ready_queue_get(task_type_t type, org_mode_t mode) {
    return &g_ready_queues[type][mode];
}

static inline mpmc_status_t ready_enqueue(task_type_t type, org_mode_t mode, const void *item) {
    return mpmc_enqueue(&g_ready_queues[type][mode], item);
}

static inline mpmc_status_t ready_dequeue(task_type_t type, org_mode_t mode, void *item) {
    return mpmc_dequeue(&g_ready_queues[type][mode], item);
}

#endif
```

### complete_queue.h - Global CompleteQueue API

```c
#ifndef DAG_COMPLETE_QUEUE_H
#define DAG_COMPLETE_QUEUE_H

#include "mpmc_queue.h"

/* Single global CompleteQueue for task completion notifications */
extern mpmc_queue_t g_complete_queue;

static inline mpmc_status_t complete_enqueue(const void *item) {
    return mpmc_enqueue(&g_complete_queue, item);
}

static inline mpmc_status_t complete_dequeue(void *item) {
    return mpmc_dequeue(&g_complete_queue, item);
}

#endif
```

### Key Design Decisions

1. **MPMC as Foundation**: Core queue type used by both ReadyQueue matrix and CompleteQueue
2. **2D Matrix**: g_ready_queues[TASK_TYPE_COUNT][ORG_MODE_COUNT] = g_ready_queues[3][4]
3. **Global Instances**: Single g_complete_queue, 12 g_ready_queues entries
4. **Concise Naming**: mpmc_* for core queue, ready_* for 2D access, complete_* for completion
5. **Task Type Indexing**: task_type and org_mode enums used directly as array indices
6. **Trust the Caller**: No validation; caller ensures valid type/mode values

---

**Status**: Plan complete. Ready for `/speckit-tasks`.