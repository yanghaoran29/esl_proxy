# Tasks: Async Task Executor

**Input**: Design documents from `/specs/003-executor/`

**Prerequisites**: plan.md (required), spec.md (required), data-model.md, quickstart.md

**Tests**: Not requested

## Format: `[ID] [P?] [Story] Description`

## Phase 1: Setup (Project Structure)

**Purpose**: Verify/create project structure

- [ ] T001 Verify include/dag/ directory exists with task.h and executor.h
- [ ] T002 Verify src/ directory exists, create if missing

---

## Phase 2: Foundational (Core Types)

**Purpose**: Define core data structures

### Implementation

- [ ] T003 [P] Define task_state_t enum in include/dag/task.h
- [ ] T004 [P] Define task_t struct in include/dag/task.h
- [ ] T005 [P] Define executor_t struct in include/dag/executor.h
- [ ] T006 Define EXECUTOR_SLOTS constant (2) in include/dag/executor.h

---

## Phase 3: User Story 1 - Task Submission and Caching (Priority: P1) 🎯 MVP

**Goal**: Submit tasks and cache when worker busy

**Independent Test**: Submit task, verify cached when worker busy, executed when idle

### Implementation

- [ ] T007 [US1] Implement executor_init() in include/dag/executor.h
- [ ] T008 [US1] Implement executor_submit() in include/dag/executor.h
- [ ] T009 [US1] Implement slot finding logic (find empty slot)

---

## Phase 4: User Story 2 - PING PONG Slot Selection (Priority: P1)

**Goal**: Fair alternation between 2 slots

**Independent Test**: Fill both slots, verify Executor selects in alternation

### Implementation

- [ ] T010 [US2] Implement PING PONG slot selection logic
- [ ] T011 [US2] Implement atomic ping_pong toggle

---

## Phase 5: User Story 3 - Async Task Execution (Priority: P1)

**Goal**: Async task execution with delay

**Independent Test**: Submit task with delay, verify caller not blocked

### Implementation

- [ ] T012 [US3] Implement executor_worker() in src/executor.c
- [ ] T013 [US3] Implement delay execution (use sleep/usleep per Constitution no-blocking, but delay is task-level not hot path)
- [ ] T014 [US3] Implement kernel invocation

---

## Phase 6: User Story 4 - Task Result Retrieval (Priority: P1)

**Goal**: Query task status and retrieve results

**Independent Test**: Execute task, query status, verify result available

### Implementation

- [ ] T015 [US4] Implement executor_status() query function
- [ ] T016 [US4] Implement executor_shutdown()

---

## Phase 7: Polish & Cross-Cutting

**Purpose**: Finalization and verification

### Implementation

- [ ] T017 Compile check with -std=c11 -Wall -Werror
- [ ] T018 Verify all functions follow Trust the Caller (no validation)

---

## Dependencies & Execution Order

### Phase Dependencies

- Phase 1 (Setup): No dependencies
- Phase 2 (Foundational): Depends on Phase 1
- Phase 3 (US1): Depends on Phase 2
- Phase 4 (US2): Depends on Phase 3
- Phase 5 (US3): Depends on Phase 4
- Phase 6 (US4): Depends on Phase 5
- Phase 7 (Polish): Depends on all implementation phases

### Sequential Execution

T003-T006 can be implemented in parallel (separate types).

### Parallel Opportunities

T003, T004, T005, T006 are independent type definitions.

---

## File Structure (Target)

```text
include/dag/
├── executor.h     # Executor type and functions (header-only)
└── task.h         # Task type (shared)

src/
└── executor.c     # Worker thread implementation
```

---

## Implementation Notes

- Header-only library: all logic in include/dag/executor.h except worker thread
- Use C11 `_Atomic` for slot_state and ping_pong
- Trust the Caller: no input validation in any function
- Delay is per-task, not in hot path (worker thread sleep is acceptable)