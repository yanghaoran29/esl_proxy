# Feature Specification: Pre-allocated Memory Pool

**Feature Branch**: `011-prealloc-memory`

**Created**: 2026-05-27

**Status**: Draft

**Input**: User description: "007-memory-pool预分配120M内存"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Pre-allocate 120MB Memory Pool (Priority: P1)

A system operator configures the memory pool to pre-allocate exactly 120 megabytes of memory before any task execution begins. The pre-allocated memory serves as the exclusive source for all task intermediate data allocations throughout the DAG execution lifetime.

**Why this priority**: Pre-allocation of 120MB ensures deterministic memory availability and eliminates dynamic allocation overhead during task execution, guaranteeing consistent microsecond-level allocation latency.

**Independent Test**: Can be verified by monitoring system memory usage before and after pool initialization, confirming exactly 120MB is reserved with no system malloc calls during task execution.

**Acceptance Scenarios**:

1. **Given** the memory pool is initialized, **When** the system reports pool size, **Then** the total capacity equals exactly 120 megabytes (125,829,120 bytes)
2. **Given** the 120MB pool is pre-allocated, **When** tasks allocate intermediate data, **Then** all allocations are served from the pre-allocated pool with no system calls
3. **Given** the 120MB pool is in use, **When** the system queries available memory, **Then** the available bytes reflect the 120MB total minus allocated portions
4. **Given** the 120MB pool is fully allocated, **When** a task requests more memory, **Then** the allocation fails gracefully with appropriate status rather than triggering system malloc

---

### User Story 2 - Pool Capacity Reuse via when2free (Priority: P1)

A system operator relies on the when2free mechanism to reclaim memory slots for reuse. When the minimum uncompleted TaskID advances past a buffer's registered threshold, that buffer's memory slot is returned to the available pool, allowing subsequent allocations to reuse the capacity.

**Why this priority**: Reusing pool capacity enables sustained task execution over long DAG pipelines without memory exhaustion, ensuring the 120MB pool can serve thousands of task allocations across the lifetime of a workload.

**Independent Test**: Can be tested by allocating buffers, registering them with when2free thresholds, advancing the minimum uncompleted TaskID past each threshold, and verifying freed capacity becomes available for new allocations.

**Acceptance Scenarios**:

1. **Given** a buffer is allocated from the 120MB pool and registered with when2free(taskID=10), **When** the minimum uncompleted TaskID advances to 11, **Then** the buffer's capacity is returned to the pool for reuse
2. **Given** the 120MB pool has 80MB allocated and 40MB available, **When** a 40MB buffer is freed via when2free, **Then** the pool now has 80MB available for new allocations
3. **Given** the 120MB pool reaches full allocation, **When** buffers are freed via when2free, **Then** new allocations succeed as capacity becomes available
4. **Given** continuous allocation and when2free-free cycles occur, **When** the pool reaches steady state, **Then** total allocated never exceeds 120MB (no leaks)

---

### User Story 3 - Memory Pool SPSC Mode with Pre-allocated Capacity (Priority: P1)

A system operator configures the pre-allocated 120MB pool to operate in Single Producer Single Consumer (SPSC) mode. The Orchestrator (producer) allocates buffers for task inputs, and the Manager thread (consumer) frees them when when2free thresholds are reached.

**Why this priority**: SPSC mode with a fixed pre-allocated pool ensures deterministic memory behavior without complex synchronization overhead, enabling the 120MB capacity to be managed efficiently throughout the workload lifetime.

**Independent Test**: Can be tested by running a workload that allocates and frees buffers through the SPSC mechanism, verifying no allocation conflicts occur and memory capacity is correctly maintained.

**Acceptance Scenarios**:

1. **Given** the 120MB pool operates in SPSC mode, **When** the Orchestrator allocates buffers, **Then** all allocations are served from the pre-allocated pool
2. **Given** the 120MB pool operates in SPSC mode, **When** the Manager frees buffers via when2free, **Then** freed capacity is returned to the pool for reuse
3. **Given** the 120MB pool operates in SPSC mode, **When** allocation and free cycles execute, **Then** total memory usage never exceeds 120MB
4. **Given** the 120MB pool operates in SPSC mode, **When** the workload completes, **Then** all 120MB is returned to available state (pool can be destroyed cleanly)

---

### User Story 4 - Pool Initialization with Explicit 120MB Capacity (Priority: P1)

A system operator initializes the memory pool with an explicit capacity parameter of 120 megabytes. The initialization call allocates the full 120MB upfront and configures internal data structures for managing the fixed capacity.

**Why this priority**: Explicit capacity specification allows the system operator to control memory consumption and tune the pool size based on workload characteristics and available system memory.

**Independent Test**: Can be tested by calling pool initialization with 120MB capacity and verifying the returned pool handle reports exactly 120MB total capacity with 120MB available.

**Acceptance Scenarios**:

