# Implementation Plan: Pre-allocated Memory Pool

**Branch**: `011-prealloc-memory` | **Date**: 2026-05-27 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/011-prealloc-memory/spec.md`

## Summary

Pre-allocate 120MB memory pool for DAG task execution with FIFO-based allocation and when2free automatic memory release. Memory release is achieved by updating the head pointer to the address recorded in the FIFO for the corresponding taskID threshold, enabling O(1) deallocation without complex data structures.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (header-only, standard C library only)

**Storage**: N/A (in-memory pool)

**Testing**: Custom unit tests via tasks.md verification

**Target Platform**: Linux/macOS (C11 atomics)

**Project Type**: Header-only C library (DAG engine component)

**Performance Goals**: O(1) allocation/deallocation, under 1 microsecond latency

**Constraints**: No mutexes in hot paths, C11 atomics only, no system malloc after initialization

**Scale/Scope**: 120MB (125,829,120 bytes) fixed capacity, SPSC mode

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited |
| Callback-Based Async Architecture | All APIs async with callbacks; no blocking in hot paths; function pointers + userdata replace C++ lambdas |
| DAG-Based Task Scheduling | All tasks form a DAG; cycles are defects; scheduler must respect dependency order; Work-Stealing required |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size), shared memory, in-place transforms; copies require benchmark justification |
| Lock-Free Concurrency | C11 atomics required; mutexes/spinlocks prohibited in hot paths; lock-free SPSC queues for task distribution |
| No Blocking in Hot Paths | No sync I/O or blocking waits; all waits async with continuation enqueue; bounded timeouts required |
| Deterministic Scheduling | Same DAG+inputs produce same results; hidden global state (time, random, env) prohibited |
| Testability & Reproducibility | Dependency injection via function pointers; mock scheduler support required; chaos testing encouraged |
| Header-Only Library | All implementation in headers; `static inline` functions; no binary dependencies |
| Trust the Caller | All inputs assumed correct; no validation, no exception handling, no edge case testing; undefined behavior on invalid input |
| Concise Naming | Variable and function names MUST be concise and avoid unnecessary prefixes |

**Rationale**: This is a high-performance async DAG engine in C with Work-Stealing scheduler. Header-only C design ensures maximum inlining and zero linking overhead.

## Project Structure

### Documentation (this feature)

```text
specs/011-prealloc-memory/
├── plan.md              # This file (/speckit-plan command output)
├── spec.md              # Feature specification (already exists)
├── tasks.md             # Task list (/speckit-tasks command - NOT created by /speckit-plan)
└── contracts/            # Not applicable (internal library, no external APIs)
```

### Source Code (repository root)

```text
include/dag/
├── mem_pool.h           # Pre-allocated 120MB pool with FIFO-based allocation
├── mem_pool.c           # Pool initialization (120MB pre-allocation via malloc)
├── mem_pool_internal.h  # Internal structures: FIFO queue, when2free entries
├── task.h               # Task types (task_type_t, org_mode_t) - existing
└── ring_buf.h           # Ring buffer API - existing, for task state tracking
```

**Structure Decision**: Header-only library with single .c file for malloc-based pre-allocation. FIFO-based continuous memory management without fixed slots. when2free mechanism updates head pointer to address in FIFO entry.

## FIFO-Based Memory Release Design

### Core Concept

Memory release is achieved by updating the head pointer to the address recorded in the FIFO for the corresponding taskID threshold. This provides O(1) deallocation without complex data structures.

### Memory Layout

```
+----------------------------------------------------------+
|                    Pre-allocated 120MB                   |
+----------------------------------------------------------+
|     |  Free   |     Allocated    |     Free      |        |
+----------------------------------------------------------+
       ^                                    ^
       |                                    |
    head (next alloc)                  tail (next free)
```

### FIFO Queue for when2free

```c
typedef struct {
    void *addr;       /* Buffer address to free */
    task_id_t task_id; /* Threshold taskID */
} when2free_entry_t;

