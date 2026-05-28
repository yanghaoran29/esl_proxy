# qwen3_decode.cpp — Fake API 使用清单与 proxy 缺失功能分析

分析对象：[`examples/qwen3_decode.cpp`](qwen3_decode.cpp)（484 行，从 `simpler-V200/examples/qwen3/dynamic_manual_scope/orchestration/qwen3_decode.cpp` 移植）

该示例是一个**纯 fake 移植**：编排逻辑（任务结构、依赖、tiling）已完整重写到 esl_proxy 的 `task_desc`（[`include/dag/task.h`](../include/dag/task.h)）之上，但所有**运行时交互**全部走 [`include/dag/fake_orchestration_api.h`](../include/dag/fake_orchestration_api.h) 的桩函数。换言之：**DAG 描述层已就绪，运行时执行层全缺**。

`task.h` 是当前唯一非 fake 的真实接口（`task_desc` + `task_type_t` + `org_mode_t` + `ring_idx`）。其余一切都是 fake。

---

## 一、Fake 使用清单（按出现位置）

| Fake 构件 | 出现位置 | 当前桩行为 | 真实应做的事 |
|---|---|---|---|
| `Fake_Tensor<N,T>` | 全部 41 个 tensor 声明（行 92–140） | 一个 `T data[N]` 全局数组，落在 .bss/.data | 从内存池分配 GM tensor，返回设备地址 |
| `fake_runtime_info` | 每个 task 的 `.data`（行 209…472） | `std::vector<void*>` 的 inputs/outputs/inouts + `std::vector<int64_t>` scalars | 写入 Runtime Information Ring Buffer 的一条定长 entry |
| `fake_next_task_id()` | 每个 task 分配 id（行 213…475） | `static` 单调自增计数器 | Ring Buffer 索引分配器（带回收 / back-pressure） |
| `FAKE_TASK_ID_INVALID` | 哨兵值（行 198…477） | 常量 `0xFFFF` | 真实 invalid-id 语义 + 与 ring index 协调 |
| `fake_add_dep` / `fake_add_deps` / `fake_add_deps_range` | 全部依赖声明（行 236…478） | **空函数体**，参数被 `(void)` 丢弃 | 把 consumer 追加到 producer 的 `g_dep_buf` successor 链，bump consumer 的 pred 计数 |
| `fake_submit` | 每个 task 提交（行 224…479） | **空函数体**，`task_desc` 被丢弃 | memcpy 到 `g_basic_buf` / `g_runtime_buf`，调用 `ready_enqueue(type, mode, &d)` |
| `FAKE_MANUAL_SCOPE()` / `fake_scope_begin/end` | 编排入口（行 194） | 空 RAII guard | 建立 manual-dependency scope（Graph 生命周期、scope 内依赖收集） |
| `k_*` kernel 桩（17 个） | `task_desc.kernel`（行 169–185） | 空函数 `{}`，仅取地址 | 链接真实 AIC/AIV/MIX kernel 二进制 |

> 注：`fake_submit_*_task` 包装器已在头文件注释中说明被移除（行 11–12），现在直接构造 `task_desc` 字面量。这部分**不算缺失**，是有意的接口简化。

---

## 二、proxy 当前缺失的功能（按 spec 归属）

按 fake 构件回溯到 `specs/` 里规划但尚未接通到本示例的真实组件：

### 1. 任务提交链路（spec 008-task / 009-ring-buffer / 010-mpmc-queue）
`fake_submit` 是最大的空洞。真实路径需要三件事全部就绪：
- **Basic Ring Buffer 写入**：`task_desc` → `g_basic_buf[ring_idx(id)]`（spec 009）
- **Runtime Information Ring Buffer 写入**：`fake_runtime_info` 的 vector 形态要换成定长 entry 写入 `g_runtime_buf`（spec 008 FR-010/011）
- **ready_enqueue**：按 `type`（CUBE/VECTOR/MIX）和 `mode`（SINGLE/SPMD_ASYNC…）入 MPMC ready queue（spec 010）

当前这三步全是注释里的伪代码（[fake_orchestration_api.h:87-91](../include/dag/fake_orchestration_api.h#L87-L91)），**没有任何 buffer 真正被写、没有任何 task 真正入队**。

### 2. 依赖图构建（spec 001-orchestrator）
`fake_add_dep` 是空函数 —— 这意味着**整个 DAG 的边目前完全没有被记录**。示例里精心构造的所有依赖（rmsnorm→q/k/v_proj、qk_norm 的 3-producer fan-in、out_proj 的 online_softmax range fan-in、down_proj 的 silu range fan-in 等）在运行时层面**全部丢失**。真实需要：
- `g_dep_buf` successor 链（3 inline + 链式扩展 entry）
- consumer 的 predecessor refcount，供调度器判断 ready

### 3. Task ID / Ring 索引分配（spec 009-ring-buffer）
`fake_next_task_id` 是无界自增。本示例任务数已超 `RING_SIZE=4096`（见下方"规模问题"），真实分配器必须：
- 复用 ring slot（id 回绕 + 在途任务不被覆盖的 back-pressure）
- 与 `ring_idx()` 的 4096 容量协调

### 4. 内存池 / Tensor 分配（spec 007-memory-pool）
`Fake_Tensor` 把每个 tensor 当成静态全局数组，**没有内存池、没有复用、没有设备地址映射**。原始 PTO2 编排里"按 pool slot 复用"的语义（头文件注释行 115–116 提到）完全没实现 —— 现在每个中间 tensor 独占一份全局 BSS。真实需要 spec 007 的内存池给出可复用的 GM 地址。

### 5. Kernel 执行（spec 003-executor）
17 个 `k_*` 是空函数。`task_desc.kernel` 只是个地址占位。真实需要 executor 把 CUBE/VECTOR/MIX 三类 kernel 派发到对应核并执行。**MIX 任务尤其特殊**：原始 `MixedKernels{10,11,11}` 的第二个 AIV lane 被丢弃（头文件注释行 16–17），现在 MIX 只携带单 kernel 指针，依赖 Dispatch 路由到双能力 executor —— 这条路由（spec 004-dispatch）也未接通。

### 6. Manual-scope / Graph 生命周期（spec 001-orchestrator）
`FAKE_MANUAL_SCOPE` 是空 RAII。真实的 manual-dependency scope 应负责 scope 内任务的依赖收集边界与 Graph 对象生命周期。
