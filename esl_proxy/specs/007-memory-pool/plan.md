# Implementation Plan: Memory Pool

**Branch**: `007-memory-pool` | **Date**: 2026-05-27 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/007-memory-pool/spec.md`

## Summary

A pre-allocated memory pool for DAG task execution supporting: SPSC allocation/deallocation via ring buffer head/tail pointer updates, when2free automatic release managed via additional FIFO queue recording addr/taskid pairs, and a dedicated Manager thread for threshold-based memory reclamation.

## Technical Context

**Language/Version**: C11 (`-std=c11`)

**Primary Dependencies**: None (standard C library only)

**Storage**: N/A (in-memory pool)

**Testing**: Unity test framework, microbenchmarks for allocation latency

**Target Platform**: Linux/macOS, x86_64

**Project Type**: Header-only C library (in-memory pool)

**Performance Goals**: Allocation/deallocation < 1μs, when2free release < 1μs after threshold

**Constraints**: SPSC mode only (single producer Orchestrator, single consumer Manager), continuous memory without fixed slots, when2free FIFO queue for addr/taskid

**Scale/Scope**: Pool sizes 1MB-1GB, thousands of concurrent allocations

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance Requirement |
|-----------|----------------------|
| Modern C11 | C11 standard (`-std=c11`) only; `_Generic`, atomics, `restrict` pointers required; unsafe practices prohibited |
| Callback-Based Async Architecture | Manager thread uses async polling pattern; when2free FIFO queue; no blocking |
| DAG-Based Task Scheduling | N/A - memory pool component, not DAG scheduler |
| Zero-Copy Task Data Flow | Buffer descriptors (pointer+size) for zero-copy sharing |
| Lock-Free Concurrency | SPSC mode with C11 atomics; ring buffer head/tail pointers are atomic; no mutexes |
| No Blocking in Hot Paths | Manager thread polls without blocking; atomic operations only |
| Deterministic Scheduling | N/A - memory pool component |
| Testability & Reproducibility | Unit tests for alloc/free/error paths; microbenchmarks |
| Header-Only Library | All implementation in headers with `static inline` |
| Trust the Caller | Caller provides valid addresses and TaskIDs; no validation at pool layer |

**Rationale**: Memory pool is a header-only C11 component using ring buffer head/tail pointers for SPSC. Manager thread runs independently with async polling. when2free FIFO queue decouples release timing from allocation.

## Project Structure

### Documentation (this feature)

```text
specs/007-memory-pool/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (/speckit-tasks)
```

### Source Code (repository root)

```text
include/dag/
├── mem_pool.h           # Memory pool header (main API)
├── mem_pool.c           # Global pool definitions
├── ring_buf.h           # Ring buffer (Task State Ring Buffer)
└── ring_buf.c           # Ring buffer global defs
```

**Structure Decision**: Header-only library under `include/dag/`. Single `mem_pool.h` with `static inline` implementations. when2free uses separate FIFO queue for addr/taskid pairs.

## Continuous Memory Ring Buffer Design

### Allocation (Producer - Orchestrator)
- Uses ring buffer tail offset to allocate variable-sized chunks
- `tail += size` with wraparound at total_size
- O(1) operation: single atomic addition by allocation size

### Release (Consumer - Manager)
- Uses ring buffer head offset to free in FIFO order
- `head += size` with wraparound at total_size
- O(1) operation: single atomic addition by allocation size

### when2free FIFO Queue
- Additional SPSC FIFO queue records when2free entries
- Each entry contains: addr (buffer address), taskid (release threshold)
- Manager thread dequeues entries when min_uncompleted > taskid
- O(1) enqueue (producer) and O(1) dequeue (consumer)

### Why Separate when2free FIFO?
1. **Decoupling**: when2free registration timing decoupled from allocation
2. **FIFO Order**: Buffers freed in registration order when thresholds met
3. **Simplicity**: No need to track in-use slots for release - just check FIFO
4. **SPSC Natural Fit**: when2free called by Orchestrator (producer), processed by Manager (consumer)

### when2free Flow

```
Orchestrator                    Manager Thread
     |                               |
when2free(addr, T) --> [FIFO] -->   |
     |                          min_uncompleted > T?
     |                               |
     |                          Yes: free(addr), dequeue
     |                               |
     |                          No: keep in FIFO
```

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Manager thread polling | Decouples when2free release from task execution | Synchronous release would block task execution paths |
| SPSC-only mode | Simplifies lock-free design to ring buffer head/tail | MPMC would require CAS retry loops |
| when2free FIFO queue | Decouples release timing, enables O(1) threshold check | Inline tracking would add complexity |
| Continuous memory (no slots) | Variable-sized allocations without wasted space | Fixed slots would waste memory or limit max size |