/* FIFO queue stores when2free entries */
typedef struct {
    when2free_entry_t *entries;
    uint32_t capacity;
    _Atomic uint32_t head;  /* Points to next entry to process */
    _Atomic uint32_t tail;  /* Points to next empty slot */
} when2free_fifo_t;
```

### Memory Release Mechanism

1. **Manager Thread** monitors minimum uncompleted TaskID
2. **When min_uncompleted crosses a threshold**: Manager looks up FIFO entry
3. **Head pointer update**: `head = addr` in the memory pool - the freed memory becomes available for new allocations
4. **FIFO advance**: Manager increments head pointer to process next entry

### when2free Registration (Producer - Orchestrator)

```c
static inline void when2free(void *addr, task_id_t task_id) {
    uint32_t idx = atomic_load(&fifo.tail);
    fifo.entries[idx] = (when2free_entry_t){addr, task_id};
    atomic_store(&fifo.tail, idx + 1);
}
```

### Memory Release (Consumer - Manager)

```c
static inline void release_when2free(task_id_t min_uncompleted) {
    while (atomic_load(&fifo.head) < atomic_load(&fifo.tail)) {
        uint32_t idx = atomic_load(&fifo.head);
        if (fifo.entries[idx].task_id >= min_uncompleted) break;
        /* Update pool head pointer to freed address */
        pool.head = fifo.entries[idx].addr;
        atomic_store(&fifo.head, idx + 1);
    }
}
```

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| None | All Constitution principles satisfied | N/A |

## Phase 0: Research Summary

Based on spec.md and 007-memory-pool design context:

### Key Decisions

1. **Pre-allocation via malloc**: 120MB allocated once at initialization, no system malloc during execution
2. **FIFO-based when2free**: Additional FIFO queue records addr+taskID pairs for automatic release
3. **Head pointer update for release**: Memory freed by updating pool head to address from FIFO entry
4. **Continuous memory management**: No fixed-size slots, variable-sized allocations via head/tail pointers

### Alternatives Considered

- Fixed slot size pools: Rejected because variable-sized allocations required
- Bitmapped free list: Rejected because FIFO provides O(1) with simpler structure
- Deferred release with batch: Rejected because immediate release needed for throughput

## Phase 1: Design Artifacts

### Data Model (mem_pool.h)

```c
/* when2free FIFO entry */
typedef struct {
    void *addr;
    task_id_t task_id;
} when2free_entry_t;

/* when2free FIFO queue - additional queue for memory release management */
typedef struct {
    when2free_entry_t *entries;
    uint32_t capacity;
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
} when2free_fifo_t;

/* Pre-allocated memory pool - continuous memory with head/tail pointers */
typedef struct {
    uint8_t *base;          /* Pre-allocated 120MB base address */
    uint32_t total_size;    /* 125,829,120 bytes */
    _Atomic uint32_t head;  /* Next allocation point */
    _Atomic uint32_t tail;  /* Next free point (for release tracking) */
} mem_pool_t;

/* Pool metadata */
typedef struct {
    uint32_t total_bytes;   /* 125,829,120 */
    _Atomic uint32_t allocated_bytes;
    _Atomic uint32_t available_bytes;
} pool_meta_t;
```

### Global Variables (mem_pool.c)

```c
#define POOL_SIZE (120 * 1024 * 1024)  /* 125,829,120 bytes */

/* Pre-allocated 120MB pool - single global instance */
extern mem_pool_t g_mem_pool;

/* when2free FIFO queue - records addr+taskID for automatic release */
extern when2free_fifo_t g_when2free_fifo;

#define WHEN2FREE_FIFO_CAPACITY 1024
```

### API Summary

| Function | Description | Return |
|----------|-------------|--------|
| `mem_pool_init(pool, size)` | Pre-allocate size bytes | `void` |
| `mem_pool_alloc(pool, size)` | Allocate from pool | `void *` or `NULL` |
| `when2free(addr, task_id)` | Register buffer for auto-release | `void` |
| `release_when2free(min_task_id)` | Release buffers where task_id < min_task_id | `void` |
| `pool_meta(pool)` | Get pool metadata | `pool_meta_t` |
| `pool_available(pool)` | Get available bytes | `uint32_t` |

## Agent Context Update

See CLAUDE.md section updated via `<!-- SPECKIT START -->` / `<!-- SPECKIT END -->` markers.