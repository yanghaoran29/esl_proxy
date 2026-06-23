# 上板文件合并可行性分析

**状态：已执行（2026-06-23）** — 激进合并计划已完成，见下方目标态。

## 目标态（当前）

| 构建目标 | onboard 源文件 | 行数约 |
|----------|----------------|--------|
| AICPU `libaicpu_kernel.so` | `aicpu_kernel.c` | ~1380 |
| AICore `aicore_kernel.o` | `aicore_kernel.cpp`（ccec） | ~125 |
| Host `esl_onboard_runner` | `host_runner.c` | ~1140 |
| Dispatcher `libsimpler_aicpu_dispatcher.so` | `aicpu_dispatcher.c` | ~250 |

| 指标 | C 化后 | 合并后 |
|------|--------|--------|
| onboard 实现源 | 17 `.c` + 3 `.cpp` | **4** |
| config 类头 | 4+ | **1**（`onboard_config.h`） |
| `include/onboard` | 26 | **6** |

## 历史约束（合并前）

- 4 个构建目标用不同编译器，跨目标不可合并
- C 化后 AICPU 全 C，消除了 C/C++ 同 TU 障碍
- `elf_fingerprint` 在 Host 与 Dispatcher 各有一份 static 副本（不同链接产物）

## 合并分组（已执行）

| 组 | 合并为 | 状态 |
|----|--------|------|
| config 头 | `onboard_config.h` | 完成 |
| AICPU 11×`.c` + case_orch | `aicpu_kernel.c` | 完成 |
| AICore 3×`.cpp` | `aicore_kernel.cpp` | 完成 |
| Host 5×`.c` | `host_runner.c` | 完成 |
| Dispatcher + elf | `aicpu_dispatcher.c` | 完成 |

## 仍保留的跨 TU 头（6 个）

`onboard_config.h`、`esl_runtime.h`、`kernel_args.h`、`aicpu_bridge.h`、`aicore.h`、`tools.h`

已并入 `aicpu_bridge.h`：`platform_regs.h`、`onboard_shm_sync.h`、`platform_aicpu_affinity.h`  
已并入 `kernel_args.h`：`esl_kernel_args.h`  
已并入 `tools.h`：`log.h`（AICPU CANN 日志；Host CSV 仍为 `include/log.h`）  
已从 `tools.h` 移除重复：`get_sys_cnt_aicpu`（统一用 `onboard_config.h` 的 `esl_onboard_sys_cnt`）

## 验证

```bash
bash tools/run_onboard.sh -d $DEVICE
# 通过：completed_cnt=1920, esl_proxy onboard: OK
```
