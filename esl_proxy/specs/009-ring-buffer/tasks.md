# Tasks: Task Ring Buffers

**Input**: Design documents from `/specs/009-ring-buffer/`

**Prerequisites**: plan.md (required), spec.md (required for user stories)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Project Initialization)

**Purpose**: Create project structure and basic configuration

- [ ] T001 [P] Create include/dag/ring_buf.h with header guard DAG_RING_BUF_H
- [ ] T002 [P] Create include/dag/ring_buf.c for global variable definitions
- [ ] T003 Configure C11 build flags in Makefile/CMakeLists

---

## Phase 2: Foundational (Core Ring Buffer Infrastructure)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

- [ ] T004 Define RING_SIZE (4096) and RING_MASK constants in ring_buf.h
- [ ] T005 Create ring_cat_t enum (STATE, BASIC, DEP, RUNTIME)
- [ ] T006 [P] Declare extern global g_state_buf[RING_SIZE] in ring_buf.h
- [ ] T007 [P] Declare extern global g_basic_buf[RING_SIZE] in ring_buf.h
- [ ] T008 [P] Declare extern global g_dep_buf[RING_SIZE] in ring_buf.h
- [ ] T009 [P] Declare extern global g_runtime_buf[RING_SIZE] in ring_buf.h
- [ ] T010 Define 4 global ring buffers in ring_buf.c
- [ ] T011 [P] Implement ring_idx() function using TaskID & RING_MASK
- [ ] T012 Include dag/task.h and verify task_desc_t, dep_base_t types

**Checkpoint**: Foundational ring buffer infrastructure ready

---

## Phase 3: User Story 1-3 - Ring Buffer Basics (Priority: P1)

**Goal**: Verify ring buffer size, indexing, and O(1) access

**Independent Test**: Verify index wraparound and bitwise AND computation

- [ ] T013 [US1] Verify RING_SIZE is 4096 (power of 2) - compile-time check
- [ ] T014 [US2] Verify ring_idx(4096) returns 0 (wraparound)
- [ ] T015 [US2] Verify ring_idx(5000) returns 904 (5000 & 4095)
- [ ] T016 [US3] Verify O(1) access time via single bitwise AND

---

## Phase 4: User Story 4 - Task State Storage (Priority: P1)

**Goal**: Store task execution state in state ring buffer

**Independent Test**: Write state to state buffer and read back

- [ ] T017 [US4] Write task state to g_state_buf[ring_idx(task_id)]
- [ ] T018 [US4] Read task state from g_state_buf[ring_idx(task_id)]

---

## Phase 5: User Story 5 - Task Basic Information Storage (Priority: P1)

**Goal**: Store task basic info in basic info ring buffer

**Independent Test**: Write task_desc_t to basic buffer and read back

- [ ] T019 [US5] Write task_desc_t to g_basic_buf[ring_idx(task_id)]
- [ ] T020 [US5] Read task_desc_t from g_basic_buf[ring_idx(task_id)]

---

## Phase 6: User Story 6 - Task Dependency Information Storage (Priority: P1)

**Goal**: Store task dependency info in dependency ring buffer

**Independent Test**: Write dep_base_t to dependency buffer and read back

- [ ] T021 [US6] Write dep_base_t to g_dep_buf[ring_idx(task_id)]
- [ ] T022 [US6] Read dep_base_t from g_dep_buf[ring_idx(task_id)]

---

## Phase 7: User Story 7 - Task Runtime Information Storage (Priority: P1)

**Goal**: Store task runtime info in runtime ring buffer

**Independent Test**: Write runtime pointer to runtime buffer and read back

- [ ] T023 [US7] Write void* runtime data to g_runtime_buf[ring_idx(task_id)]
- [ ] T024 [US7] Read void* runtime data from g_runtime_buf[ring_idx(task_id)]

---

## Phase 8: User Story 8 - Conditional State Insert (Priority: P1)

**Goal**: Atomic state insert with empty-check using CAS

**Independent Test**: Insert into empty succeeds, insert into non-empty fails

- [ ] T025 [US8] Implement state_put_if_empty() with atomic CAS
- [ ] T026 [US8] Verify state_put_if_empty() returns 0 on empty insert success
- [ ] T027 [US8] Verify state_put_if_empty() returns -1 on non-empty insert fail

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Verification and cleanup

- [ ] T028 [P] Verify all 4 ring buffers compile and link correctly
- [ ] T029 [P] Verify C11 atomics usage throughout
- [ ] T030 Update checklist status and commit changes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-8)**: All depend on Foundational phase completion
  - User stories can proceed in parallel (if staff available)
- **Polish (Phase 9)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1-3**: Can start after Foundational (Phase 2)
- **User Story 4**: Can start after Foundational (Phase 2)
- **User Story 5**: Can start after Foundational (Phase 2)
- **User Story 6**: Can start after Foundational (Phase 2) - depends on dep_base_t from task.h
- **User Story 7**: Can start after Foundational (Phase 2)
- **User Story 8**: Can start after Foundational (Phase 2) - uses g_state_buf

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- Once Foundational phase completes, all user stories can start in parallel
- All user story phases marked [P] can run in parallel

---

## Implementation Strategy

### MVP First (User Story 4 as core)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1-3 (basic indexing verification)
4. Complete Phase 8: User Story 8 (conditional insert - key feature)
5. **STOP and VALIDATE**: Core ring buffer with conditional insert working

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add US1-3 → Indexing verified
3. Add US8 → Conditional insert working (core feature)
4. Add US4-7 → All 4 buffers functional
5. Polish → Complete

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- All ring buffer access is O(1) via ring_idx()
- Lock-free operations use C11 atomics only
- No mutexes in hot paths