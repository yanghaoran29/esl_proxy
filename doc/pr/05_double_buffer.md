# PR-E：double_buffer

相对基线：`pr/D-mix`。

---

## 1. 为什么修改

basic 路径每核单槽：核在跑时无法提前 prepare 下一发，流水线气泡大。  
真机调度用 **per-core 双 payload 槽**（`task_id & 1`）做 ping-pong：busy 槽 ACK 后可 prefetch 第二槽。

本 PR 打开 `dispatch.c` 中 `#if ESL_DISPATCH_DOUBLE_BUFFER` 路径与 CUBE/VECTOR `dispatch_prefetch`，并用 Makefile / onboard 开关启用。

---

## 2. 怎么修改

| 点 | 做法 |
|----|------|
| 代码 | `send_task` / `dispatch_prefetch` / `dispatch` 增加双槽逻辑（`ESL_DISPATCH_DOUBLE_BUFFER`） |
| 主机 Makefile | `make DISPATCH=double_buffer` → `-DESL_DISPATCH_DOUBLE_BUFFER=1` |
| onboard | `ESL_PROXY_DOUBLE_BUFFER=ON` / `run_onboard.sh --double-buffer` |

prefetch：busy slot 已 ACK 后写第二槽并 publish。

---

## 3. 与 simpler 的对应

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| per-core 双槽 `reg_task_id & 1` | `payload_per_core_[core][buf_idx]`，`buf_idx = reg_task_id & 1`（`scheduler_dispatch.cpp`） |
| AICore 读 `payload + (task_id & 1)` | `aicore_executor.cpp`：`payload + (task_id & 1u)` |
| `dispatch_prefetch` / 第二槽提前下发 | `pending_occupied_` / early-dispatch 第二 slot |
| `DISPATCH=double_buffer` | simpler 默认双槽能力；esl_proxy 用编译开关打开 |
