# Data Model: Memory Pool Manager

**Feature**: 007-memory-pool
**Date**: 2026-05-27

## Entities

### mem_pool_t

Memory pool with FIFO-based allocation and when2free support.

| Field | Type | Description |
|-------|------|-------------|
| `base` | `void* restrict` | Base address of pre-allocated memory region |
| `size` | `size_t` | Total size of the memory pool in bytes |
| `head` | `_Atomic uintptr_t` | Atomic head pointer (consumer side — for deallocation) |
| `tail` | `_Atomic uintptr_t` | Atomic tail pointer (producer side — for allocation) |
| `fifo` | `when2free_fifo_t*` | FIFO queue for when2free entries |

**Validation Rules**: None (Trust the Caller — inputs assumed valid)

### when2free_fifo_t

FIFO queue for when2free entries.

| Field | Type | Description |
|-------|------|-------------|
| `entries` | `when2free_entry_t*` | Array of entries |
| `capacity` | `size_t` | FIFO capacity |
| `head` | `_Atomic size_t` | Consumer index |
| `tail` | `_Atomic size_t` | Producer index |

### when2free_entry_t

Entry in the when2free FIFO queue.

| Field | Type | Description |
|-------|------|-------------|
| `addr` | `void*` | Memory address to be released |
| `taskid` | `uint32_t` | Threshold task ID for automatic release |

## Constants

| Name | Value | Description |
|------|-------|-------------|
| `MEM_POOL_ALIGN` | `8` | Alignment requirement for allocations |
| `SENTINEL_DONE` | `UINT32_MAX` | Sentinel indicating no uncompleted tasks |

## State Transitions

```
ALLOCATED → RELEASED (head advances via when2free processing)
```

## Dependencies

- `task.h`: task_id_t, task_state_t, TASK_STATE_COMPLETED
- `ring_buf.h`: ring_min_uncompleted(), g_state_buf[]