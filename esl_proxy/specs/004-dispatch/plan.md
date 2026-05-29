# Implementation Plan: [FEATURE]

**Branch**: `[###-feature-name]` | **Date**: [DATE] | **Spec**: [link]

**Input**: Feature specification from `/specs/[###-feature-name]/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Dispatch component for task distribution via shared memory. Supports dual-source task acquisition (Orchestrator + Cutter), work-stealing load balancing across Dispatches, and mixed CUBE/VECTOR Executor pools. Each Dispatch worker manages 60 CUBE + 60 VECTOR Executors with 1-bit completion signaling.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (header-only, standard C only)

**Storage**: Shared memory regions (Orchestrator-to-Dispatch, Cutter-to-Dispatch, Dispatch-to-Executor)

**Testing**: Custom test harness, shared memory simulation

**Target Platform**: Linux server, high-performance computing, NUMA-aware

**Project Type**: Header-only C library component

**Performance Goals**: <10μs dispatch latency, <100μs work-stealing redistribution, zero-copy via shared memory

**Constraints**: Lock-free in hot paths, atomic operations for shared memory sync, Trust the Caller principle

**Scale/Scope**: 2 Dispatch threads, each managing 120 Executors (60 CUBE + 60 VECTOR)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited | ✓ PASS |
| Callback-Based Async Architecture | All APIs async with callbacks; no blocking in hot paths; function pointers + userdata replace C++ lambdas | ✓ PASS |
| DAG-Based Task Scheduling | All tasks form a DAG; cycles are defects; scheduler must respect dependency order; Work-Stealing required | ✓ PASS |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size), shared memory, in-place transforms; copies require benchmark justification | ✓ PASS (shared memory zero-copy) |
| Lock-Free Concurrency | C11 atomics required; mutexes/spinlocks prohibited in hot paths; lock-free SPSC queues for task distribution | ✓ PASS |
| No Blocking in Hot Paths | No sync I/O or blocking waits; all waits async with continuation enqueue; bounded timeouts required | ✓ PASS |
| Deterministic Scheduling | Same DAG+inputs produce same results; hidden global state (time, random, env) prohibited | ✓ PASS |
| Testability & Reproducibility | Dependency injection via function pointers; mock scheduler support required; chaos testing encouraged | ✓ PASS |
| Header-Only Library | All implementation in headers; `static inline` functions; no binary dependencies | ✓ PASS |
| Trust the Caller | All inputs assumed correct; no validation, no exception handling, no edge case testing; undefined behavior on invalid input | ✓ PASS |
| Concise Naming | Variable and function names MUST be concise and avoid unnecessary prefixes | ✓ PASS |

**Rationale**: This is a high-performance DAG task distribution system. Shared memory enables zero-copy. Work-Stealing ensures efficient load balancing.

### Source Code (repository root)

```text
include/dag/
├── dispatch.h        # Dispatch type and functions
├── shm_buf.h         # Shared memory buffer descriptors
└── executor.h        # Executor type (shared)

src/
└── dispatch.c        # Dispatch implementation
```

**Structure Decision**: Header-only library with dispatch implementation in src/. Shared memory buffer descriptors for zero-copy communication.

## Phase 0: Research

No unknowns requiring research. All technical decisions derived from spec and Constitution.

## Phase 1: Design

No research.md needed - all design decisions from spec and Constitution.

### Data Model

**Shared Memory Buffer Descriptor** (`shm_buf_t`):
- `addr: void*` - Shared memory base address
- `size: size_t` - Buffer size
- `tail: _Atomic size_t` - Producer position (lock-free SPSC)
- `head: _Atomic size_t` - Consumer position

**Dispatch Type** (`dispatch_t`):
- `id: uint32_t` - Dispatch instance ID
- `executor_pool: executor_t[120]` - 120 Executors (60 CUBE + 60 VECTOR)
- `shm_from_orch: shm_buf_t` - Shared memory from Orchestrator
- `shm_from_cutter: shm_buf_t` - Shared memory from Cutter
- `completion_bits: _Atomic uint64_t` - 1-bit per Executor completion signals

### Quickstart (quickstart.md)

```c
// Attach to shared memory regions
dispatch_t disp;
dispatch_init(&disp, orch_shm, cutter_shm);

// Read and dispatch tasks
while (dispatch_has_work(&disp)) {
    task_id_t tid = dispatch_acquire_task(&disp);
    dispatch_distribute(&disp, tid);
}

// Check completion signals
if (completion_bit_read(&disp.completion_bits, exec_idx)) {
    dispatch_free_executor(&disp, exec_idx);
}
```
