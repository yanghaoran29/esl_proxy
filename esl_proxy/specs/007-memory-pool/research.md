# Research: Memory Pool Manager

**Feature**: 007-memory-pool
**Date**: 2026-05-27

## Decisions Made

### Decision 1: when2free Release via FIFO Head Pointer Update

**What was chosen**: Memory release via when2free is implemented by updating the FIFO head pointer to the registered address, making memory available for reuse without an explicit free operation.

**Rationale**: The FIFO-based allocation scheme uses head/tail pointers for O(1) operations. When the minimum uncompleted TaskID crosses a buffer's threshold, updating the head pointer to that buffer's address effectively "frees" the memory by advancing the allocation boundary. This leverages the contiguity of the memory pool.

### Decision 2: SPSC Mode

**What was chosen**: Single Producer Single Consumer mode for memory pool operations.

**Rationale**: Producer (typically Orchestrator) allocates and registers when2free. Consumer (Manager thread) processes FIFO entries and updates head pointer. SPSC eliminates need for complex synchronization.

### Decision 3: Trust the Caller

**What was chosen**: No input validation in functions.

**Rationale**: Constitution Principle X. All inputs assumed correct. Undefined behavior on invalid input. This simplifies code and eliminates defensive programming overhead.

## Integration Points

### Task State Ring Buffer Interface

- `ring_min_uncompleted()` from `ring_buf.h` — returns minimum ID with state != COMPLETED
- `task_state_get(task_id)` — reads task state from ring buffer

### Dependencies

- `include/dag/task.h` — task_id_t, task_state_t definitions
- `include/dag/ring_buf.h` — ring_min_uncompleted(), g_state_buf[]

## Summary

All unknowns resolved:
- Ring buffer interface: `ring_min_uncompleted()` function available
- FIFO head pointer: atomic update via C11 atomics
- Input validation: not needed (Trust the Caller)