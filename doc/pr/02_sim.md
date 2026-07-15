# PR-B：Sim 后端

相对基线：`pr/A-board-bringup`。

---

## 1. 为什么修改

PR-A 只打通 **NPU onboard**。无板或日常回归时，需要在主机上用同一套 algorithm（orch/cutter/dispatch + fake kernel）验证 **paged attention + basic dispatch**。

本 PR 补齐 **sim 后端**：用 pthread 模拟 AICPU/AICore 与寄存器门铃。

---

## 2. 怎么修改

| 纳入 | 说明 |
|------|------|
| `include/platform/sim/**`、`src/platform/sim/**` | 主机侧 sim HAL、device runner、核入口包装 |
| `tools/run_sim_benchmark.sh`（若有） | 一键 sim 冒烟 |
| Makefile | 链 sim 源 |

- sim 的 `aicore_execute_wrapper` 与 PR-A 三参数 `aicore_execute` 对齐；
- 寄存器/COND 用主机内存模拟，dispatch 走 prepare/`wmb`/publish。

---

## 3. 与 simpler 的对应

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| `platform/sim/**` | `simpler/src/a2a3/platform/sim/{host,aicore,aicpu}/` |
| sim Host device runner / pthread AICore | `platform/sim/host/device_runner.{h,cpp}`；共享基类 `src/common/platform/sim/host/device_runner_base.cpp` |
| sim AICore 入口 → `aicore_execute` | `platform/sim/aicore/kernel.cpp`、`inner_kernel.h` → runtime `aicore_executor` |
| 与 onboard 共用 algorithm | simpler：`runtime/` 与 `platform/sim` / `platform/onboard` 分离 |

a5 镜像路径：`simpler/src/a5/platform/sim/…`。
