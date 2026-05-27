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

### Buffer Handle

Reference to allocated buffer returned to caller.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Buffer address within pool |
| `size` | `size_t` | Buffer size |

### when2free Entry

Entry in the when2free FIFO queue.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Buffer address to free |
| `task_id` | `uint32_t` | Release threshold (free when min_uncompleted > task_id) |

### when2free FIFO Queue

SPSC queue for when2free entries.

| Field | Type | Description |
|-------|------|-------------|
| `entries` | `when2free_entry_t*` | Array of when2free entries |
| `capacity` | `size_t` | Queue capacity |
| `_Atomic head` | `size_t` | Consumer head pointer |
| `_Atomic tail` | `size_t` | Producer tail pointer |

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
├── when2free FIFO Queue
│   ├── head: Manager reads entries here
│   └── tail: Orchestrator writes entries here
└── min_uncompleted_tracker
    └── queries Task State Ring Buffer for state
```

## Validation Rules

1. Allocations are variable-sized (no fixed slots)
2. `tail` advances by allocation size on alloc
3. `head` advances by allocation size on free (FIFO order)
4. when2free release when `min_uncompleted > entry.task_id`
5. SPSC: only Orchestrator (producer) advances `tail`, only Manager (consumer) advances `head`

## State Transitions

### Memory Pool (Continuous)
```
[FREE] --[tail += size]--> [ALLOCATED]
[ALLOCATED] --[head += size]--> [FREE] (FIFO order)
```

### when2free FIFO Queue
```
EMPTY --[when2free]--> [ENTRY Added] --[Manager dequeues]--> [ENTRY Removed]
```

### TaskID Tracker
```
Running --> min_uncompleted updates when task state changes to COMPLETED
```