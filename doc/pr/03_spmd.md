# PR-C：SPMD range-claim

相对基线：`pr/B-sim`。

---

## 1. 为什么修改

PR-A/B 的 basic dispatch 固定 `block_idx=0`，只能跑单 block（或等价单次下发）图。  
多 block 任务需要：一次 claim 连续 block 范围、失败 rewind、按 block 完成计数，末 block 完成才进 `completed_queue`。

本 PR 把 SPMD range-claim 接到 `ready_queue` + prepare/publish 路径上。

---

## 2. 怎么修改

主要改 `src/algorithm/dispatch.c`（及必要头文件/钩子）：

| 点 | 做法 |
|----|------|
| 游标 / 完成计数 | 增加 `g_next_block` / `g_finished_blocks`（或等价 per-task 状态） |
| `send_task` | 按空闲核数 `claim` 连续 block；失败 rewind；有剩余 push 回 `ready_queue` |
| 完成路径 | `note_block_done`：仅当末 block 完成才入 `completed_queue` |
| ready 钩子 | `dispatch_spmd_on_ready`：重置游标与完成计数 |

---

## 3. 与 simpler 的对应

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| `g_next_block` / claim 连续 range | `PTO2TaskSlotState::next_block_idx` / `claim_block_range`（`pto_runtime2_types.h`） |
| `g_finished_blocks` / `note_block_done` | `completed_subtasks` + `on_subtask_complete`（`scheduler_completion.cpp`） |
| `dispatch_spmd_on_ready` 重置游标 | task 再次 ready / `push_ready_routed` 前的 slot 状态复位 |
| `send_task` 内 range-claim + rewind | `scheduler_dispatch.cpp` / `pto_scheduler.h` 的 SPMD 下发 |
