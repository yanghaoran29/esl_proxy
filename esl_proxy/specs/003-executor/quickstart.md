# Quickstart: Async Task Executor

## Basic Usage

### 1. Include Header

```c
#include "dag/executor.h"
```

### 2. Define a Kernel Function

```c
void my_kernel(void *arg) {
    // Task execution logic
    int *input = (int *)arg;
    // ... process input
}
```

### 3. Create and Initialize Executor

```c
executor_t exec;
executor_init(&exec);
```

### 4. Submit Tasks

```c
task_t task = {
    .id = 0,
    .kernel = my_kernel,
    .duration_ms = 100,
    .input = &my_data
};
executor_submit(&exec, &task);
```

### 5. Check Task Status

```c
if (task.state == TASK_COMPLETED) {
    // Task finished
}
```

### 6. Shutdown Executor

```c
executor_shutdown(&exec);
```

## PING PONG Strategy

The Executor uses a PING PONG strategy to select from its 2 slots:

1. When both slots are occupied, Executor alternates: Slot 0 → Slot 1 → Slot 0 → ...
2. This ensures fair task selection and prevents starvation
3. If only one slot is occupied, that slot is selected regardless of PING PONG state

## Async Execution Flow

```
Submit Task
    ↓
[Slot 0 or Slot 1 empty?]
    ↓ yes
Cache task in available slot
    ↓
Worker picks task (PING PONG selection)
    ↓
Set TASK_EXECUTING
    ↓
Wait for duration_ms
    ↓
Execute kernel
    ↓
Set TASK_COMPLETED or TASK_ERROR
```

## Thread Safety

- Executor uses C11 atomics for slot state and PING PONG selector
- No mutexes in hot path (per Constitution)
- Caller must ensure proper synchronization for shared task data