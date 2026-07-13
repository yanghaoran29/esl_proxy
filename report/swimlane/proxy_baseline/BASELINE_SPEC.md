# proxy_baseline 基线泳道图标准化构建说明

本文件归档两套理想基线泳道图的构建逻辑、绘制规则与约束条件，保证基线模型**可复现、可追溯**。生成工具：[`tools/rebuild_baselines.py`](../../../tools/rebuild_baselines.py)（自包含，弃用旧的 `zero_gap_baseline.py` / `baseline_swimlane.py`）。

## 0. 输入 / 输出

- **输入**：`report/swimlane/proxy/**/l2_swimlane_records.json` —— ESL proxy 在昇腾芯片上的**原始实测泳道图**（52 个配置，见 §5）。
- **输出**：
  - `proxy_baseline/scheme1_fixed_topo/<proxy相对路径镜像>/{l2_swimlane_records.json, l2_swimlane_trace.json}`
  - `proxy_baseline/scheme2_optimal/<proxy相对路径镜像>/{l2_swimlane_records.json, l2_swimlane_trace.json}`
  - `scheme1_diff_analysis.json` / `scheme2_diff_analysis.json` —— 逐配置的逐任务 + 总差
  - `DIFF_SUMMARY.md` —— 52 配置总差汇总表

## 1. 数据模型与时间单位

- `aicore_tasks` 每行 = 一个**子任务**：`[core_idx, task_id, reg_task_id, start_cycle, end_cycle]`。
- `core_idx < 24` → **aic** 核；`>= 24` → **aiv** 核（共 24 aic + 48 aiv = 72 核）。
- 一个**任务**（`task_id`）可含多个子任务（SPMD，分布在多核，共享同一 `task_id`）。
- 时钟 `clock_freq_hz = 50 MHz`，1 tick(cycle) = 20 ns。内部调度**全程以 tick 为单位**，`dur = end_cycle − start_cycle` 整数，**真实执行时长精确保留、零舍入漂移**；仅差值报告换算为 us（`us = ticks / 50`）。
- **cycle 原点偏移**：基线调度从 cycle 0 起算，但 `swimlane_converter` 以“最小**非零**时间戳”为渲染原点（字面 0 被排除）。若最早任务落在 cycle 0，渲染会错误地以第二早任务为原点、把首个依赖间隔压没（表现为 q/k/v_proj 与 rmsnorm 同在 0 时刻）。故落盘 records 时对所有 cycle 统一 `+BASELINE_CYCLE_ORIGIN`（=1,000,000），使真实最早事件成为最小非零戳，Perfetto 泳道图正确渲染；相对时序不变（converter 会减回原点），差值分析基于原点 0 的内存 records，不受影响。
- **任务完成语义**：任务的**全部子任务**完成后，后继任务才就绪（与 C 运行时 `g_predecessor_cnt` 计数归零一致，见 `src/algorithm/cutter.c`）。

## 2. DAG 依赖来源（关键：完整数学 DAG，非运行时裁剪版）

- 依赖由 orchestration 在 `add_predecessors()`（`include/algorithm/ring_buf.h`）声明。
- **重要修正**：该函数原有的 `succeed,...` 日志在两处裁剪（`target < g_min_uncomplete_task`、前置已 `COMPLETED`）**之后**才打印，因此 `succeed` 日志是**运行时裁剪后的残缺 DAG**——凡是"前置任务已提前完成"的依赖边都被丢弃（如 `q_proj(t5..t9) ← rmsnorm(t0)`）。若用它建基线，会让后继任务错误地在 t=0 与前置并行启动，得到偏短、不真实的基线。
- 修正方式：在两处裁剪**之前**新增一条 WORKER_LOG 日志 `depall,task_id,X,predecessor_id,Y`，记录**每一条声明的依赖边**。工具解析 `depall` 得到**完整数学 DAG**。
- 提取方式（CPU 功能仿真，**不需要 NPU**）：
  ```
  make CASE=<family>.h WORKER_LOG=1 [QWEN3_SPMD_TIER=<tier>] clean all
  WORKER_LOG=1 LOG_OUTPUT_MODE=0 ./bin/esl_proxy      # 生成 log/pto._thread_*.csv
  ```
  正则 `depall,task_id,(\d+),predecessor_id,(\d+)` 解析。
- DAG 只随 **(case家族, SPMD-tier)** 变化，与 dispatch-mode / lane 无关；`qwen3_dynamic_tensormap` 复用 `qwen3_dynamic_manual_scope` 的 DAG（任务图完全相同）。共约 7 次提取，缓存至 `tools/_case_dag_cache.json`（`--refresh-dag` 强制重提）。
- **对齐校验**：实测泳道的 `task_id` 集合与提取 DAG 的任务集合一致（非根任务全覆盖；根任务 t0 无 `depall` 边）。

