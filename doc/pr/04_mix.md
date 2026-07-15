# PR-D：MIX 集群调度

相对基线：`pr/C-spmd`。

---

## 1. 为什么修改

部分 case（如 qwen3 OUT_PROJ）需要 **MIX**：同一逻辑任务占 **AIC + AIV0 + AIV1** 三路，齐套后才算 subtask/block 完成。  
PR-C 只有 CUBE/VECTOR 单核下发，无法表达集群占用与三路 harvest。

本 PR 加入 MIX occupy / prepare / flush / harvest。

---

## 2. 怎么修改

主要改 `src/algorithm/dispatch.c`：

| 点 | 做法 |
|----|------|
| 下发 | `send_task_mix`：占集群三路，prepare 后统一可见再门铃 |
| 空闲过滤 | `send_task` 跳过 `mix_core_busy` 的核 |
| 预取（集群第二 slot） | `dispatch_mix_prefetch` |
| 完成 | harvest：`cluster_all_done` → `note_block_done` |
| 清槽 | mark/force 使用 `mix_defer_slot_clear`，避免 COND 被过早覆盖 |

---

## 3. 与 simpler 的对应

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| `TASK_TYPE_MIX` / `send_task_mix` | `PTO2ResourceShape::MIX`；`scheduler_dispatch.cpp` 集群放置 |
| 集群三路占用 | `CoreTracker::classify_mix_cluster` / `get_mix_running_cluster_offset_states`（`scheduler_types.h`） |
| `cluster_all_done` harvest | `scheduler_completion.cpp` 对 MIX cluster 的齐套完成 |
| `mix_defer_slot_clear` | simpler 在三路未齐时推迟释放/清槽，避免完成寄存器被覆盖 |
