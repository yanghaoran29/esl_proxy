# esl_proxy 死代码分析报告

## 概述

对 `esl_proxy/esl_proxy/` 代码库进行了全面的死代码分析，涵盖以下类别：

| 类别 | 发现数量 |
|------|----------|
| 未编译的源文件 | 1 个 |
| 死的全局函数 | 6 个 |
| 不可达代码 | 1 处 |
| 未使用的宏/定义 | 17 个 |
| 未使用的结构体字段 | 11 个（含 2 个填充字段） |
| 注释掉的代码 | 2 处 |
| 未使用的包装宏 | 7 个 |
| 重复声明 | 1 组（7 个函数） |
| `#if 0` 死代码块 | 0 处 |
| 未使用的 static 函数 | 0 个（初始 35 个疑似均为误报） |

分析范围：`src/`、`include/`、`cases/`、`tests/` 下所有 `.c` 和 `.h` 文件，以及 `Makefile`、`cmake/` 构建配置。

---

## 1. 未编译的源文件

### 1.1 `src/algorithm/manager.c`

**状态：完全死代码** — 未被任何构建配置编译。

- `Makefile` 的 `SRCS` 列表中不包含此文件
- `cmake/sources.cmake` 的源文件列表中不包含此文件
- 项目文档 `doc/overview.md` 明确记载："manager.c 保留文件但未接入 sim/onboard 线程创建"

该文件定义了 `manager_worker` 函数（唯一函数），但该函数从未被调用，文件也从未被编译。

---

## 2. 死的全局函数

以下函数在头文件中声明、在 `.c` 文件中定义，但在整个代码库中**零调用**：

### 2.1 `manager_worker`

