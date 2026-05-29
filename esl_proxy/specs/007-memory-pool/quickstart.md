# Quickstart: Memory Pool

**Feature**: 007-memory-pool
**Date**: 2026-05-27

## Basic Usage

### 1. Include Header

```c
#include <dag/mem_pool.h>
```

### 2. Initialize Pool

```c
mem_pool_t pool;
void *memory = /* pre-allocated buffer */;
mem_pool_init(&pool, memory, 4096);
```

### 3. Allocate Memory

```c
void *buf = mem_pool_alloc(&pool, 128);
// Use buf for task data
```

### 4. Register when2free

```c
mem_pool_when2free(&pool, buf, 10);
// Buffer will be released when min_uncompleted >= 10
```

### 5. Start Manager Thread

```c
pthread_t mgr;
pthread_create(&mgr, NULL, mem_pool_manager, &pool);
// Manager processes when2free queue based on ring buffer
```

## Query Minimum Uncompleted TaskID

```c
uint32_t min_id = mem_pool_min_uncompleted();
// Returns minimum ID with state != COMPLETED, or UINT32_MAX if all done
```

## Process when2free Entries (Internal)

```c
mem_pool_process_when2free(&pool);
// Called by manager thread to update FIFO head
```

## Constants

- `MEM_POOL_ALIGN` — alignment requirement (8 bytes)
- `SENTINEL_DONE` — UINT32_MAX, indicates no uncompleted tasks

## Trust the Caller

No validation is performed on any inputs. Passing invalid pointers or sizes results in undefined behavior. The caller is responsible for ensuring all inputs are valid.