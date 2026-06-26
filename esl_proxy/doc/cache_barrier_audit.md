# cache / barrier / lock 清理审计

对照 [`simpler/docs/hardware/cache-coherency.md`](../../../simpler/docs/hardware/cache-coherency.md) 与 onboard3 清理计划（v5/v6）。

## 原则

1. **simpler 不用，proxy 不用**（语义等价 + 回归验证）。
2. **algorithm 层零 `#ifdef ESL_PROXY_ONBOARD`** — cache / handshake bootstrap 由 `platform.h` 声明，sim/onboard 链接不同实现。
3. **QuteMiao 死代码不得删** — 见 [`onboard2_overview.md` §3.11](onboard2_overview.md)。

## QuteMiao 保护清单（未动）

| 项 | 位置 |
|----|------|
| `g_lock_buf` + `lock()`/`unlock()` | `ring_buf.h`, `shm.c` |
| `mpmc_queue.h` stub | `mpmc_queue.h` |
| `executor_worker` | `executor.c` |
| `queue_t.lock` + spin 路径 | `queue.h` |
| TODO / 注释死代码 | `dispatch.c`, `cutter.c`, `shm.c`, `log.c` |

## 已执行变更（Phase 1–4）

### Phase 1 — cache/barrier 裁剪（v5）

| 变更 | 文件 |
|------|------|
| 删 post-flush `OUT_OF_ORDER_STORE_BARRIER` | `ring_buf.h`, `cutter.c`（per-edge） |
| 删 dispatch 同线程 `free_bitmap`/`msg_bitmap` flush | `dispatch.c` |
| handshake 等待环去掉 civac | `handshake.c` |
| `add_predecessors` 仅保留 `g_predecessors` 末尾 flush（tensormap 跨核读前必需） | `ring_buf.h` |

### Phase 2 — Orch + queue（v5）

| 变更 | 文件 |
|------|------|
| Orch 结束仅 flush `g_orch_is_done` + `g_task_id` | `aicpu_runtime.c` |
| queue cache 迁至 `platform_queue_*` hooks | `queue.h`, `queue_cache_hooks.c` |
| `g_completed_cnt` 发布改 `memory_order_release` | `dispatch.c` |

### Phase 3 — platform 结构（v5）

| 变更 | 文件 |
|------|------|
| `cache_invalidate_range` / `cache_flush_range` 拆至独立文件 | `sim/cache_ops.c`, `onboard/cache_ops.c` |
| 自 `platform_regs.c` / `npu_hal.c` 移除重复实现 | 同上 |

### Phase 4 — sched 同步对齐 simpler atomic+wmb（v6）

| 变更 | 文件 |
|------|------|
| 删 `cache_flush_sched_counters()` bulk flush | `ring_buf.h`, `cutter.c`, `dispatch.c` |
| `advance_task_id()` → `wmb` + `fetch_add(release)` | `ring_buf.h` |
| `sched_acquire_counters()` acquire fence | `dispatch.h`, `cutter.c`, `dispatch.c` |
| `g_commit_task_id` 原子化 | `cutter.c`, `cutter.h` |
| `add_predecessors` 去 per-edge cvac，批次末 `wmb` | `ring_buf.h` |
| cutter `resolve_dep` / `g_predecessor_cnt` 写后 `wmb` 替代 cvac | `cutter.c` |
| Orch 结束 `wmb` 替代 `g_orch_is_done`/`g_task_id` cvac | `aicpu_runtime.c` |
| queue unlock → `wmb`；lock 去 civac | `onboard/queue_cache_hooks.c` |
| dispatch payload 去 AICPU cvac（AICore `dcci` 已有） | `dispatch_payload.c` |
| dispatch 写 reg 前 `wmb` | `dispatch.c` |

## 保留（onboard 必需）

- Runtime `cache_invalidate_range`（host DMA）
- cutter/dispatch **consumer 侧** per-slot `cache_invalidate_range`（读 orch 写的 slot）
- handshake 写路径 flush + store 前 barrier
- AICore `dcci(exec_payload)`（`aicore_executor.cpp`）
- COND 读前 `OUT_OF_ORDER_LOAD_BARRIER`（`npu_hal.c`）
- Stats/trace 写 host 可见 GM 的 flush

## 锁 / 原子对照

| proxy | simpler | 处置 |
|-------|---------|------|
| `queue_t.lock` spin | Vyukov 无锁 | **保留**（QuteMiao） |
| `g_lock_buf` 未使用 | 无 | **保留**（QuteMiao） |
| `g_task_id` / `g_commit_task_id` | atomic release/acquire | **已对齐** |
| sched bulk flush | atomic+wmb in ring_buf/dispatch | **已删除** |
| queue hooks civac/cvac | wmb + lock acquire | **已简化** |

## 验证（每 Phase）

- **CPU**：4 case Makefile `run`，全 PASS。
- **上板**：4 case `task-submit` + `ESL_PROXY_ORCH_CASE`；单 case 失败最多重试 2 次（共 3 次）。

| Phase | CPU | 上板 |
|-------|-----|------|
| 1–3 (v5) | PASS | PASS（重试后） |
| 4 (v6) | PASS | PASS（重试后） |

## Changelog

- 2026-06-26：Phase 1–3 落地；`cache_ops.c` 拆分；审计文档初版。
- 2026-06-26：Phase 4 sched 同步对齐 simpler atomic+wmb；删 bulk counter flush；删除 `sched_sync.h`，迁入 `ring_buf.h` / `dispatch.h`。
- 2026-06-26：统一 `esl_platform_init/shutdown`；删 `aicore_bridge.h` / `platform_bringup`；handshake bootstrap 经 `platform_handshake_aicore_bootstrap` hook。
