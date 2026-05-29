# Data Model: Dispatch

## Entity Definitions

### Shared Memory Buffer Descriptor (`shm_buf_t`)

| Field | Type | Description |
|-------|------|-------------|
| addr | void* | Shared memory base address |
| size | size_t | Buffer size in bytes |
| tail | _Atomic size_t | Producer tail position (SPSC) |
| head | _Atomic size_t | Consumer head position (SPSC) |

### Dispatch Type (`dispatch_t`)

| Field | Type | Description |
|-------|------|-------------|
| id | uint32_t | Unique Dispatch instance ID |
| executor_pool | executor_t[120] | 120 Executors per Dispatch |
| cube_count | int | Number of CUBE-capable Executors (60) |
| vector_count | int | Number of VECTOR-capable Executors (60) |
| shm_from_orch | shm_buf_t | Shared memory from Orchestrator |
| shm_from_cutter | shm_buf_t | Shared memory from Cutter |
| completion_bits | _Atomic uint64_t | Per-Executor 1-bit completion signals |

### Executor Capability (`exec_capability_t`)

| Value | Description |
|-------|-------------|
| EXEC_CUBE | CUBE-only capable |
| EXEC_VECTOR | VECTOR-only capable |
| EXEC_MIX | Both CUBE and VECTOR capable |

### Task Type (`task_type_t`)

| Value | Description |
|-------|-------------|
| TASK_CUBE | CUBE computation task |
| TASK_VECTOR | VECTOR computation task |
| TASK_MIX | Combined CUBE+VECTOR task |

## Relationships

- Dispatch contains 120 Executors (composition)
- Dispatch reads from 2 shared memory sources (Orchestrator, Cutter)
- Dispatch writes taskID+index to shared memory for Executor
- Executor writes 1-bit completion signal to shared memory

## State Transitions

### Task Dispatch State

```
PENDING → DISPATCHED (Dispatch writes to shared memory)
DISPATCHED → EXECUTING (Executor reads task)
EXECUTING → COMPLETED (Executor writes 1-bit completion)
```

### Dispatch Lifecycle

```
INIT → RUNNING (dispatch_init)
RUNNING → STOPPED (dispatch_shutdown)
```

## Validation Rules

1. Dispatch ID must be unique across all Dispatch instances
2. Executor pool size is fixed at 120 per Dispatch
3. Completion bits use exactly 1 bit per Executor
4. Shared memory regions must not overlap