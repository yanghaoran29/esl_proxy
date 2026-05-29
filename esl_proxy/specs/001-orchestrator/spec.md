# Feature Specification: Orchestrator

**Feature Branch**: `001-orchestrator`

**Created**: 2026-05-22

**Status**: Draft

**Input**: User description: "目标：编排任务依赖关系。任务声明：用户通过传递输入数据地址、输出数据地址、常量、Kernel地址来创建 Task。拓扑编排：支持 A.precede(B)（A 在 B 之前执行）或 B.succeed(A)（B 在 A 之后执行）的操作符或方法。+ 入口函数void aicpu_orchestration_entry(const ChipStorageTaskArgs& orch_args) + orchestrator为主线程，负责创建1个manager线程，2个dispatch线程和2个cutter线程，以及120个Executor线程 + aicpu_orchestration_entry调用mem_pool_alloc接口完成内存分配 + aicpu_orchestration_entry调用mem_pool_when2free接口注册内存释放时机"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Task Graph Construction (Priority: P1)

A data engineer needs to define a pipeline of computation tasks where each task depends on the outputs of previous tasks. They create a Graph, add Tasks representing each computational step, and wire them together using precedence constraints.

**Why this priority**: This is the core use case - users must be able to build a DAG of tasks with dependencies.

**Independent Test**: Can be tested by creating a simple 3-task graph, verifying that execution respects the dependency order.

**Acceptance Scenarios**:

1. **Given** an empty Graph, **When** the user adds Task A and Task B, **Then** both tasks exist in the graph with no dependency between them
2. **Given** Task A and Task B exist in a Graph, **When** the user calls `A.precede(B)`, **Then** B will execute after A completes
3. **Given** Task A and Task B exist in a Graph, **When** the user calls `B.succeed(A)`, **Then** the same dependency is established as `A.precede(B)`

---

### User Story 2 - Task Data Binding (Priority: P1)

A data engineer needs to specify what data each task reads from and writes to. They provide memory addresses for input/output data, constants, and the kernel (computation function) when creating each Task.

**Why this priority**: Task execution requires knowing the data sources and destinations - this is fundamental to task operation.

**Independent Test**: Can be tested by creating Tasks with data bindings and verifying the Graph captures these bindings correctly.

**Acceptance Scenarios**:

1. **Given** a user creates a Task with input address X, output address Y, input output address W, constant Z, duration M, subTaskCnt N , taskType V and kernel address K, **When** the Task is added to a Graph, **Then** the Graph stores these bindings
2. **Given** a Task with input bound to output of another Task, **When** the Graph executes, **Then** data flows from the producing Task to the consuming Task

---

### User Story 3 - Parallel Execution Planning (Priority: P2)

A data engineer wants to understand how their task graph will be executed. The Graph should expose information about execution order - specifically, which tasks can run in parallel.

**Why this priority**: Users need to verify the scheduler will exploit parallelism as expected.

**Independent Test**: Can be tested by creating a diamond dependency pattern and verifying tasks on different branches can execute concurrently.

**Acceptance Scenarios**:

1. **Given** a Graph with a diamond pattern (A→B, A→C, B→D, C→D), **When** the user queries parallel execution groups, **Then** {A} is in the first group, {B, C} in the second, and {D} in the third
2. **Given** a Graph is valid (no cycles), **When** the user requests topological order, **Then** the returned order respects all precedence constraints

---

### User Story 4 - User Entry Point (Priority: P1)

A system operator invokes the orchestrator through a single entry point function `aicpu_orchestration_entry` that receives a `ChipStorageTaskArgs` structure containing all necessary configuration for task graph construction and execution.

**Why this priority**: This is the primary interface for external callers to invoke the orchestration system. It provides a clean, unified entry point that encapsulates all necessary setup and execution logic.

**Independent Test**: Can be tested by calling `aicpu_orchestration_entry` with valid arguments and verifying the orchestrator correctly constructs and executes the task graph.

**Acceptance Scenarios**:

1. **Given** a valid `ChipStorageTaskArgs` structure containing task graph configuration, **When** the caller invokes `aicpu_orchestration_entry`, **Then** the orchestrator initializes and executes the task graph as configured
2. **Given** the orchestrator has completed execution, **When** `aicpu_orchestration_entry` returns, **Then** all tasks have been executed according to their precedence constraints

---

### User Story 5 - Threaded Orchestration Execution (Priority: P1)

A system operator triggers the orchestration system by providing the `aicpu_orchestration_entry` function to be executed in a dedicated thread. The system creates one thread that runs the provided entry function, allowing asynchronous orchestration while the caller continues execution.

**Why this priority**: Running orchestration in a dedicated thread enables non-blocking behavior so the caller can perform other operations while the task graph executes.

**Independent Test**: Can be tested by calling the orchestration launcher with `aicpu_orchestration_entry`, verifying that the function is invoked in a new thread and the caller returns immediately without waiting for completion.

**Acceptance Scenarios**:

