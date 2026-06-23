# esl_proxy 上板代码说明（`include/onboard` 与 `src/onboard`）

最小上板 smoke 的全部代码，扁平存放于：

- `esl_proxy/include/onboard/` — 头文件（**7**）
- `esl_proxy/src/onboard/` — 源文件（**4** 实现 + 4 文档）

按"执行所在位置"分为五层：**Host 启动器** → **AICPU 调度内核** → **AICore 执行内核** → **Dispatcher** →
**移植自 simpler 的平台运行时**，外加一组 host/device 共用的**布局与平台头**。

数据流：host 加载三件产物并按序 launch → AICPU 单线程跑 orchestration/cutter/dispatch，
经寄存器把任务下发给 AICore → AICore（fake_kernel 忙等）写 FIN → AICPU 轮询 FIN 收完成 →
依赖链推进直至全部完成 → AICPU best-effort 关停 AICore → host 读 `device_wall` 打印 `OK`。

## 合并后文件 → 构建目标

| 文件 | 构建目标 | 内容 |
|------|----------|------|
| `aicpu_kernel.c` | AICPU `.so` | platform + glue + runtime + case_orch（`ORCH_CASE`） |
| `aicore_kernel.cpp` | AICore `.o` | kernel 入口 + executor + fake_kernel（weak） |
| `host_runner.c` | Host 可执行 | main + loader + launcher + host_regs + elf fingerprint |
| `aicpu_dispatcher.c` | Dispatcher `.so` | bootstrap inner SO 写入 |

> 核心调度逻辑（cutter / dispatch / shm / log）位于 `esl_proxy/src/`，由 AICPU 构建脚本直接列入编译，不在本目录。

## 构建与各目标取哪些文件

- **AICPU kernel** (`libaicpu_kernel.so`)：`aicpu_kernel.c` + `src/{cutter,dispatch,shm,log}.c`
- **AICore kernel** (`aicore_kernel.o`)：`aicore_kernel.cpp`（AIC+AIV 双编）
- **Host runner** (`esl_onboard_runner`)：`host_runner.c`
- **Dispatcher** (`libsimpler_aicpu_dispatcher.so`)：`aicpu_dispatcher.c`

## 共用头要点

- **`onboard_config.h`**：统一配置（bring-up、平台常量、RegId、时间、HAL、`EslFakeTaskArgs`）
- **`esl_runtime.h` / `kernel_args.h`**：GM 与 CANN ABI 布局（`EslKernelArgs` 为 `KernelArgs` 别名）
- **`aicpu_bridge.h`**：AICPU glue（寄存器、shm sync、affinity、bridge API；含 core `dispatch.c` 引用）；`esl_aicpu_execute` 声明在 `esl_runtime.h`
- **`tools.h`**：fingerprint / 文件 I/O / dispatcher 偏移
- **`onboard_log.h`**：AICPU CANN 日志（`LOG_*` 对齐 `DLOG_*`；`LOG_INFO_V0`→debug）；见 `TOOLS_CALL_GRAPH.md`
