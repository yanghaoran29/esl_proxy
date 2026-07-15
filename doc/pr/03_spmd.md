# PR-C：SPMD range-claim

相对基线：`pr/B-sim`。

---

## 宏观：本 PR 完成了什么

让 **多 block** 任务可调度：按空闲核 **claim 连续 block 范围**，失败 rewind，按 block 计完成，**末 block 完成**才进 `completed_queue`；任务再次 ready 时重置游标。  
打在既有 `ready_queue` + prepare/publish 路径上，不引入 MIX/DB。相对 simpler：对应 `next_block_idx` / `claim_block_range` / `on_subtask_complete`，本仓用单 dispatch 线程串行游标而非 CAS 全家桶。

---

## 微观：各部分怎么做

### 1. ready 时重置 SPMD 游标

- **为什么修改**：二次 ready 必须清零游标与完成计数。
- **怎么修改**：`dispatch_spmd_on_ready`；cutter 调用。
- **相关代码**：

```1144:1150:esl_proxy/src/algorithm/dispatch.c
__attribute__((weak)) void dispatch_spmd_on_ready(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    g_next_block[slot] = 0;
    g_finished_blocks[slot] = 0;
}
```

- **对应 simpler**：`next_block_idx.store(0)` / slot reset。
- **相比 simpler 的区别**：weak 钩子 vs slot 内 atomic store。对照：

esl_proxy：

```1144:1150:esl_proxy/src/algorithm/dispatch.c
__attribute__((weak)) void dispatch_spmd_on_ready(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    g_next_block[slot] = 0;
    g_finished_blocks[slot] = 0;
}
```

simpler：

```575:575:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2_types.h
        next_block_idx.store(0, std::memory_order_relaxed);
```

---

### 2. claim 连续 range + rewind

- **为什么修改**：多 block 需按空闲核认领连续范围；失败要 rewind。
- **怎么修改**：`dispatch_spmd_claim_range` / `dispatch_spmd_rewind`；`send_task` 内使用。
- **相关代码**：

```1153:1188:esl_proxy/src/algorithm/dispatch.c
static int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block)
{
    /* ... */
    g_next_block[slot] = (uint16_t)(start + n);
    return (int)n;
}
static int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block)
{
    g_next_block[task_id & RING_MASK] = (uint16_t)next_block;
    return 1;
}
```

- **对应 simpler**：`claim_block_range`（CAS）。
- **相比 simpler 的区别**：串行更新 vs `compare_exchange_weak`。对照：

esl_proxy：

```1161:1177:esl_proxy/src/algorithm/dispatch.c
    uint16_t cur = g_next_block[slot];
    start = cur;
    /* ... */
    g_next_block[slot] = (uint16_t)(start + n);
```

simpler：

```497:514:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_runtime2_types.h
    std::atomic<int16_t> next_block_idx{0};

    int32_t claim_block_range(int32_t block_limit, int32_t max_count, int32_t &start) {
        int16_t current = next_block_idx.load(std::memory_order_relaxed);
        while (current < block_limit && max_count > 0) {
            int32_t count = block_limit - current;
            if (count > max_count) count = max_count;
            int16_t desired = static_cast<int16_t>(current + count);
            if (next_block_idx.compare_exchange_weak(
                    current, desired, std::memory_order_seq_cst, std::memory_order_relaxed
                )) {
                start = current;
                return count;
            }
        }
        start = current;
        return 0;
    }
```

---

### 3. `note_block_done` 按 block 完成

- **为什么修改**：未齐套不能进 `completed_queue`。
- **怎么修改**：`g_finished_blocks` 原子加一；等于 `count` 才完成。
- **相关代码**：

```1213:1221:esl_proxy/src/algorithm/dispatch.c
static int dispatch_spmd_note_block_done(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;
    uint16_t prev = atomic_fetch_add_explicit(&g_finished_blocks[slot], 1, memory_order_acq_rel);
    return (uint32_t)(prev + 1U) == total;
}
```

- **对应 simpler**：`on_subtask_complete` / `completed_subtasks`。
- **相比 simpler 的区别**：block 计数 vs 更完整的 subtask/mask。对照：

esl_proxy：

```1213:1221:esl_proxy/src/algorithm/dispatch.c
static int dispatch_spmd_note_block_done(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;
    uint16_t prev = atomic_fetch_add_explicit(&g_finished_blocks[slot], 1, memory_order_acq_rel);
    return (uint32_t)(prev + 1U) == total;
}
```

simpler：

```155:155:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
    bool task_complete = sched_->on_subtask_complete(slot_state);
```