| 属性 | 值 |
|------|-----|
| 声明 | [manager.h:10](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/manager.h#L10) |
| 定义 | [manager.c:15](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/manager.c#L15) |
| 调用点 | 0 |
| 说明 | manager.c 本身未被编译（见 §1.1）；函数主体中有不可达代码（见 §3.1） |

### 2.2 `cutter` (queue_t*, queue_t*)

| 属性 | 值 |
|------|-----|
| 声明 | [cutter.h:17](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/cutter.h#L17) |
| 定义 | **无**（cutter.c 中不存在此签名） |
| 调用点 | 0 |
| 说明 | 悬空声明：头文件声明了 `cutter(queue_t*, queue_t*)`，但从未定义也从未调用。可能是早期设计的接口残留。 |

### 2.3 `executor_worker`

| 属性 | 值 |
|------|-----|
| 声明 | [executor.h:40](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/executor.h#L40) |
| 定义 | [executor.c:37](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/executor.c#L37) |
| 调用点 | 0 |
| 说明 | 已定义但从未被调用，连 `pthread_create(&executor_worker, ...)` 都没有。可能是计划中的 executor 线程入口，但功能从未接入。 |

### 2.4 `is_l2_swimlane_enabled`

| 属性 | 值 |
|------|-----|
| 声明 | [swimlane_aicpu.h:27](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_aicpu.h#L27) |
| 定义 | [swimlane_aicpu.c:53](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/swimlane/swimlane_aicpu.c#L53) |
| 调用点 | 0 |
| 说明 | 查询函数，可能曾用于条件判断，现已被内联或移除。 |

### 2.5 `get_aicore_profiling_flag`

| 属性 | 值 |
|------|-----|
| 声明 | [swimlane_aicore.h:17](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_aicore.h#L17) |
| 定义 | weak 函数，定义于 `aicore_wrapper.cpp:23` 和 `aicore_entry.cpp:25` |
| 调用点 | 0 |
| 说明 | 与 `set_aicore_profiling_flag`（有 3 处调用）配对，但 getter 从未使用。 |

### 2.6 `get_l2_swimlane_aicore_head`

| 属性 | 值 |
|------|-----|
| 声明 | [swimlane_aicore.h:19](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_aicore.h#L19) |
| 定义 | weak 函数，定义于 `aicore_wrapper.cpp:34` 和 `aicore_entry.cpp:36` |
| 调用点 | 0 |
| 说明 | `aicore_executor.c` 改为直接解引用 rotation_table 获取 head，未走此函数。 |

---

## 3. 不可达代码

### 3.1 `manager_worker` 中的提前 return

**文件**：[manager.c:15-25](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/manager.c#L15-L25)

```c
void *manager_worker(void *arg)
{
    mem_pool_t *pool = (mem_pool_t *)arg;
    WORKER_LOGF("started pool=%p %d", (void *)pool, 0);
    return NULL;          // 第20行 - 提前返回
    while (1) {           // 第21行 - 不可达
        mem_pool_process_when2free(pool);
    }
    return NULL;          // 第25行 - 不可达
}
```

**分析**：第 20 行的无条件 `return NULL;` 导致其后的 `while(1)` 循环和第二个 `return NULL;` 永远不会执行。这看起来像是调试残留或功能禁用的临时措施。注意：由于 manager.c 本身未被编译（§1.1），这段代码属于"双重死亡"。

---

## 4. 未使用的宏/定义

### 4.1 `include/algorithm/conf.h`

| 行号 | 宏名 | 值 | 说明 |
|------|------|----|------|
| [32](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L32) | `EXECUTOR_THREAD_CNT` | `1` | 定义后从未被任何文件引用 |
| [60](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L60) | `DEP_DUMP` | `0` | 有 `#ifndef`/`#define` 守护，但无任何 `#if DEP_DUMP` 使用；注释声称"runtime via DEP_DUMP=1 env"，但代码中无 `getenv("DEP_DUMP")` |
| [65](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L65) | `NO_DEPS` | `0` | 有 `#ifndef`/`#define` 守护，但无任何 `#if NO_DEPS` 使用 |

### 4.2 `include/platform/platform_config.h`

| 行号 | 宏名 | 值 | 说明 |
|------|------|----|------|
| [33](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L33) | `PLATFORM_MAX_AICPU_THREADS_JUST_FOR_LAUNCH` | `6` | 仅定义，从未引用 |
| [35](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L35) | `PLATFORM_MAX_AIC_PER_THREAD` | 宏表达式 | 仅用于定义 `PLATFORM_MAX_CORES_PER_THREAD`（本身也未使用） |
| [36](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L36) | `PLATFORM_MAX_AIV_PER_THREAD` | 宏表达式 | 同上，传递性未使用 |
| [37](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L37) | `PLATFORM_MAX_CORES_PER_THREAD` | 宏表达式 | 仅定义，从未引用 |
| [40](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L40) | `PLATFORM_PROF_BUFFER_SIZE` | `1000` | 仅在 `#ifdef __cplusplus` 的 C++ constexpr 中使用，C 代码中未使用 |
| [53](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L53) | `PROFILING_FLAG_NONE` | `0u` | 仅定义，从未作为标志值引用 |
| [54](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L54) | `PROFILING_FLAG_DUMP_TENSOR` | `(1u << 0)` | 仅定义，从未引用 |
| [56](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L56) | `PROFILING_FLAG_PMU` | `(1u << 2)` | 仅定义，从未引用 |
| [57](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L57) | `PROFILING_FLAG_DEP_GEN` | `(1u << 3)` | 仅定义，从未引用 |
| [58](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L58) | `PROFILING_FLAG_SCOPE_STATS` | `(1u << 4)` | 仅定义，从未引用 |
| [61](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L61) | `CLEAR_PROFILING_FLAG(flags, bit)` | 函数式宏 | 仅定义，从未调用（`SET_PROFILING_FLAG`/`GET_PROFILING_FLAG` 有使用） |
| [113](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L113) | `ESL_PROXY_FAKE_AICORE_COUNT` | 宏表达式 | 注释标记为"Deprecated alias"，从未引用 |
| [123](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L123) | `SIM_REG_BLOCK_SIZE` | `0x500U` | 仅定义，从未引用 |
| [145](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L145) | `AICPU_TASK_INVALID` | `(-1)` | 仅定义，从未引用 |

---

## 5. 未使用的结构体字段

### 5.1 `include/algorithm/task.h` — `struct task_desc`

| 行号 | 字段名 | 类型 | 说明 |
|------|--------|------|------|
| [57](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h#L57) | `mode` | `org_mode_t` | **只写不读**：在 `ring_buf.h:142` 和 `cases/qwen3_dynamic_tensormap.h:68` 被写入 `ORG_MODE_SPMD_SYNC`，但从未被读取 |
| [58](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h#L58) | `kernel` | `void *` | **从未访问**：仅在 specs 文档中提及 |
| [59](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h#L59) | `index` | `uint32_t` | **从未访问** |

### 5.2 `include/algorithm/task.h` — `task_state` 结构体

| 行号 | 字段名 | 类型 | 说明 |
|------|--------|------|------|
| [50](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h#L50) | `task_id` | `uint16_t` | **只写不读**：仅在 `cutter.c:17` 被置零 |

### 5.3 `include/algorithm/tensor.h` — `struct Tensor`

| 行号 | 字段名 | 类型 | 说明 |
|------|--------|------|------|
| [28](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/tensor.h#L28) | `owner_task_id` | `uint64_t` | **只写不读**：仅在 `tensor.h:82` 被置零；通过 `memcpy` 复制到 `TmEntry` 后立即被 `tm_link_entry()` 覆盖 |
| [33](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/tensor.h#L33) | `manual_dep` | `uint8_t` | **只写不读**：仅在 `tensor.h:87` 被置零 |
| [35](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/tensor.h#L35) | `_pad_cl1` | `uint8_t` | 填充字段（缓存行对齐，设计如此） |
| [41](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/tensor.h#L41) | `_pad_cl2[36]` | `uint8_t[]` | 填充字段（缓存行对齐，设计如此） |

### 5.4 `include/swimlane/swimlane_types.h` — `L2SwimlaneDataHeader`

以下 4 个字段注释标记为 **"Legacy header tail — kept for ABI"**，仅在 `host_swimlane.c:695-698` 被初始化为零，**从未被功能代码读取**：

| 行号 | 字段名 | 类型 | 说明 |
|------|--------|------|------|
| [241](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_types.h#L241) | `num_sched_phase_threads` | `uint32_t` | 仅置零，从未读取 |
| [242](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_types.h#L242) | `num_orch_phase_threads` | `uint32_t` | 仅置零，从未读取 |
| [243](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_types.h#L243) | `num_phase_cores` | `uint32_t` | 仅置零，从未读取 |
| [244](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/swimlane/swimlane_types.h#L244) | `core_to_thread[]` | `int8_t[]` | 仅 `memset` 置为 -1，从未读取 |

---

## 6. 注释掉的代码

| 文件 | 行号 | 内容 | 说明 |
|------|------|------|------|
| [ring_buf.h:101](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/ring_buf.h#L101) | 101 | `// int slotIdx = task_id & RING_MASK;` | 被第 102 行 `int slotIdx = task_id;` 替代 |
| [shm.c:77](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/shm.c#L77) | 77 | `// set_mix(tid);` | 函数调用被注释掉，无替代代码 |

---

## 7. 未使用的包装宏

`include/swimlane/swimlane_host.h` 中定义了以下 7 个包装宏，但**从未被外部代码作为宏调用**（对应的函数都是直接在 `host_swimlane.c` 内部调用，而非通过宏）：

| 行号 | 宏名 |
|------|------|
| ~L17 | `ESL_SWIMLANE_HOST_SET_LEVEL` |
| ~L18 | `ESL_SWIMLANE_HOST_INIT` |
| ~L20 | `ESL_SWIMLANE_HOST_STOP_EXPORT` |
| ~L21 | `ESL_SWIMLANE_HOST_FINALIZE` |
| ~L22 | `ESL_SWIMLANE_HOST_DATA_BASE` |
| ~L23 | `ESL_SWIMLANE_HOST_ROTATION_TABLE` |
| ~L24 | `ESL_SWIMLANE_HOST_SET_CORE_TYPES` |

---

## 8. 重复声明

`include/swimlane/swimlane_aicpu.h` 和 `include/swimlane/swimlane_device.h` 对以下 7 个函数做了**完全相同的声明**：

1. `set_platform_l2_swimlane_base`
2. `set_platform_l2_swimlane_aicore_rotation_table`
3. `set_l2_swimlane_enabled`
4. `is_l2_swimlane_enabled`
5. `l2_swimlane_aicpu_init`
6. `l2_swimlane_aicpu_on_aicore_dispatch`
7. `l2_swimlane_aicpu_flush`

这不是死代码，但属于冗余声明，可能导致维护不一致。

---

## 9. 经验证非死代码的项目

以下项目最初被标记为疑似死代码，但经详细核查确认为**活跃使用**：

### 9.1 全部 35 个 static 函数

初始分析报告了 35 个"未使用的 static 函数"，分布在 `dispatch.c`、`dispatch_double_buffer.c`、`dispatch_spmd_mix.c`、`host_onboard.c`、`device_runner_instant.c`、`tools.c`、`aicpu_dispatcher.c`、`onboard_log.c`、`swimlane_aicpu.c`、`handshake.c`、`dispatch_payload.c` 等文件中。

**核查结论：全部 35 个函数均在其定义文件内被直接调用，无一为死代码。** 初始报告为误报。

### 9.2 `dispatch_spmd_mix.c`

- **不是死代码**，也**不是可选的 DISPATCH 变体**
- `Makefile` 中无 `DISPATCH=spmd_mix` 选项，但该文件被**无条件编译**
- 它是 SPMD 共享辅助模块，被 `dispatch.c`、`dispatch_double_buffer.c`、`cutter.c` 大量调用（20+ 处）

### 9.3 `device_runner.c` 与 `device_runner_instant.c`

- **两者均活跃使用，非重复实现**
- `device_runner.c`：始终编译，提供 `esl_sim_aicore_workers_start/stop` 生命周期管理
- `device_runner_instant.c`：仅 `SIM_AICORE=instant`（默认）时编译，提供 instant manager 线程主函数
- 两者互补，在默认 instant 构建中协同工作

---

## 10. 汇总与建议

### 死代码汇总

| 类别 | 数量 | 严重程度 |
|------|------|----------|
| 未编译源文件 | 1（manager.c） | 中 — 整个文件未接入构建 |
| 死全局函数 | 6 | 中 — 含 1 个悬空声明、2 个死 weak 函数 |
| 不可达代码 | 1 处 | 低 — 在未编译的 manager.c 中 |
| 未使用宏 | 17 | 低 — 不影响运行，但增加阅读负担 |
| 未使用结构体字段 | 9 个功能字段 + 2 个填充 | 低 — 其中 4 个为 ABI 兼容保留 |
| 注释代码 | 2 行 | 极低 |
| 未使用包装宏 | 7 | 极低 |
| 重复声明 | 7 个函数 | 极低 — 不影响编译 |

### 清理建议

1. **高优先级**：删除 `manager.c` 和 `manager.h`（未编译 + 不可达代码 + 死函数）
2. **高优先级**：删除 `cutter.h` 中的 `cutter(queue_t*, queue_t*)` 悬空声明
3. **中优先级**：删除 `executor_worker` 函数及其声明（已定义但从未调用）
4. **中优先级**：删除 `is_l2_swimlane_enabled`、`get_aicore_profiling_flag`、`get_l2_swimlane_aicore_head` 三个死函数
5. **低优先级**：清理 `platform_config.h` 中的未使用宏（尤其是 PROFILING_FLAG 系列）
6. **低优先级**：清理 `swimlane_host.h` 中的未使用包装宏
7. **谨慎处理**：`swimlane_types.h` 中的 4 个遗留 ABI 字段——如果有外部工具依赖二进制布局，删除前需确认
8. **谨慎处理**：`task_desc` 的 `kernel`、`index` 字段和 `Tensor` 的 `owner_task_id`、`manual_dep` 字段——可能有未来使用计划，建议与团队确认
