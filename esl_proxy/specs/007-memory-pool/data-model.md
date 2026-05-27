# Data Model: Memory Pool

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Entities

### Memory Pool

Pre-allocated continuous memory region managed via ring buffer head/tail pointers.

| Field | Type | Description |
|-------|------|-------------|
| `base_addr` | `void*` | Base address of pre-allocated continuous memory block |
| `total_size` | `size_t` | Total pool size in bytes |
| `_Atomic tail` | `size_t` | Producer offset pointer for allocation (SPSC) |
| `_Atomic head` | `size_t` | Consumer offset pointer for release (SPSC) |

**State**: N/A (static allocation via ring buffer)

### Allocation Record

Tracks individual allocation within the continuous pool.

| Field | Type | Description |
|-------|------|-------------|
| `offset` | `size_t` | Offset from base_addr where allocation starts |
| `size` | `size_t` | Size of this allocation in bytes |
| `in_use` | `_Atomic bool` | Whether this allocation is active |
| `when2free_task_id` | `uint32_t` | Threshold for when2free release (0 = none) |

### Buffer Handle

Reference to allocated buffer returned to caller.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Buffer address within pool (base_addr + offset) |
| `size` | `size_t` | Buffer size |
| `offset` | `size_t` | Offset from pool base for release |

### when2free Registry

Registration entry for automatic memory release.

| Field | Type | Description |
|-------|------|-------------|
| `offset` | `size_t` | Offset of registered allocation |
| `task_id` | `uint32_t` | Release threshold (free when min_uncompleted > task_id) |
| `active` | `bool` | Whether registration is active |

### Minimum Uncompleted Tracker

Tracks minimum uncompleted TaskID via Task State Ring Buffer.

| Field | Type | Description |
|-------|------|-------------|
| `min_uncompleted` | `_Atomic uint32_t` | Current minimum uncompleted TaskID |
| `sentinel` | `uint32_t` | Value indicating no uncompleted tasks (e.g., UINT32_MAX) |

## Relationships

```
Memory Pool (Continuous Ring Buffer)
├── base_addr + tail (alloc offset)
├── base_addr + head (free offset)
├── allocation_records[] (variable-sized allocations tracked by offset)
│   └── each allocation: size + in_use state + when2free_task_id
├── when2free_registry[]
│   └── each entry: offset + task_id threshold
└── min_uncompleted_tracker
    └── queries Task State Ring Buffer for state
```

## Validation Rules

1. Allocations are variable-sized (no fixed slots)
2. `tail` advances by allocation size on alloc
3. `head` advances by allocation size on free (FIFO order)
4. when2free release only when `min_uncompleted > allocation.when2free_task_id`
5. SPSC: only Orchestrator (producer) advances `tail`, only Worker/Manager (consumer) advances `head`

## State Transitions

### Allocation State (Continuous)
```
FREE --[tail += size]--> ALLOCATED
ALLOCATED --[head += size]--> FREE (FIFO order)
```

### Ring Buffer Pointer Advance
```
tail: tail + allocation_size (with wrap at total_size)
head: head + allocation_size (with wrap at total_size)
```

### when2free Entry
```
ACTIVE --[min_uncompleted > task_id]--> FREED
ACTIVE --[explicit free]--> EXPLICIT_FREE
```

### TaskID Tracker
```
Running --> min_uncompleted updates when task state changes to COMPLETED
```