## 3. 方案一：固定拓扑零调度间隔基线（`simulate_fixed_topo_zero_gap`）

**核心原则**：完全继承实测拓扑，仅消除调度空闲。

**构建规则 / 约束**：
1. 每个子任务**留在实测的原核**（泳道归属不变）。
2. 每核内子任务**保留实测执行顺序**（按实测 `start_cycle` 升序）。
3. 尊重完整 DAG 依赖：任务的**所有前置任务全部完成**后其子任务方可启动。
4. **调度间隔置零**：子任务启动时刻
   `start = max(所有前置任务完成时刻, 本核上一子任务完成时刻)`，无任何额外派发延迟。
5. 每个子任务**执行时长严格取实测 `end − start`，不改一分**。

**性质**：因保留每核顺序且间隔置零，每个子任务基线 `start ≤ 实测 start`（拓扑归纳），故 **基线 makespan ≤ 实测 makespan**。整体表现为"实测泳道图右侧任务向左平移、任务间无空隙"。

## 4. 方案二：依赖约束最优拓扑基线（`simulate_optimal_list_schedule`）

**核心原则**：仅 DAG 依赖为硬约束，无依赖任务自由排布以压缩 makespan。

**构建规则 / 约束**：
1. **唯一硬约束**：DAG 依赖——前置任务全部子任务完成后，后继才就绪，依赖间调度间隔为 0。
2. **无依赖任务自由重排 + 跨核迁移**：每个子任务**保留其核类型**（aic→任意空闲 aic，aiv→任意空闲 aiv），可放到该类型中**任意最早空闲的核**，不再受实测核归属与顺序约束。
3. **子任务数与每个子任务的真实执行时长均保留不变**。
4. 调度算法：事件驱动**贪心列表调度**——就绪任务按就绪时刻（`ready_time = max 前置完成时刻`）优先出队；任务内子任务按时长降序（LPT）依次分配给同类型中最早空闲的核，`start = max(ready_time, 该核空闲时刻)`。

**性质**：`方案二 makespan ≤ 方案一 makespan ≤ 实测 makespan`。多核 + 依赖下的 makespan 精确最小化为 NP-hard，本调度器给出**贪心近最优理论基线**（理论最优耗时下界的近似），用于评估"任务排布不合理 + 无效调度等待"的双重冗余。

## 5. 覆盖范围（52 个配置）

维度：`case`（4）× `dispatch-mode`（basic / double_buffer）× `lane`（顶层 / lane2_shared / spmd）× `SPMD-tier`（qwen3: tier0–4；paged: 无 tier）。

- `basic/<case>`、`double_buffer/<case>`（顶层，非 lane2）≡ **tier 0**（任务数与 lane2 tier0 一致校验通过）。
- `lane2_shared/{basic,double_buffer}/<case>/tier{0..4}`
- `spmd/{basic,double_buffer}/qwen3_*/tier{0..4}`
- `lane1` 仅有 `summary.txt`、无 `l2_swimlane_records.json`，跳过。

qwen3 各 tier 任务数：tier0=3096、tier1=1602、tier2=864、tier3=678、tier4=522（子任务行恒为 3576）；paged=1920。

## 6. 差值计算规则（逐任务 + 全流程总差）

以基线为基准，对每个配置：
- **逐任务**（`per_task[]`）：按 `task_id` 取该任务所有子任务的 min-start / max-end，输出实测与基线的 `start / end / span`（us）及 `start_delay_us = 实测start − 基线start`；附算子名 `func`（查 `case_task_func_maps.json` + `<family>_func_names.json`，17 个算子）。
- **全流程总差**：`total_diff_us = 实测 makespan − 基线 makespan`，`speedup_pct`。
- 方案一总差 = 纯调度空闲损耗；方案二总差 = 调度空闲 + 排布不优的双重冗余。

## 7. 不变量自校验（脚本内 assert）

- 子任务总数：基线 == 实测。
- 子任务时长**多重集**：基线 == 实测（逐 tick 精确）。
- 每 `(task_id, 核类型)` 的子任务数：基线 == 实测（方案二只换物理核、不改类型/数量）。
- makespan 序：`方案二 ≤ 方案一 ≤ 实测`。
- DAG 依赖：基线中每个任务 start ≥ 其所有前置任务 end（0 违反）。

## 8. 复现命令

```bash
cd esl_proxy
python3 tools/rebuild_baselines.py                 # 全部 52 配置，生成两套基线 + 差值
python3 tools/rebuild_baselines.py --only <substr> # 仅匹配路径子串的配置
python3 tools/rebuild_baselines.py --refresh-dag   # 忽略缓存重新提取 DAG
python3 tools/rebuild_baselines.py --no-trace      # 跳过 Perfetto trace（更快）
```
