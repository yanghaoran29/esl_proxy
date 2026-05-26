# Tasks: MPMC Queue

**Input**: Design documents from `/specs/010-mpmc-queue/`

**Prerequisites**: plan.md (required), spec.md (required for user stories)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup (Project Initialization)

**Purpose**: Create project structure and basic configuration

- [ ] T001 [P] Create include/dag/mpmc_queue.h with header guard DAG_MPMC_QUEUE_H, mpmc_status_t enum, mpmc_queue_t struct
- [ ] T002 [P] Create include/dag/mpmc_queue.c for global queue definitions
- [ ] T003 [P] Create include/dag/ready_queue.h with 2D queue matrix API
- [ ] T004 [P] Create include/dag/ready_queue.c for global ReadyQueue matrix
- [ ] T005 [P] Create include/dag/complete_queue.h with global CompleteQueue API
- [ ] T006 [P] Create include/dag/complete_queue.c for global CompleteQueue definition

---

## Phase 2: Foundational (Core MPMC Queue Infrastructure)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

- [ ] T007 Implement mpmc_init() in mpmc_queue.h - initialize queue with capacity and elem_size
- [ ] T008 [P] Implement mpmc_idx() function using pos % capacity
- [ ] T009 Implement mpmc_enqueue() with atomic head/tail, circular buffer copy
- [ ] T010 Implement mpmc_dequeue() with atomic head/tail, circular buffer copy
- [ ] T011 Implement mpmc_size() returning tail - head approximate size
- [ ] T012 [P] Define global mpmc_queue_t g_ready_queues[3][4] in ready_queue.c
- [ ] T013 [P] Define global mpmc_queue_t g_complete_queue in complete_queue.c
- [ ] T014 Implement ready_queue_get(type, mode) inline accessor
- [ ] T015 Implement complete_enqueue() and complete_dequeue() inline accessors

**Checkpoint**: Core MPMC queue infrastructure ready

---

## Phase 3: User Story 1 - Task Dispatch via MPMC Queue (Priority: P1)

**Goal**: Basic enqueue/dequeue operations

**Independent Test**: Verify multiple producers/consumers can enqueue/dequeue concurrently without loss

- [ ] T016 [US1] Verify mpmc_enqueue() stores item in circular buffer
- [ ] T017 [US1] Verify mpmc_dequeue() retrieves item from circular buffer

---

## Phase 4: User Story 2 - Bounded Queue with Backpressure (Priority: P1)

**Goal**: Queue capacity limits and backpressure behavior

**Independent Test**: Verify enqueue returns MPMC_FULL when queue is at capacity

- [ ] T018 [US2] Verify mpmc_enqueue() returns MPMC_FULL when tail - head >= capacity
- [ ] T019 [US2] Verify mpmc_dequeue() creates available capacity after removal

---

## Phase 5: User Story 3 - FIFO Ordering (Priority: P2)

**Goal**: FIFO ordering for sequential enqueue/dequeue

**Independent Test**: Verify items dequeue in same order as enqueued

- [ ] T020 [US3] Verify sequential enqueue/dequeue maintains FIFO order

---

## Phase 6: User Story 4 - Non-Blocking Dequeue Option (Priority: P2)

**Goal**: Non-blocking dequeue operation

**Independent Test**: Verify dequeue returns immediately with empty status when queue is empty

- [ ] T021 [US4] Verify mpmc_dequeue() returns MPMC_EMPTY immediately when queue is empty

---

## Phase 7: User Story 5 - Memory-Efficient Circular Implementation (Priority: P3)

**Goal**: Circular buffer with wraparound behavior

**Independent Test**: Verify buffer wraparound and memory reuse

- [ ] T022 [US5] Verify mpmc_idx() correctly wraps index using pos % capacity

---

## Phase 8: User Story 6 - Batch Enqueue (Priority: P2)

**Goal**: Batch enqueue of multiple items

**Independent Test**: Verify batch of 10 items can be enqueued in single call

- [ ] T023 [US6] Implement mpmc_enqueue_batch() with loop copying items
- [ ] T024 [US6] Verify mpmc_enqueue_batch() returns actual count enqueued
- [ ] T025 [US6] Verify partial batch when count exceeds available capacity

