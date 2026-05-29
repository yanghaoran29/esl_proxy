# Tasks: Dispatch

**Input**: Design documents from `/specs/004-dispatch/`

**Prerequisites**: plan.md (required), spec.md (required), data-model.md, quickstart.md

**Tests**: Not requested

## Format: `[ID] [P?] [Story] Description`

## Phase 1: Setup (Project Structure)

**Purpose**: Verify/create project structure

- [X] T001 Verify include/dag/ directory exists with shm_buf.h and dispatch.h
- [X] T002 Verify src/ directory exists, create if missing

---

## Phase 2: Foundational (Core Types)

**Purpose**: Define core data structures

### Implementation

- [X] T003 [P] Define shm_buf_t struct in include/dag/shm_buf.h
- [X] T004 [P] Define task_type_t enum (TASK_CUBE, TASK_VECTOR, TASK_MIX) in include/dag/shm_buf.h
- [X] T005 [P] Define exec_capability_t enum (EXEC_CUBE, EXEC_VECTOR, EXEC_MIX) in include/dag/shm_buf.h
- [X] T006 Define DISPATCH_EXECUTOR_COUNT (120), CUBE_COUNT (60), VECTOR_COUNT (60) constants
- [X] T007 Define completion bit macros (completion_bit_read, completion_bit_set)

---

## Phase 3: User Story 1/1b/1c - Shared Memory Communication (Priority: P1) 🎯 MVP

**Goal**: Read from Orchestrator, write to Executor, receive completion signals

**Independent Test**: Write to shared memory, verify data read correctly, set completion bit

### Implementation

- [X] T008 [US1] Implement shm_buf_init() in include/dag/shm_buf.h
- [X] T009 [US1] Implement shm_buf_read() lock-free SPSC read in include/dag/shm_buf.h
- [X] T010 [US1] Implement shm_buf_write() lock-free SPSC write in include/dag/shm_buf.h
- [X] T011 [US1] Implement dispatch_init() in include/dag/dispatch.h
- [X] T012 [US1] Implement dispatch_write_task_to_executor() in include/dag/dispatch.h
- [X] T013 [US1] Implement completion_bit_read() in include/dag/dispatch.h

---

## Phase 4: User Story 2 - Dual Source Task Acquisition (Priority: P1)

**Goal**: Read tasks from both Orchestrator and Cutter shared memory without duplication

**Independent Test**: Write tasks to both sources, verify dispatch reads from both

### Implementation

- [X] T014 [US2] Implement dispatch_acquire_task() dual-source acquisition
- [X] T015 [US2] Implement dispatch_has_work() to check both sources

---

## Phase 5: User Story 3 - Orchestrator Output Integration (Priority: P1)

**Goal**: Read task graph topology and ready tasks from Orchestrator shared memory

**Independent Test**: Write ready tasks to shared memory, verify Dispatch reads correctly

### Implementation

- [X] T016 [US3] Implement dispatch_read_orchestrator_output() in include/dag/dispatch.h
- [X] T017 [US3] Implement dispatch_sync_shm() for shared memory synchronization

---

## Phase 6: User Story 4/4b - Executor Pool Management (Priority: P1)

**Goal**: Each Dispatch manages 60 CUBE + 60 VECTOR Executors, routes MIX tasks to dual-capable

**Independent Test**: Create Dispatch, verify 120 Executors, route CUBE/VECTOR/MIX tasks correctly

### Implementation

- [X] T018 [US4] Define dispatch_t struct with executor_pool[120] in include/dag/dispatch.h
- [X] T019 [US4] Implement dispatch_find_idle_cube() in include/dag/dispatch.h
- [X] T020 [US4] Implement dispatch_find_idle_vector() in include/dag/dispatch.h
- [X] T021 [US4] Implement dispatch_find_idle_mix() for dual-capable Executor
- [X] T022 [US4] Implement dispatch_route_task() with capability routing

---

## Phase 7: User Story 6 - Work-Stealing Load Balancing (Priority: P1)

**Goal**: Idle Executors steal work from busy Dispatches respecting dependencies

**Independent Test**: Create Dispatch A (busy) and B (idle), verify work-stealing occurs

### Implementation

- [X] T023 [US6] Implement dispatch_steal_from() in include/dag/dispatch.h
- [X] T024 [US6] Implement dispatch_find_busiest() to locate steal source
- [X] T025 [US6] Implement dispatch_check_dependency() to verify predecessors satisfied

---

## Phase 8: User Story 7 - Shared Memory Synchronization (Priority: P2)

**Goal**: Coordinate access between Orchestrator writer and Dispatch reader

**Independent Test**: Concurrent access simulation, verify no corruption

### Implementation

- [X] T026 [US7] Implement dispatch_sync_reader() atomic coordination
- [X] T027 [US7] Implement dispatch_sync_writer() atomic coordination

---

## Phase 9: User Story 8 - Distributed Task Distribution (Priority: P2)

**Goal**: Dynamic addition/removal of Dispatch-Executor pairs with Work-Stealing updates

**Independent Test**: Add/remove Dispatch, verify Work-Stealing pool updates

### Implementation

- [X] T028 [US8] Implement dispatch_register() for Work-Stealing pool
- [X] T029 [US8] Implement dispatch_unregister() for pool removal

---

## Phase 10: User Story 9 - Dispatch Lifecycle (Priority: P2)

**Goal**: Clean shutdown with shared memory detachment

**Independent Test**: Create and destroy Dispatch, verify no memory leaks

### Implementation

- [X] T030 [US9] Implement dispatch_shutdown() with shared memory cleanup
- [X] T031 [US9] Implement dispatch_detach_shm() shared memory detachment

---

## Phase 11: User Story 10 - Task Affinity (Priority: P3)

**Goal**: Route tasks to specific Dispatches based on affinity rules

**Independent Test**: Set affinity, verify tasks route to correct Dispatch

### Implementation

- [X] T032 [US10] Implement dispatch_check_affinity() affinity routing
- [X] T033 [US10] Implement dispatch_set_affinity() affinity configuration

---

## Phase 12: Polish & Cross-Cutting

**Purpose**: Finalization and verification

### Implementation

- [X] T034 Compile check with -std=c11 -Wall -Werror
- [X] T035 Verify all functions follow Trust the Caller (no validation)

---

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 (Setup): No dependencies
- Phase 2 (Foundational): Depends on Phase 1
- Phase 3 (US1): Depends on Phase 2
- Phase 4 (US2): Depends on Phase 3
- Phase 5 (US3): Depends on Phase 4
- Phase 6 (US4): Depends on Phase 5
- Phase 7 (US6): Depends on Phase 6
- Phase 8 (US7): Depends on Phase 7
- Phase 9 (US8): Depends on Phase 8
- Phase 10 (US9): Depends on Phase 9
- Phase 11 (US10): Depends on Phase 10
- Phase 12 (Polish): Depends on all implementation phases

### Sequential Execution

T003-T007 can be implemented in parallel (separate type definitions).

### Parallel Opportunities

T003, T004, T005, T006, T007 are independent type/constant definitions.

---

## File Structure (Target)

```text
include/dag/
├── shm_buf.h      # Shared memory buffer types and functions (header-only)
└── dispatch.h     # Dispatch type and functions (header-only)

src/
└── dispatch.c    # Dispatch implementation (if needed for worker thread)
```

---

## Implementation Notes

- Header-only library: all logic in include/dag/ headers except worker thread
- Use C11 `_Atomic` for tail/head indices and completion bits
- Trust the Caller: no input validation in any function
- Lock-free SPSC for shared memory queues
- 1-bit completion signals per Executor (64-bit word supports 64 Executors)