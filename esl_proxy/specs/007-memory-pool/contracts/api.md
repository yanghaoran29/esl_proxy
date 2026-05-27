# Memory Pool Contracts

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Public API Contracts

### mem_pool_init

```c
int mem_pool_init(mem_pool_config_t *config);
```

**Contract**:
- Pre-allocate `config->pool_size` bytes using standard allocator
- Initialize ring buffer: head=0, tail=0
- Initialize when2free FIFO queue
- Return 0 on success, -1 on allocation failure
- Initial state: all memory FREE, when2free FIFO empty

**Caller guarantees**: `config` is non-NULL, `config->pool_size > 0`

---

### mem_pool_alloc

```c
buffer_handle_t* mem_pool_alloc(size_t size);
```

**Contract**:
- Check if `(tail - head + size) <= total_size` (available memory)
- Allocate contiguous memory starting at `base_addr + tail`
- Advance `tail += size` (atomic)
- Return buffer handle with addr and size on success, NULL if pool full

**Caller guarantees**: Called from Orchestrator (producer role) only; SPSC mode

**Performance**: < 1μs under normal conditions

---

### mem_pool_free

```c
int mem_pool_free(void *addr);
```

**Contract**:
- Mark memory region as FREE
- Advance `head += size` to match the allocation size
- Return 0 on success, -1 if address not found

**Caller guarantees**: Called from Manager (consumer role) only; addr was returned by mem_pool_alloc

**Performance**: < 1μs under normal conditions

---

### mem_pool_when2free

```c
int mem_pool_when2free(void *addr, uint32_t task_id);
```

**Contract**:
- Enqueue when2free entry (addr, task_id) to FIFO queue
- Entry will be processed when min_uncompleted > task_id
- Return 0 on success, -1 if FIFO queue full

**Caller guarantees**: Called from Orchestrator only; addr was returned by mem_pool_alloc

---

### mem_pool_process_when2free

```c
void mem_pool_process_when2free(uint32_t min_uncompleted);
```

**Contract**:
- Dequeue entries from when2free FIFO queue
- For each entry where `min_uncompleted > entry.task_id`:
  - Free the buffer memory
  - Advance head pointer
- Stop when entry with `min_uncompleted <= task_id` is found (FIFO order)

**Called by**: Manager thread (typically in polling loop)

**Performance**: < 1μs for typical queue sizes (< 1000 entries)

---

## Continuous Ring Buffer Design

### Memory State Machine

```
[FREE] --[tail += size]--> [ALLOCATED]
[ALLOCATED] --[head += size]--> [FREE]
```

### Pool Full Detection
```
(tail - head) >= total_size → Pool exhausted
```

### Pointer Wraparound
```
tail = tail % total_size
head = head % total_size
```

## when2free FIFO Queue Design

### Entry Structure
```c
typedef struct {
    void *addr;
    uint32_t task_id;
} when2free_entry_t;
```

### SPSC Access Control

| Role | Producer (Orchestrator) | Consumer (Manager) |
|------|------------------------|-------------------|
| mem_pool_alloc | MUST | MUST NOT |
| mem_pool_free | MUST NOT | MUST |
| mem_pool_when2free | MUST | MUST NOT |
| mem_pool_process_when2free | MUST NOT | MUST |

### FIFO Queue State Machine

```
EMPTY --[enqueue]--> [HAS_ENTRIES] --[dequeue all eligible]--> [EMPTY or HAS_ENTRIES]
```

## Error Handling

Per Constitution Principle X (Trust the Caller):
- Invalid input → undefined behavior (caller guarantees validity)
- Allocation failure → return NULL (not pool panic)
- Double-free → undefined behavior (caller guarantees single free)
- Wrong SPSC role → undefined behavior (caller guarantees correct role)
- when2free FIFO full → return -1 (caller should handle)