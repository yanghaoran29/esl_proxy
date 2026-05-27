# Quickstart: Memory Pool Integration

**Feature**: `specs/007-memory-pool/spec.md`
**Created**: 2026-05-27

## Basic Usage

### 1. Initialize Memory Pool

```c
// Configure pool at system init
mem_pool_config_t config = {
    .pool_size = 64 * 1024 * 1024,  // 64MB
};
mem_pool_init(&config);
```

### 2. Orchestrator Allocates Buffer (Producer)

```c
// Orchestrator allocates buffer via ring buffer tail
buffer_handle_t *buf = mem_pool_alloc(1024);
if (!buf) {
    // Handle pool exhaustion
}

// Register for automatic release via when2free FIFO
mem_pool_when2free(buf->addr, target_task_id);
```

### 3. Manager Thread Processes when2free (Consumer)

```c
// Manager thread runs continuously
while (running) {
    uint32_t min_id = task_state_ring_buffer_min_uncompleted();
    mem_pool_process_when2free(min_id);
    // Poll interval: typically 1-10μs
}
```

## Continuous Ring Buffer Flow

```
Producer (Orchestrator)              Consumer (Manager)
      |                                  |
      |-- tail += size -->              |
      |                                  |
   tail = 100                          head = 0
      |                                  |
      |-- alloc(100) --> buffer[0-99]  |
      |                                  |
   tail = 200                          head = 100
      |                                  |
      |-- tail += 50 -->                |
      |                                  |
   tail = 250                          head = 100
      |                                  |
      |-- alloc(50) --> buffer[100-149] |
```

## when2free FIFO Queue Flow

```
Orchestrator                 when2free FIFO              Manager Thread
      |                            |                           |
      |-- when2free(A, 5) -->      |                           |
      |                            |                           |
      |                            | --> dequeues A            |
      |                            |                           min_id = 6 > 5?
      |                            |                           Yes: free(A)
      |                            |                           |
      |-- when2free(B, 8) -->      |                           |
      |                            |                           |
      |                            | --> dequeues B            |
      |                            |                           min_id = 6 < 8?
      |                            |                           No: re-enqueue or keep
```

## Test Scenarios

### Scenario 1: Basic Alloc/Free

```
1. Init pool (64MB)
2. tail = 0, head = 0
3. alloc(1024) → returns buffer, tail = 1024
4. free(buffer) → head = 1024
5. Verify ring buffer state: head == tail (balanced)
```

### Scenario 2: when2free FIFO Processing

```
1. Init pool + when2free FIFO queue
2. Orchestrator: when2free(buffer_A, taskID=5)
3. Orchestrator: when2free(buffer_B, taskID=8)
4. Manager: min_uncompleted = 6 > 5? → free buffer_A
5. Manager: min_uncompleted = 6 < 8? → keep buffer_B in FIFO
6. Manager: min_uncompleted = 9 > 8? → free buffer_B
```

### Scenario 3: Continuous Memory (Variable-Sized)

```
1. Init pool (100KB)
2. alloc(30KB) → tail = 30KB
3. alloc(50KB) → tail = 80KB
4. free(30KB) → head = 30KB
5. alloc(20KB) → tail = 100KB
6. Verify: No fixed slots, memory used contiguously
```

### Scenario 4: FIFO Queue Wraparound

```
1. when2free FIFO capacity = 1024
2. when2free(A, 1), when2free(B, 2), ..., when2free(X, 24)
3. Manager processes entries 1-24
4. dequeues in FIFO order: A first, then B, etc.
```

### Scenario 5: Pool Exhaustion

```
1. Init pool (1KB)
2. alloc(500B) → tail = 500
3. alloc(500B) → tail = 1000
4. alloc(500B) → tail wraps, but head still 0 → Pool full?
5. Actually: (tail - head) >= pool_size? → return NULL
```