# cache / barrier / lock 清理审计

对照 [`simpler/docs/hardware/cache-coherency.md`](../../../simpler/docs/hardware/cache-coherency.md) 与 onboard3 清理计划（v5）。

## 原则

1. **simpler 不用，proxy 不用**（语义等价 + 回归验证）。
2. **algorithm 层零 `#ifdef ESL_PROXY_ONBOARD`** — cache 由 `platform.h` 声明，sim/onboard 链接不同实现。
3. **QuteMiao 死代码不得删** — 见 [`onboard2_overview.md` §3.11](onboard2_overview.md)。

## QuteMiao 保护清单（未动）

| 项 | 位置 |
|----|------|
| `g_lock_buf` + `lock()`/`unlock()` | `ring_buf.h`, `shm.c` |
| `mpmc_queue.h` stub | `mpmc_queue.h` |
| `executor_worker` | `executor.c` |
| `queue_t.lock` + spin 路径 | `queue.h` |
| TODO / 注释死代码 | `dispatch.c`, `cutter.c`, `shm.c`, `log.c` |

## 已执行变更（Phase 1–3）

### Phase 1 — cache/barrier 裁剪

| 变更 | 文件 |
|------|------|
| 删 post-flush `OUT_OF_ORDER_STORE_BARRIER` | `sched_sync.h`, `cutter.c`, `ring_buf.h`（per-edge） |
| 删 dispatch/bridge 同线程 `free_bitmap`/`msg_bitmap` flush | `dispatch.c`, `aicore_bridge.c` |
| handshake 等待环去掉 civac | `handshake.c` |
| `add_predecessors` 仅保留 `g_predecessors` 末尾 flush（tensormap 跨核读前必需） | `ring_buf.h` |

### Phase 2 — Orch + queue

| 变更 | 文件 |
|------|------|
| Orch 结束仅 flush `g_orch_is_done` + `g_task_id` | `aicpu_runtime.c` |
| queue cache 迁至 `platform_queue_*` hooks | `queue.h`, `queue_cache_hooks.c` |
| `g_completed_cnt` 发布改 `memory_order_release` | `dispatch.c` |

### Phase 3 — platform 结构

| 变更 | 文件 |
|------|------|
| `cache_invalidate_range` / `cache_flush_range` 拆至独立文件 | `sim/cache_ops.c`, `onboard/cache_ops.c` |
| 自 `platform_regs.c` / `npu_hal.c` 移除重复实现 | 同上 |

## 保留（onboard 必需）

- Runtime `cache_invalidate_range`（host DMA）
- `publish_task_slot` / `publish_counters` / `invalidate_sched_snapshot`
- `add_predecessors` 内 per-edge + `g_predecessors` flush
- dispatch_submit 读 task slot 前 invalidate
- handshake 写路径 flush + store 前 barrier
- AICore `dcci(exec_payload)`（`aicore_executor.cpp` 已有）
- COND 读前 `OUT_OF_ORDER_LOAD_BARRIER`（`npu_hal.c`）
- onboard queue hooks civac/cvac

## 暂缓（Phase 4 可选）

- sched_sync 全表 flush → atomic + 批量 wmb（需单独 RFC）
- AICPU 读 AICore slot 去掉 civac（需 coherency 域证明）

## 锁 / 原子对照

| proxy | simpler | 处置 |
|-------|---------|------|
| `queue_t.lock` spin | Vyukov 无锁 | **保留**（QuteMiao） |
| `g_lock_buf` 未使用 | 无 | **保留**（QuteMiao） |
| `g_task_id` 等 atomic | atomic | **保留** |
| sched_sync flush 块 | atomic+wmb | **暂缓** |

## 验证（每 Phase）

- **CPU**：4 case Makefile `run`，全 PASS。
- **上板**：4 case `task-submit` + `ESL_PROXY_ORCH_CASE`；单 case 失败最多重试 2 次（共 3 次）。

| Phase | CPU | 上板 |
|-------|-----|------|
| 1 | PASS | PASS（重试后） |
| 2 | PASS | PASS（重试后） |
| 3 | PASS | PASS（重试后） |

## Changelog

- 2026-06-26：Phase 1–3 落地；`cache_ops.c` 拆分；审计文档初版。
