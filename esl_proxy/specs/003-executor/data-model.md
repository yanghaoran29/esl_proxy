# Data Model: Async Task Executor

## Entity Definitions

### Task State (`task_state_t`)

| State | Value | Description |
|-------|-------|-------------|
| TASK_PENDING | 0 | Task is cached, waiting for execution |
| TASK_EXECUTING | 1 | Task is currently being executed |
| TASK_COMPLETED | 2 | Task completed successfully |
| TASK_ERROR | 3 | Task execution failed |

### Task Descriptor (`task_t`)

| Field | Type | Description |
|-------|------|-------------|
| id | uint32_t | Unique task identifier (monotonically assigned) |
| state | task_state_t | Current execution state |
| input | void* | Input data pointer |
| output | void* | Output data pointer |
| constant | void* | Constant data pointer |
| kernel | void (*)(void*) | Kernel function pointer |
| duration_ms | uint32_t | Delay duration in milliseconds |
| subtask_cnt | uint32_t | Sub-task count |

### Executor Type (`executor_t`)

| Field | Type | Description |
|-------|------|-------------|
| slots | task_t[2] | Two task cache slots |
| slot_state | atomic int[2] | Slot occupancy (0=empty, 1=occupied) |
| ping_pong | atomic int | Current slot selector (0 or 1) |
| worker | pthread_t | Worker thread handle |
| running | atomic bool | Executor running state |

## State Transitions

### Task State Machine

```
TASK_PENDING → TASK_EXECUTING (worker picks up task)
TASK_EXECUTING → TASK_COMPLETED (delay elapsed, kernel returned)
TASK_EXECUTING → TASK_ERROR (kernel returned error)
```

### Executor State Machine

```
INIT → RUNNING (executor_init)
RUNNING → STOPPED (executor_shutdown)
```

## Validation Rules

1. Task ID must be monotonically increasing per Executor instance
2. Duration must be > 0 for meaningful delay
3. Kernel must be non-NULL for task execution
4. Slots can only hold one task each

## Relationships

- Executor contains 2 task slots (composition)
- Task references Kernel function (delegation)
- Task state transitions managed by Executor worker thread