# onboard 代码分析：fake_aicore 使用情况 与 未使用代码

分析对象：`esl_proxy/include/onboard/`（**6** 头）与 `esl_proxy/src/onboard/`（**4** 实现源 + 4 文档）。
"编译进 onboard" 的三个目标：AICPU `.so`、AICore `.o`、host runner，外加独立 dispatcher `.so`。

---

## 一、是否使用了 fake_aicore

| 项 | 是否使用 | 说明 |
|----|:--:|------|
| **非HW 的 fake AICore 寄存器模拟** | ❌ 已删除 | onboard 一律走真实 HW AICore 寄存器 |
| **`fake_kernel`（AICore 忙等桩）** | ✅ | 在 `aicore_kernel.cpp` 内，`EslFakeTaskArgs` 定义于 `onboard_config.h` |
| **`ESL_PROXY_FAKE_AICORE_COUNT=2`** | ✅ | 定义于 `onboard_config.h` |

---

## 二、未使用 / 不可达代码

### A. 已清理

| 项 | 状态 |
|------|------|
| `smoke_orch.c` | 已删除，编排并入 `aicpu_kernel.c`（`ORCH_CASE`） |
| 多文件 platform/glue/host | 已合并为 4 个单 TU |

### B. 编译进 onboard、三线程路径使用

| 代码 | 位置 | 说明 |
|------|------|------|
| 多线程调度入口 | `src/cutter.c`、`src/dispatch.c` | onboard：`esl_aicpu_execute` idx 0/1/2 分流 |
| `onboard_crosscore_sync` | `src/onboard/onboard_crosscore_sync.c` | 跨 AICPU 核 cache flush/invalidate |

### C. 易误判为"未用"、但实际在用

| 代码 | 说明 |
|------|------|
| fast-complete 分支 | 2 核时多数 task 走 fast-complete |
| `onboard_shm_sync`（`aicpu_bridge.h`） | 单线程仍调用 |
| `onboard_config.h` 时间 inline | 经 `log.h` → `log.c` 使用 |
| `aicpu_dispatcher.c` | bootstrap 写 inner SO |

---

## 三、文件清单（合并后）

**源（4）**：`aicpu_kernel.c`、`aicore_kernel.cpp`、`host_runner.c`、`aicpu_dispatcher.c`

**头（6）**：`onboard_config.h`、`esl_runtime.h`、`kernel_args.h`、`aicpu_bridge.h`、`aicore.h`、`tools.h`（含原 `log.h` AICPU 日志）