1. **Given** a valid `aicpu_orchestration_entry` function pointer, **When** the caller invokes the orchestration launcher, **Then** a new thread is created to execute the function asynchronously
2. **Given** the thread has been created, **When** the caller does not wait, **Then** the caller can continue execution while the thread runs the orchestration
3. **Given** the orchestration thread is running, **When** the caller waits for completion, **Then** the caller can synchronize with the thread to obtain execution results
4. **Given** the orchestration thread encounters an error, **When** the thread completes, **Then** the error status is captured and available for retrieval

---

### User Story 7 - Orchestrator Main Thread and Worker Thread Creation (Priority: P1)

The Orchestrator serves as the main thread that creates 1 Manager thread, 2 Dispatch threads, 2 Cutter threads, and 120 Executor threads to parallelize task execution. The Orchestrator bootstraps the entire system by creating these worker threads at startup, each responsible for specific functionality: Manager handles memory management and when2free processing, Dispatch threads distribute tasks to executors, Cutter threads handle task cutting/preprocessing, and Executor threads execute the actual task kernels.

**Why this priority**: Having a clear thread topology ensures proper parallelism and separation of concerns. The Orchestrator as main thread creates and coordinates all worker threads, establishing the execution architecture.

**Independent Test**: Can be tested by verifying the Orchestrator creates exactly 125 worker threads (1 Manager, 2 Dispatch, 2 Cutter, 120 Executor) and each thread is running its designated function.

**Acceptance Scenarios**:

1. **Given** the Orchestrator starts, **When** initialization occurs, **Then** exactly 1 Manager thread is created to handle memory management and when2free processing
2. **Given** the Orchestrator continues initialization, **When** dispatcher setup proceeds, **Then** exactly 2 Dispatch threads are created to distribute tasks to executors
3. **Given** the Orchestrator completes dispatcher creation, **When** cutter setup proceeds, **Then** exactly 2 Cutter threads are created to handle task cutting/preprocessing
4. **Given** cutter threads are created, **When** executor setup proceeds, **Then** exactly 120 Executor threads are created to execute task kernels
5. **Given** all worker threads are created, **When** the Orchestrator runs, **Then** all 125 threads operate concurrently and coordinate through defined interfaces

---

### User Story 8 - Memory Allocation via mem_pool_alloc (Priority: P1)

The `aicpu_orchestration_entry` entry point uses the memory pool's `mem_pool_alloc` interface to allocate memory for task data buffers. Instead of calling system malloc/free, the Orchestrator allocates memory from a pre-allocated memory pool, ensuring O(1) allocation time and avoiding system call overhead during task execution. The memory pool interface is called during task graph construction and execution to allocate buffers for input data, output data, and intermediate results.

**Why this priority**: Pre-allocated memory pool allocation is essential for high-performance task execution. Using mem_pool_alloc ensures predictable allocation latency and prevents memory fragmentation during DAG execution.

**Independent Test**: Can be tested by verifying that aicpu_orchestration_entry calls mem_pool_alloc instead of system malloc, and that allocated memory comes from the pre-allocated pool.

**Acceptance Scenarios**:

1. **Given** `aicpu_orchestration_entry` is invoked, **When** task data buffers need to be allocated, **Then** mem_pool_alloc is called to obtain memory from the pre-allocated pool
2. **Given** mem_pool_alloc is called with a requested size, **When** the pool has available memory, **Then** memory is allocated from the pool in O(1) time
3. **Given** the memory pool is initialized, **When** aicpu_orchestration_entry runs, **Then** all memory allocations use mem_pool_alloc instead of system malloc

---

### User Story 9 - Memory Release Registration via mem_pool_when2free (Priority: P1)

The `aicpu_orchestration_entry` entry point uses the memory pool's `mem_pool_when2free` interface to register when allocated memory should be automatically released. After allocating memory via `mem_pool_alloc`, the Orchestrator registers the memory address with a task ID threshold via `mem_pool_when2free`. When the minimum uncompleted TaskID crosses the threshold, the memory is automatically released back to the pool (by updating the FIFO head pointer) without requiring explicit free calls.

**Why this priority**: Automatic memory release via when2free eliminates manual memory management and prevents memory leaks by ensuring buffers are made available precisely when all dependent tasks have completed.

**Independent Test**: Can be tested by verifying that aicpu_orchestration_entry calls mem_pool_when2free after allocating buffers, and that memory is released when the task ID threshold is reached.

**Acceptance Scenarios**:

1. **Given** memory is allocated via mem_pool_alloc, **When** aicpu_orchestration_entry registers the buffer with mem_pool_when2free(addr, taskid), **Then** the buffer is recorded for automatic release
2. **Given** a buffer is registered with mem_pool_when2free(addr, taskid=T), **When** the minimum uncompleted TaskID advances to T or beyond, **Then** the memory is automatically released back to the pool
3. **Given** multiple buffers are registered with different thresholds, **When** each threshold condition is met, **Then** each buffer is released in threshold order

---

