# esl_proxy 上板运行

在 **esl_proxy 仓库内**自建 AICPU 运行时（orchestration + cutter + dispatch）。
**自包含**：从 simpler 移植来的最小平台运行时已内置于 `esl_proxy/{include,src}/onboard/`，
无需外部 `../simpler`，也无需设置 `SIMPLER_ROOT` 环境变量。

## 目录

所有上板代码都在核心工程内（无子文件夹，扁平存放）：

- `esl_proxy/include/onboard/` — 全部上板头文件（esl 自有 + glue + 移植平台头，扁平）
- `esl_proxy/src/onboard/` — 全部上板源文件（扁平）：
  - AICPU kernel: `kernel.cpp`、`executor.cpp`、`aicore_bridge*.c/.cpp`、`aicore_handshake.cpp`、`platform_init.c`、`smoke_orch.c`、移植平台源（`platform_regs.cpp`/`device_*.cpp` 等）
  - AICore: `aicore_kernel.cpp`、`aicore_executor.cpp`（收到任务立即 FIN）、`fake_kernel.cpp`
  - Host: `onboard_runner.cpp`、`aicpu_loader.cpp`、`aicore_launcher.cpp`、`host_regs.cpp`、`elf_fingerprint.cpp`
  - Dispatcher: `aicpu_dispatcher.cpp`
- `platform/cmake/` — 仅构建脚本与 CMakeLists（aicpu/aicore/host）
- `tools/run_onboard.sh` — 上板 smoke 启动

> 三个构建目标共用 `src/onboard` 扁平目录，故各自在构建脚本/CMake 中**显式列出**自己的源文件（AICPU 取除 aicore/host/dispatcher 之外的全部，再加核心调度 `cutter/dispatch/shm/log.c`）。
- `tests/onboard/smoke/smoke_orch.h` — 4 task 链 smoke 编排

## 构建 AICPU SO

```bash
source $ASCEND_HOME_PATH/bin/setenv.bash
bash platform/cmake/build_aicpu.sh
# 产物: build/onboard/aicpu/libaicpu_kernel.so
```

## 构建 AICore runtime（fake-FIN 调度循环）

```bash
bash platform/cmake/build_aicore.sh
# 产物: build/onboard/aicore/aicore_kernel.o
```

`aicore_execute` 收到 `DATA_MAIN_BASE` 新 task_id 后写 ACK + FIN，不执行真实 kernel。

## 上板 smoke（仅需 NPU，无 simpler 依赖）

`tools/run_onboard.sh` 构建三件产物（AICPU `.so` / AICore `.o` / host runner）并运行。
经 task-submit 占卡（内层用 `$TASK_DEVICE`，不要写死 `-d`）：

```bash
task-submit --device auto --max-time 30 --timeout 30 \
  --env ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0 \
  --run 'source "$ASCEND_HOME_PATH/bin/setenv.bash" && \
cd /path/to/esl_proxy && \
bash tools/run_onboard.sh -d $TASK_DEVICE'
# 已构建过可加 --skip-build
```

成功输出 `esl_proxy onboard smoke: OK device_wall_ns=...`。
`run_onboard.sh` 会优先读取 task-submit 注入的 `$TASK_DEVICE`。

## Host 仿真（不变）

```bash
cd esl_proxy && make run
```

## 调度模型（三 AICPU 线程）

CANN 拉起 `ESL_PROXY_AICPU_THREAD_NUM=3` 个 AICPU 线程，按进入 `esl_aicpu_execute` 的顺序分配角色（与 host 仿真 pthread 模型对齐）：

| idx | 角色 | 入口 |
|-----|------|------|
| 0 | Cutter | `cutter_loop_run()` |
| 1 | Dispatch | `dispatch_loop_run(0)` + `aicore_bridge` 寄存器下发 / FIN 轮询 |
| 2 | Orchestrator | `aicpu_orchestration_entry()` → `esl_signal_orch_done()` |

AICPU 核间非 cache 一致，cutter / dispatch / orch 共享队列与计数器通过 `onboard_crosscore_sync`（队列自旋锁 + `cache_flush` / `cache_invalidate`）保持可见性。编排期间 cutter 与 dispatch **并行**运行（与 `main.c` 一致）。

## 构建说明

- CMake 入口：`platform/cmake/{aicpu,aicore,host}/CMakeLists.txt`
- 仅需 `ASCEND_HOME_PATH`（无 `SIMPLER_ROOT`）。所有上板代码扁平存放于
  `esl_proxy/include/onboard`（头）与 `esl_proxy/src/onboard`（源）。
