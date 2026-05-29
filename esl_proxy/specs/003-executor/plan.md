# Implementation Plan: [FEATURE]

**Branch**: `[###-feature-name]` | **Date**: [DATE] | **Spec**: [link]

**Input**: Feature specification from `/specs/[###-feature-name]/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Async Task Executor with 2-slot PING PONG cache for task buffering. Executor reads Task info, delays for specified duration, and returns result. Uses callback-based async pattern with function pointers. Header-only C library.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (header-only, standard C only)

**Storage**: N/A (in-memory task execution)

**Testing**: Custom test harness (no external framework per constitution)

**Target Platform**: Linux server, high-performance computing

**Project Type**: Header-only C library

**Performance Goals**: Sub-microsecond task submission, O(1) slot selection

**Constraints**: Lock-free in hot paths, no blocking, Trust the Caller principle

**Scale/Scope**: 2-slot cache per Executor, supports 120+ Executor threads

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited | ✓ PASS |
| Callback-Based Async Architecture | All APIs async with callbacks; no blocking in hot paths; function pointers + userdata replace C++ lambdas | ✓ PASS |
| DAG-Based Task Scheduling | All tasks form a DAG; cycles are defects; scheduler must respect dependency order; Work-Stealing required | ✓ PASS (Executor is DAG node executor) |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size), shared memory, in-place transforms; copies require benchmark justification | ✓ PASS |
| Lock-Free Concurrency | C11 atomics required; mutexes/spinlocks prohibited in hot paths; lock-free SPSC queues for task distribution | ✓ PASS |
| No Blocking in Hot Paths | No sync I/O or blocking waits; all waits async with continuation enqueue; bounded timeouts required | ✓ PASS |
| Deterministic Scheduling | Same DAG+inputs produce same results; hidden global state (time, random, env) prohibited | ✓ PASS |
| Testability & Reproducibility | Dependency injection via function pointers; mock scheduler support required; chaos testing encouraged | ✓ PASS |
| Header-Only Library | All implementation in headers; `static inline` functions; no binary dependencies | ✓ PASS (executor.h header-only) |
| Trust the Caller | All inputs assumed correct; no validation, no exception handling, no edge case testing; undefined behavior on invalid input | ✓ PASS |
| Concise Naming | Variable and function names MUST be concise and avoid unnecessary prefixes | ✓ PASS |

**Rationale**: This is a high-performance async DAG engine in C with Work-Stealing scheduler. Header-only C design ensures maximum inlining and zero linking overhead.

### Source Code (repository root)

```text
include/dag/
├── executor.h         # Executor type and functions
└── task.h             # Task type (shared)

src/
└── executor.c         # Executor implementation (worker thread)
```

**Structure Decision**: Header-only library with executor implementation in src/ for worker thread entry point. Task type in include/dag/task.h for sharing.

## Phase 0: Research

No unknowns requiring research. The Executor is a straightforward async task runner with PING PONG slot selection - all technical decisions are determined by the Constitution.

## Phase 1: Design

No research.md needed - all design decisions derived directly from spec and Constitution.

### Data Model (data-model.md)

**Task State Enum** (`task_state_t`):
- `TASK_PENDING` = 0
- `TASK_EXECUTING` = 1
- `TASK_COMPLETED` = 2
- `TASK_ERROR` = 3

**Task Descriptor** (`task_t`):
- `id: uint32_t` - Task identifier
- `state: task_state_t` - Current execution state
- `input: void*` - Input data pointer
- `output: void*` - Output data pointer
- `constant: void*` - Constant data pointer
- `kernel: void (*)(void*)` - Kernel function pointer
- `duration_ms: uint32_t` - Delay duration in milliseconds
- `subtask_cnt: uint32_t` - Sub-task count

**Executor Type** (`executor_t`):
- `slots: task_t[2]` - Two task cache slots
- `slot_state: atomic int[2]` - Slot occupancy state
- `ping_pong: atomic int` - Current slot selector (0 or 1)
- `worker: pthread_t` - Worker thread handle
- `running: atomic bool` - Executor running state

### Quickstart (quickstart.md)

```c
// Create executor
executor_t exec;
executor_init(&exec);

// Submit task (async, returns immediately)
task_t task = { .id = 0, .duration_ms = 100, .kernel = my_kernel };
executor_submit(&exec, &task);

// Wait for completion
while (task.state != TASK_COMPLETED) { /* spin or sleep */ }
```
