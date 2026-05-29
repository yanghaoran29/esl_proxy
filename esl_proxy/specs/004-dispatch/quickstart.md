# Quickstart: Dispatch

## Basic Usage

### 1. Include Header

```c
#include "dag/dispatch.h"
```

### 2. Initialize Dispatch

```c
// Shared memory regions from Orchestrator and Cutter
shm_buf_t shm_orch = { .addr = orch_base, .size = orch_size };
shm_buf_t shm_cutter = { .addr = cutter_base, .size = cutter_size };

dispatch_t disp;
dispatch_init(&disp, &shm_orch, &shm_cutter);
```

### 3. Acquire Tasks

```c
// Read from Orchestrator and Cutter shared memory
while ((task = dispatch_acquire_task(&disp)) != NULL) {
    // Task acquired from dual sources
}
```

### 4. Distribute Tasks

```c
// Distribute to Executors via shared memory
dispatch_distribute(&disp, task_id, executor_idx);

// Check if MIX task needs dual-capable executor
if (task_type == TASK_MIX) {
    idx = dispatch_find_idle_mix(&disp);
}
```

### 5. Check Completion

```c
// Poll completion bits
if (completion_bit_read(&disp.completion_bits, exec_idx)) {
    dispatch_free_executor(&disp, exec_idx);
    // Steal work if needed
    dispatch_steal_work(&disp);
}
```

### 6. Shutdown

```c
dispatch_shutdown(&disp);
```

## Work-Stealing

When a Dispatch has no work:

```c
// Actively steal from busiest dispatch
dispatch_steal_from(&target_dispatch, &disp);
```

Steal policy:
1. Find Dispatch with deepest queue
2. Steal half of its tasks
3. Respect dependency constraints (predecessors must be satisfied)

## Shared Memory Protocol

### Dispatch → Executor

| Field | Size | Description |
|-------|------|-------------|
| task_id | uint32_t | Task identifier |
| index | uint32_t | Slot index (A or B) |
| type | uint8_t | TASK_CUBE/VECTOR/MIX |

### Executor → Dispatch

| Field | Size | Description |
|-------|------|-------------|
| completion | 1 bit | Slot A task completed (0/1) |
| slot_idx | 1 bit | Which slot completed (A=0, B=1) |

## Thread Safety

- Shared memory uses lock-free SPSC queues (atomic tail/head)
- Completion bits use atomic read/write
- No mutexes in hot path (per Constitution)
- Trust the Caller for shared memory addresses