### User Story 10 - Task ID Monotonic Increment (Priority: P1)

The Orchestrator assigns monotonically increasing TaskIDs to tasks during graph construction. Each newly created task receives a TaskID greater than all previously assigned TaskIDs. This monotonicity is essential for the when2free mechanism to correctly determine when memory can be released - since memory is released when min_uncompleted >= taskid threshold, task IDs must increase as task execution progresses.

**Why this priority**: Task ID monotonicity is a foundational requirement for the when2free automatic memory release mechanism. Without monotonically increasing TaskIDs, the system cannot correctly determine memory release timing, potentially causing use-after-free or memory leaks.

**Independent Test**: Can be tested by creating multiple tasks and verifying each receives a higher TaskID than all previously created tasks.

**Acceptance Scenarios**:

1. **Given** no tasks exist, **When** the first task is created, **Then** it receives TaskID = 0
2. **Given** N tasks exist with TaskIDs 0 to N-1, **When** task N+1 is created, **Then** it receives TaskID = N
3. **Given** tasks are created in any order, **When** TaskIDs are assigned, **Then** each subsequent TaskID is greater than all previous TaskIDs

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Users MUST be able to create a Graph instance
- **FR-002**: Users MUST be able to create a Task by providing input addresses, output addresses，input output addresses, constants, and kernel address
- **FR-003**: Users MUST be able to add Tasks to a Graph
- **FR-004**: Users MUST be able to express precedence via `A.precede(B)` method
- **FR-005**: Users MUST be able to express precedence via `B.succeed(A)` method
- **FR-006**: The Graph MUST provide topological ordering of Tasks
- **FR-007**: The Graph MUST identify independent Tasks that can execute in parallel
- **FR-008**: The Graph MUST validate that all Task references are resolvable before execution
- **FR-009**: The system MUST expose a user entry point `aicpu_orchestration_entry(const ChipStorageTaskArgs&)` that accepts task graph configuration and executes the orchestrator
- **FR-012**: The Orchestrator main thread MUST create exactly 1 Manager thread, 2 Dispatch threads, 2 Cutter threads, and 120 Executor threads to enable parallel task execution
- **FR-013**: The `aicpu_orchestration_entry` entry point MUST use `mem_pool_alloc` interface to allocate memory instead of system malloc, ensuring O(1) allocation from the pre-allocated memory pool
- **FR-014**: The `aicpu_orchestration_entry` entry point MUST use `mem_pool_when2free` interface to register memory for automatic release when the minimum uncompleted TaskID crosses the specified threshold
- **FR-015**: Task IDs MUST be assigned in monotonically increasing order, where each newly created task receives a TaskID greater than all previously assigned TaskIDs

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can construct a 100-task DAG with mixed precedence constraints in under 5 minutes
- **SC-002**: Cycle detection completes in O(V+E) time where V=tasks, E=edges
- **SC-003**: Independent task groups are correctly identified for parallel execution
- **SC-004**: Users receive clear error messages when cycle is detected, identifying the specific Tasks involved
- **SC-005**: The `aicpu_orchestration_entry` function completes execution and returns within a configurable timeout period
- **SC-006**: The orchestration launcher returns to the caller without blocking, allowing concurrent execution with the orchestration thread
- **SC-007**: The LLM entry point successfully initializes all modules and creates required threads within the startup timeout period
- **SC-008**: The Orchestrator creates exactly 125 worker threads (1 Manager, 2 Dispatch, 2 Cutter, 120 Executor) that are all running within 2 seconds of system startup
- **SC-009**: Memory allocation via mem_pool_alloc completes in under 1 microsecond per allocation
- **SC-010**: Memory registered with mem_pool_when2free is automatically released within 1 microsecond of threshold condition being met
- **SC-011**: TaskID assignment is O(1) and guarantees monotonic increment for each newly created task

---

## Assumptions

- Users have a valid kernel implementation (function pointer/address) for each Task
- Memory addresses provided are valid and accessible during Graph execution
- The Graph is intended for single-threaded construction but multi-threaded execution
- No automatic data flow inference - users explicitly specify input/output bindings
- The `ChipStorageTaskArgs` structure contains all necessary configuration for the orchestrator including task definitions, data bindings, and execution parameters
- The orchestration launcher creates exactly one dedicated thread for executing the user-provided `aicpu_orchestration_entry` function
- The caller can optionally wait for the orchestration thread to complete by synchronizing with the thread handle
- The LLM entry point serves as the central coordinator that creates threads, initializes modules, and calls module interfaces in the proper sequence
- The Orchestrator main thread creates exactly 125 worker threads: 1 Manager (memory management/when2free), 2 Dispatch (task distribution), 2 Cutter (task cutting/preprocessing), 120 Executor (task kernel execution)
- The `aicpu_orchestration_entry` entry point uses mem_pool_alloc for all memory allocation instead of system malloc
- The `aicpu_orchestration_entry` entry point uses mem_pool_when2free to register automatic memory release when task ID threshold is reached
