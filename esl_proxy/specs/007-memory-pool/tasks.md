# Tasks: Memory Pool Manager

**Input**: Design documents from `/specs/007-memory-pool/`

**Prerequisites**: plan.md (required), spec.md (required), data-model.md

**Tests**: None requested

## Format: `[ID] [P?] [Story] Description`

## Phase 1: Setup (Project Structure)

**Purpose**: Verify/create project structure

- [X] T001 Verify include/dag/ directory exists with task.h and ring_buf.h
- [X] T002 Verify src/ directory exists, create if missing

---

## Phase 2: Foundational (Core Types)

**Purpose**: Define core data structures

- [X] T003 [P] Define mem_pool_t struct in include/dag/mem_pool.h
- [X] T004 [P] Define when2free_fifo_t struct in include/dag/mem_pool.h
- [X] T005 [P] Define when2free_entry_t struct in include/dag/mem_pool.h
- [X] T006 Define MEM_POOL_ALIGN and SENTINEL_DONE constants in include/dag/mem_pool.h

---

## Phase 3: User Story 1 - Pre-allocated Memory Pool (Priority: P1) 🎯 MVP

**Goal**: Memory pool initialization and allocation

**Independent Test**: Pre-allocate pool, allocate buffer, verify pointer is within pool bounds

### Implementation

- [X] T007 [US1] Implement mem_pool_init() in include/dag/mem_pool.h
- [X] T008 [US1] Implement mem_pool_alloc() in include/dag/mem_pool.h (FIFO tail advance)

---

## Phase 4: User Story 2 - when2free Registration (Priority: P1)

**Goal**: when2free FIFO registration mechanism

**Independent Test**: Call when2free(), verify entry is in FIFO

### Implementation

- [X] T009 [US2] Implement mem_pool_when2free() in include/dag/mem_pool.h
- [X] T010 [US2] Implement when2free_fifo_push() in include/dag/mem_pool.h
- [X] T011 [US2] Implement when2free_fifo_pop() in include/dag/mem_pool.h

---

## Phase 5: User Story 5 - Automatic Memory Release (Priority: P1)

**Goal**: FIFO head pointer update when threshold reached

**Independent Test**: Process when2free entries, verify head pointer advances

### Implementation

- [X] T012 [US5] Implement mem_pool_process_when2free() in include/dag/mem_pool.h
- [X] T013 [US5] Implement mem_pool_min_uncompleted() in include/dag/mem_pool.h (calls ring_min_uncompleted)

---

## Phase 6: User Story 10 - Manager Thread (Priority: P1)

**Goal**: Dedicated manager thread for automatic release

**Independent Test**: Start manager thread, register when2free, verify memory released after threshold

### Implementation

- [X] T014 [US10] Implement mem_pool_manager() entry point in src/mem_pool.c
- [X] T015 [US10] Add manager thread loop with ring buffer polling in src/mem_pool.c

---

## Phase 7: Polish & Cross-Cutting

**Purpose**: Finalization and verification

- [X] T016 Verify all functions have no input validation (Trust the Caller)
- [X] T017 Ensure C11 atomics used for head/tail/fifo operations
- [X] T018 Run compilation check with -std=c11

---

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 (Setup): No dependencies
- Phase 2 (Foundational): Depends on Phase 1
- Phase 3 (US1): Depends on Phase 2
- Phase 4 (US2): Depends on Phase 3
- Phase 5 (US5): Depends on Phase 4
- Phase 6 (US10): Depends on Phase 5
- Phase 7 (Polish): Depends on all implementation phases

### Sequential Execution

Tasks within same phase are sequential (T003-T006 must complete before T007).

### Parallel Opportunities

T003, T004, T005, T006 can be implemented in parallel since they define separate types/constants.

---

## File Structure (Target)

```text
include/dag/
└── mem_pool.h     # Header file (types, inline functions)

src/
└── mem_pool.c     # Implementation file (manager thread)
```

---

## Implementation Notes

- No validation in any function (Trust the Caller - Principle X)
- Use C11 `_Atomic` for head, tail, fifo indices
- mem_pool_manager() runs in dedicated thread, polls ring buffer for min_uncompleted
- when2free release: update pool->head to addr when min_uncompleted >= taskid