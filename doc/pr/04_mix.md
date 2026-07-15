# PR-D：MIX 集群调度

相对基线：`pr/C-spmd`。

---

## 宏观：本 PR 完成了什么

支持 **MIX**：同一逻辑任务占 **AIC + AIV0 + AIV1**，三路 prepare 后统一 `wmb`/门铃，三路 FIN 齐套才算 block/任务完成；未齐套延迟清槽。  
CUBE/VECTOR 路径跳过已被 MIX 占用的核。相对 simpler：对应 `PTO2ResourceShape::MIX` 与集群放置/齐套；本仓固定三路、挂在 `g_executors` bitmap 上。文档亦记录 cutter 仍可能把 MIX 折到 CUBE 出口、与 `ready_queue[MIX]` 取货不完全对齐的现状。

---

## 微观：各部分怎么做

### 1. `send_task_mix`

- **为什么修改**：OUT_PROJ 等需 AIC+AIV0+AIV1 齐套。
- **怎么修改**：空闲集群 + `ready_queue[MIX]` + claim + 三路 prepare/flush。
- **相关代码**：

```366:416:esl_proxy/src/algorithm/dispatch.c
static int send_task_mix(ctrl_t *ctrl)
{
    /* idle clusters → dequeue MIX → claim_range →
       occupy + prepare_cluster → flush */
}
```

- **对应 simpler**：`PTO2ResourceShape::MIX` + cluster place。
- **相比 simpler 的区别**：固定三路 vs `active_mask`。对照：

esl_proxy：

```396:409:esl_proxy/src/algorithm/dispatch.c
        if (!batch_dequeue(&ctrl->ready_queue[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
            break;
        }
        n = dispatch_spmd_claim_range(one, ncl - used, &start);
        /* ... */
            dispatch_mix_occupy_cluster(ctrl, core, slot, one, start + (uint32_t)b);
            if (dispatch_mix_prepare_cluster(ctrl, core, slot, one, start + (uint32_t)b, pubs, phys_arr, &np) != 0) {
```

simpler：

```60:65:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h
enum class PTO2ResourceShape : uint8_t {
    AIC = 0,
    AIV = 1,
    MIX = 2,    // Full cluster (dispatch uses active_mask)
    DUMMY = 3,
};
```

```515:516:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
        if (shape == PTO2ResourceShape::MIX) {
            // Gated MIX uses split placement ...
```

---

### 2. prepare_cluster / flush

- **为什么修改**：三路需统一可见后再门铃。
- **怎么修改**：三路 prepare；`wmb`；三路 publish。
- **相关代码**：`dispatch_mix_prepare_cluster` / `dispatch_mix_flush`（`dispatch.c` ~821+）。
- **对应 simpler**：MIX 集群放置后写门铃。
- **相比 simpler 的区别**：复用 esl `EslPublishHandle`。对照：

esl_proxy：

```text
dispatch_mix_prepare_cluster(... pubs[])
dispatch_mix_flush: wmb(); for i: esl_publish_subtask_to_core(pubs[i])
```

simpler：

```136:138:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_context.h
    // buf_idx = reg_task_id & 1
    PTO2DispatchPayload payload_per_core_[RUNTIME_MAX_WORKER][2];
```

---

### 3. harvest / defer clear

- **为什么修改**：三路未齐清槽会覆盖 COND。
- **怎么修改**：`cluster_all_done` 才 `note_block_done`；`mix_defer_slot_clear`。
- **相关代码**：`dispatch.c` MIX harvest。
- **对应 simpler**：MIX completion + `pending_occupied`。
- **相比 simpler 的区别**：bitmap vs BitStates。对照：

esl_proxy：

```text
dispatch_mix_cluster_all_done → note_block_done → completed_queue
mix_defer_slot_clear while g_mix_active
```

simpler：

```461:466:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
            tracker.clear_pending_occupied(bit_pos);  // Idle safeguard
            // Case 4 (running ACK) or Case 2 (pending ACK): clear pending_occupied only
            tracker.clear_pending_occupied(bit_pos);
```

---

### 4. 跳过 `mix_core_busy`

- **为什么修改**：MIX 占核后不能再发单核任务。
- **怎么修改**：`send_task`/`prefetch` 过滤 busy 核。
- **相关代码**：`dispatch.c` 中 `dispatch_mix_core_busy`。
- **对应 simpler**：CoreTracker 空闲集合。
- **相比 simpler 的区别**：显式过滤 `free_bitmap`。对照：

esl_proxy：

```435:439:esl_proxy/src/algorithm/dispatch.c
#if !ESL_DISPATCH_DOUBLE_BUFFER
    for (int core = 0; core < AIC_CNT; core++) {
        if (dispatch_mix_core_busy(core)) {
            free_bitmap &= ~((uint64_t)1 << core);
```

simpler：

```644:644:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
        auto idle = (shape == PTO2ResourceShape::MIX) ? tracker.get_mix_running_cluster_offset_states(core_mask) :
```

---

### 5. cutter MIX→CUBE（与 dispatch 不完全对齐）

- **为什么修改**：记录 A 遗留折线与 `send_task_mix` 取 `ready_queue[MIX]` 的不一致。
- **怎么修改**：现状仍映射到 CUBE 下标。
- **相关代码**：

```32:38:esl_proxy/src/algorithm/cutter.c
static inline int ready_queue_index(task_type_t type)
{
    if (type == TASK_TYPE_MIX) {
        return TASK_TYPE_CUBE;
    }
    return (int)type;
}
```

- **对应 simpler**：独立 `ready_queues[MIX]`。
- **相比 simpler 的区别**：出口下标不一致。对照：

esl_proxy：

```32:38:esl_proxy/src/algorithm/cutter.c
static inline int ready_queue_index(task_type_t type)
{
    if (type == TASK_TYPE_MIX) {
        return TASK_TYPE_CUBE;
    }
    return (int)type;
}
```

```396:396:esl_proxy/src/algorithm/dispatch.c
        if (!batch_dequeue(&ctrl->ready_queue[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
```

simpler：

```60:65:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h
enum class PTO2ResourceShape : uint8_t {
    AIC = 0, AIV = 1, MIX = 2, DUMMY = 3,
};
```

```180:190:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/shared/pto_runtime2_init.cpp
                &sched->ready_queues[i], arena, layout.off_ready_queue_slots[i], layout.ready_queue_capacity
```