1. **Given** pool initialization is called with capacity=120MB, **When** the call returns, **Then** the pool has exactly 125,829,120 bytes of total capacity
2. **Given** pool initialization with 120MB, **When** the system queries pool metadata before any allocations, **Then** available bytes equals 120MB and allocated bytes equals 0
3. **Given** pool initialization with 120MB, **When** the first allocation is requested, **Then** the allocation is served immediately from pre-allocated capacity
4. **Given** pool initialization with 120MB, **When** pool initialization fails (insufficient system memory), **Then** an appropriate error status is returned without partial state

---

### User Story 5 - Monitor Pre-allocated Pool Utilization (Priority: P2)

A system operator monitors the utilization of the pre-allocated 120MB pool to understand memory consumption patterns. The operator queries pool metadata to see total, allocated, and available bytes.

**Why this priority**: Visibility into pool utilization enables operators to confirm the 120MB pool is properly sized and detect potential issues before memory exhaustion impacts task execution.

**Independent Test**: Can be tested by running a workload that allocates known amounts, querying pool metadata after each allocation, and verifying the metadata reflects actual usage accurately.

**Acceptance Scenarios**:

1. **Given** the 120MB pool is initialized, **When** the operator queries pool metadata, **Then** total bytes = 125,829,120, allocated = 0, available = 125,829,120
2. **Given** the 120MB pool has 60MB allocated, **When** the operator queries pool metadata, **Then** total bytes = 125,829,120, allocated = 62,914,560, available = 62,914,560
3. **Given** the 120MB pool is near exhaustion, **When** the operator monitors utilization, **Then** the operator can make informed decisions about throttling new allocations
4. **Given** the 120MB pool is at steady state with continuous allocation and when2free cycles, **When** the operator monitors utilization over time, **Then** allocated and available values fluctuate correctly reflecting pool activity

---

### Edge Cases

- What happens when 120MB is not available on the system (insufficient memory)?
- What happens when allocation size exceeds remaining capacity in the 120MB pool?
- What happens when when2free is called with a taskID that never completes (orphan buffer)?
- What happens when the pool is destroyed while tasks are still using allocated buffers?
- What happens when the minimum uncompleted TaskID advances very slowly (buffers held for long time)?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST pre-allocate exactly 120 megabytes (125,829,120 bytes) of memory for the memory pool at initialization
- **FR-002**: All task intermediate data allocations MUST be served from the pre-allocated 120MB pool with no system malloc calls
- **FR-003**: The pre-allocated pool MUST report accurate metadata: total capacity (125,829,120 bytes), allocated bytes, available bytes
- **FR-004**: Allocation requests that exceed remaining pool capacity MUST return failure status without triggering system malloc
- **FR-005**: when2free-registered buffers MUST have their capacity returned to the available pool when threshold conditions are met
- **FR-006**: The pool MUST operate in SPSC mode with Orchestrator as producer and Manager as consumer
- **FR-007**: Pool initialization with 120MB capacity MUST fail with error status if insufficient system memory is available
- **FR-008**: The total allocated bytes at any time MUST never exceed the pre-allocated capacity (125,829,120 bytes)
- **FR-009**: Pool capacity freed via when2free MUST be immediately available for new allocations
- **FR-010**: The system MUST support querying pool utilization (total, allocated, available) at any time

### Key Entities *(include if feature involves data)*

- **Memory Pool**: Pre-allocated 120MB region of memory from which task intermediate data is allocated. Attributes: total size (125,829,120 bytes), allocated bytes, available bytes.
- **Buffer Handle**: Reference to an allocated buffer within the pool. Used by tasks to read/write data and to free the buffer back to the pool.
- **Pool Metadata**: Information about pool state including total capacity, allocated bytes, available bytes.
- **when2free Entry**: Record containing buffer address and taskID threshold for automatic memory release.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Pool initialization with 120MB capacity completes and the pool reports exactly 125,829,120 bytes total capacity
- **SC-002**: Allocation from the pre-allocated pool completes in under 1 microsecond with no system malloc calls
- **SC-003**: Pool metadata queries return values that accurately reflect current utilization (allocated + available = 125,829,120)
- **SC-004**: Allocated bytes never exceed 125,829,120 at any point during workload execution
- **SC-005**: when2free-triggered capacity return makes freed memory available for new allocations within 1 microsecond
- **SC-006**: The system handles pool exhaustion gracefully with allocation failure status rather than crash or system malloc

## Assumptions

- The 120MB pool size is fixed at initialization and cannot be dynamically resized during runtime
- System has sufficient memory (120MB plus overhead for internal data structures) available at pool initialization
- The memory pool is private to a single Executor/Orchestrator pair (not shared across processes)
- when2free threshold advancement is monotonic (minimum uncompleted TaskID only increases)
- Pool destruction is only called when no active allocations remain (caller responsibility)
- Memory pool operates in SPSC mode where Orchestrator allocates and Manager frees via when2free