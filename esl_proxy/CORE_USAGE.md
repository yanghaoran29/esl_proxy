# esl_proxy 核心 `include/` 与 `src/`（onboard 之外）调用情况分析

分析对象：`esl_proxy/include/*.h` 与 `esl_proxy/src/*.c`（不含 `onboard/` 子目录）。

两条构建/运行路径：

- **host 仿真**：`make run`（`Makefile`，host gcc）。`SRCS = main, executor, dispatch, cutter, manager, log, shm`。
- **上板 AICPU**：`build_aicpu.sh` 仅从核心显式取 **`cutter.c / dispatch.c / shm.c / log.c`**（其余 onboard 源在 `src/onboard/`）。

判定口径：①是否被某构建**编译**；②运行时函数是否被**真正调用**。

---

## 一、`src/*.c` 结论

| 文件 | host 仿真 | 上板 | 运行时调用 | 结论 |
|------|:--:|:--:|------|------|
| `main.c` | ✅ 入口 | ✗ | ✅ | **正常使用** — host 仿真主入口（init → 编排 →（默认还起 cutter/dispatch 线程）） |
| `cutter.c` | ✅ | ✅ | ✅ | **正常使用** — 双构建共用，依赖解析/就绪 |
| `dispatch.c` | ✅ | ✅ | ✅ | **正常使用** — 双构建共用，下发/完成 |
| `shm.c` | ✅ | ✅ | ✅ | **正常使用** — 全局定义与 `init_ctrl_t` 等 |
| `log.c` | ✅ | ✅ | ✅ | **正常使用** — 日志后端 |
| `executor.c` | ✅ 编译 | ✗ | ❌ | **死代码（编译但不调用）** — `executor_init()`/`executor_worker()` 在 `main.c` 中均被注释；调度走 cutter/dispatch，不用独立 executor 线程。注意上板用的是另一个 `src/onboard/executor.cpp`（同名不同文件）。 |
| `manager.c` | ✅ 编译 | ✗ | ❌ | **死代码（编译但不调用）** — 仅含 `manager_worker()`，`main.c` 中 `pthread_create(manager_worker)` 被注释；内存池 `mem_pool_init` 来自 `mem_pool.h`（header-only），与 manager.c 无关。 |
| `fake_kernel.c` | ✗ | ✗ | ❌ | **完全未用** — 不在 `Makefile` SRCS（含 `__aicore__/__global__`，host gcc 编不了），上板用的是 `src/onboard/fake_kernel.cpp`。仅自引用，属早期遗留 stub。 |

## 二、`include/*.h` 结论

| 头文件 | 用途 | 结论 |
|--------|------|------|
| `conf.h` | 全局配置常量（线程数、RING_SIZE、开关等） | **正常使用**（广泛） |
| `task.h` | 任务/前驱/后继结构体、状态枚举 | **正常使用** |
| `ring_buf.h` | 编排环形缓冲、`new_task`/`add_predecessors`/`init_predecessors` | **正常使用**（含上板 smoke_orch） |
| `queue.h` | ready/completed 队列（自旋锁批量入出队） | **正常使用** |
| `spin.h` | `spin_wait` | **正常使用**（被 `queue.h` 用） |
| `dispatch.h` | dispatch 接口、`ctrl_t` | **正常使用** |
| `cutter.h` | cutter 接口 | **正常使用** |
| `log.h` | `WORKER_LOGF`/`MAIN_LOGF` 宏 | **正常使用** |
| `executor.h` | **类型 `executor_t`** + `EXEC_SLOT_EMPTY` 哨兵 | **正常使用（类型）** — `g_executors[][]` 在 shm/dispatch 中使用；但其中声明的 `executor_worker()`（实现在死的 `executor.c`）未被调用 |
| `mem_pool.h` | 内存池（`mem_pool_init`/`_fifo`，static inline） | **正常使用**（`main.c` 调用；多文件 include） |
| `mpmc_queue.h` | MPMC 队列模板 | **间接使用** — 仅被 `ring_buf.h` include（编排路径）；建议确认是否真有实例，否则可精简 |
| `manager.h` | `manager_worker()` 声明 | **死代码** — 仅服务于未被调用的 `manager.c` |
| `tensor.h` | `Tensor` 句柄/输入输出指针 | **正常使用**（host 仿真 cases；上板 smoke 不用） |
| `tensormap.h` / `tensormap_core.h` | tensormap 数据依赖追踪 | **正常使用**（host 仿真 `qwen3_dynamic_tensormap` / `paged_attention` cases；上板 smoke 不用） |

> `tensor*.h` 在上板构建里被包含进来但 smoke_orch 未真正用到（smoke 是手写依赖链，不走 tensormap）。它们是 host 仿真四个 case 的核心。

---

## 三、可精简项（建议）

| 项 | 性质 | 建议 |
|----|------|------|
| `src/fake_kernel.c` | 任何构建都不编译 | **可直接删除**（上板已有 `src/onboard/fake_kernel.cpp`） |
| `src/executor.c` + `src/manager.c` + `include/manager.h` | host 仿真编译但运行时全部注释掉 | **可删除或显式 `#if 0`** —— 当前 scheduler 用 cutter/dispatch 线程模型，独立 executor/manager 线程模型未启用。若计划恢复该模型则保留并加注释说明。 |
| `include/mpmc_queue.h` | 仅被 `ring_buf.h` include | 确认 `ring_buf` 是否真实例化 MPMC 队列；若无则连同 include 一起精简 |

> 注：`executor.c`/`manager.c` 仍在 `Makefile` 的 `SRCS` 里，删除时需同步从 `SRCS` 移除，否则 `make` 报缺文件。`executor.h`（类型）与 `executor.c`（线程实现）是两回事——**保留 `executor.h`，仅 `executor.c` 是死代码**。
