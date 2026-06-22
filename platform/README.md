# esl_proxy 上板运行

在 **esl_proxy 仓库内**自建 AICPU 三线程运行时（cutter / dispatch / orchestrator），Host 启动暂借 simpler 的 DeviceRunner。

## 目录

- `platform/a2a3/aicpu/` — AICPU kernel 入口、三线程 executor、AICore bridge
- `platform/a2a3/aicore/` — `aicore_executor`（收到任务立即 FIN）、`kernel.cpp`、`fake_kernel`
- `platform/cmake/` — 构建脚本
- `tools/run_onboard.py` — 上板 smoke 启动
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

## 构建 fake_kernel（可选，ChipCallable 子 kernel stub）

```bash
bash platform/cmake/build_fake_kernel.sh
# 产物: build/onboard/aicore/fake_kernel.o
```

## 上板 smoke（需 NPU + pip install -e simpler）

```bash
pip install --no-build-isolation -e ../simpler/.[test]

# 推荐经 task-submit 占卡（内层命令不要带 -d，用 $TASK_DEVICE）
task-submit --device auto --max-time 30 --timeout 30 \
  --env ASCEND_HOME_PATH --env PTO_ISA_ROOT --env PATH --env LD_LIBRARY_PATH \
  --run 'source "$ASCEND_HOME_PATH/bin/setenv.bash" && \
export SIMPLER_ROOT=/path/to/simpler && \
export PYTHONPATH=/path/to/simpler/python:$PYTHONPATH && \
cd /path/to/esl_proxy && \
bash platform/cmake/build_aicpu.sh && \
bash platform/cmake/build_aicore.sh && \
python3 tools/run_onboard.py -p a2a3 --skip-build'
```

`run_onboard.py` 会优先读取 task-submit 注入的 `$TASK_DEVICE`。

## Host 仿真（不变）

```bash
cd esl_proxy && make run
```

## 三线程模型

| AICPU idx | 角色 |
|-----------|------|
| 0 | Cutter — `cutter_loop_run()` |
| 1 | Dispatch — `dispatch_loop_run(0)` |
| 2 | Orchestrator — `smoke_orch.c` / `aicpu_orchestration_entry()` |

编译 AICPU 时定义 `ESL_PROXY_ONBOARD`，dispatch 通过 `aicore_bridge` 下发（M1 仍即时完成，M2 接 FIN 轮询）。

## 构建说明

- CMake 入口：`platform/cmake/aicpu/CMakeLists.txt`（不编译 simpler 自带 `kernel.cpp`）
- 需设置 `SIMPLER_ROOT`（默认同级 `../simpler`）与 `ASCEND_HOME_PATH`