---

## Phase 9: User Story 7 - Batch Dequeue (Priority: P2)

**Goal**: Batch dequeue of multiple items

**Independent Test**: Verify batch of 10 items can be dequeued in single call

- [ ] T026 [US7] Implement mpmc_dequeue_batch() with loop copying items
- [ ] T027 [US7] Verify mpmc_dequeue_batch() returns actual count dequeued
- [ ] T028 [US7] Verify partial batch when fewer items than requested available

---

## Phase 10: User Story 8 - Batch Size Limits and Partial Results (Priority: P2)

**Goal**: Accurate partial batch handling

**Independent Test**: Verify batch API returns accurate count when fewer items than requested

- [ ] T029 [US8] Verify mpmc_enqueue_batch() handles partial batch correctly
- [ ] T030 [US8] Verify mpmc_dequeue_batch() handles partial batch correctly

---

## Phase 11: User Story 9 - Per-TaskType+OrgMode ReadyQueues (Priority: P1)

**Goal**: 2D ReadyQueue matrix (task_type × org_mode)

**Independent Test**: Verify tasks routed to correct queue based on type and org_mode

- [ ] T031 [US9] Implement ready_enqueue(type, mode, item) inline function
- [ ] T032 [US9] Implement ready_dequeue(type, mode, item) inline function
- [ ] T033 [US9] Verify 12 queue combinations (3 types × 4 modes) accessible

---

## Phase 12: User Story 10 - Global ReadyQueue Matrix Access (Priority: P1)

**Goal**: O(1) lookup via 2D indexing

**Independent Test**: Verify ready_queue_get() returns correct queue in O(1)

- [ ] T034 [US10] Implement ready_queue_get(type, mode) returning queue pointer
- [ ] T035 [US10] Verify O(1) access via direct array indexing

---

## Phase 13: User Story 11 - CompleteQueue for Task Completion Tracking (Priority: P1)

**Goal**: Record completed task notifications

**Independent Test**: Verify completion notifications can be enqueued and dequeued

- [ ] T036 [US11] Implement complete_enqueue() inline function
- [ ] T037 [US11] Implement complete_dequeue() inline function
- [ ] T038 [US11] Verify completion notifications recorded in CompleteQueue

---

## Phase 14: User Story 12 - Global CompleteQueue Access (Priority: P1)

**Goal**: Global CompleteQueue visibility

**Independent Test**: Verify g_complete_queue is globally accessible

- [ ] T039 [US12] Verify g_complete_queue global variable exists
- [ ] T040 [US12] Verify workers can enqueue without queue references

---

## Phase 15: Polish & Cross-Cutting Concerns

**Purpose**: Verification and cleanup

- [ ] T041 [P] Verify all queue implementations compile with clang -std=c11
- [ ] T042 [P] Verify C11 atomics usage (_Atomic, atomic_load/store)
- [ ] T043 Update checklist status and commit changes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-14)**: All depend on Foundational phase completion
- **Polish (Phase 15)**: Depends on all user stories being complete

### User Story Dependencies

- **US1-8**: Depend on Foundational (Phase 2) - core MPMC operations
- **US9-10**: Depend on Foundational (Phase 2) - ReadyQueue matrix
- **US11-12**: Depend on Foundational (Phase 2) - CompleteQueue

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- User stories 1-8 can proceed in parallel after Foundational
- User stories 9-10 can proceed in parallel after Foundational
- User stories 11-12 can proceed in parallel after Foundational

---

## Implementation Strategy

### MVP First (User Story 1 + 2 as core)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3-4: US1-2 (basic enqueue/dequeue, bounded)
4. **STOP and VALIDATE**: Core MPMC queue working

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add US1-2 → Core queue operations working
3. Add US3-8 → Batch operations working
4. Add US9-10 → 2D ReadyQueue matrix working
5. Add US11-12 → CompleteQueue working
6. Polish → Complete

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- All queue operations are O(1)
- Lock-free operations use C11 atomics only
- No mutexes in hot paths