# esl_proxy 基准测试报告

生成时间：2026-07-03（§5 调度开销基线于当日刷新；qwen3 泳道 2026-07-02；paged 泳道 2026-07-01）  
条件：`QWEN3_SPMD_TIER=0`（non-spmd），四样例全部 **PASS**

> 说明：第 2、3、4 节的所有指标均直接来自 `report/swimlane/proxy/<mode>/<case>/l2_swimlane_trace.json`
> （Perfetto 格式，仅 AICore View）。`span` 为全部 AICore kernel 切片的
> `max(ts+dur) - min(ts)`，`ktasks/s = task_cnt / span`。trace 中不含 runner
> `wall_ns`，故本报告以 **span** 作为上板时间指标（替代旧版的 wall 列）。

> 第 5 节调度开销：`actual` 同上；`baseline` 来自 `report/swimlane/proxy_baseline/<mode>/<case>/`
> （`tools/baseline_swimlane.py` 生成，不覆盖 `proxy/` 目录）。**每个 task 的 kernel 时长**
> 取自对应 `proxy/` 实际上板泳道 slice 的 `end−start`（qwen3 优先 `l2_swimlane_records.json`，
> paged 仅有 trace 时从 `l2_swimlane_trace.json` 的 `dur` 读取）；span 对比以 records 为准。

---

## 1. Sim（主机 instant AICore）

方法：每样例 **10 次**运行取**中位数**（`tools/run_sim_benchmark.sh`）  
配置：`SIM_AICORE=instant`，`DISPATCH=basic`

| 样例 | task_cnt | Orch TP (MTasks/s) | Sched TP (MTasks/s) | Sched 耗时 (ms) |
|------|---------:|-------------------:|--------------------:|----------------:|
| qwen3_dynamic_manual_scope | 3096 | 3.25 | 2.48 | 1.26 |
| qwen3_dynamic_tensormap | 3096 | 1.55 | 1.47 | 2.12 |
| paged_attention_unroll | 1920 | 2.32 | 2.25 | 0.85 |
| paged_attention_unroll_manual_scope | 1920 | 3.75 | 3.61 | 0.53 |

原始数据：[sim_benchmark.json](sim_benchmark.json)

---

## 2. 上板 NPU — 单缓冲（basic dispatch）

命令：`bash tools/run_onboard.sh --all-cases --swimlane --npu`

泳道图目录：`report/swimlane/proxy/basic/<case>/`

| 样例 | task_cnt | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 5.37 | 576.81 | [trace](swimlane/proxy/basic/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 5.34 | 579.58 | [trace](swimlane/proxy/basic/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 2.92 | 658.58 | [trace](swimlane/proxy/basic/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 2.89 | 663.74 | [trace](swimlane/proxy/basic/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

---

## 3. 上板 NPU — 双缓冲（double_buffer dispatch）

命令：`bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu`

泳道图目录：`report/swimlane/proxy/double_buffer/<case>/`

| 样例 | task_cnt | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 4.86 | 637.58 | [trace](swimlane/proxy/double_buffer/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 4.86 | 637.62 | [trace](swimlane/proxy/double_buffer/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 2.46 | 781.11 | [trace](swimlane/proxy/double_buffer/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 2.39 | 804.72 | [trace](swimlane/proxy/double_buffer/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

---

## 4. 单缓冲 vs 双缓冲（上板 span 时间）

| 样例 | basic span (ms) | double_buffer span (ms) | 变化 |
|------|----------------:|------------------------:|-----:|
| qwen3_dynamic_manual_scope | 5.37 | 4.86 | **-9.5%** |
| qwen3_dynamic_tensormap | 5.34 | 4.86 | **-9.1%** |
| paged_attention_unroll | 2.92 | 2.46 | **-15.7%** |
| paged_attention_unroll_manual_scope | 2.89 | 2.39 | **-17.5%** |

双缓冲在四个样例上 span 时间均有明显下降，收益从约 9% 到 17.5%；paged_attention_unroll_manual_scope 收益最大（约 17.5%）。

---

## 5. 调度开销（顺序保持基线 vs 实际上板）

在 **24 AIC + 48 AIV（72 worker）** 资源约束下，估算「零 AICPU 下发延迟」时的理想 span，并与 §2/§3 实际上板 span 对比。**调度开销 = actual − baseline**。

### 基线模型（`tools/baseline_swimlane.py`）

| 项 | 说明 |
|----|------|
| **依赖** | 编排数学逻辑（`add_predecessors` / tensormap WAR·WAW），经 sim `WORKER_LOG` CSV 还原 DAG |
| **时长** | **proxy 实际上板泳道**中每个 task 的 kernel 执行时间（`records` 的 tick 差或 `trace` 的 `dur`），非编排名义 `dur` |
| **资源** | 24 AIC（worker 0–23）+ 48 AIV（worker 24–71），每 worker 同时只跑 1 个 kernel |
| **顺序** | **保持实际上板泳道中各 core 的任务先后顺序**（basic 对 basic、double_buffer 对 double_buffer） |
| **启动** | `start(T) = max(前驱全部完成, 同 core 上一任务完成)`，无额外 dispatch 延迟 |

基线泳道：`report/swimlane/proxy_baseline/<mode>/<case>/l2_swimlane_trace.json`（Perfetto）。原始数值：[baseline_sched_analysis.json](baseline_sched_analysis.json)。

### 开销汇总

| 样例 | 模式 | 基线 span (ms) | 实际 span (ms) | 开销 (ms) | 开销 (%) | 基线泳道 |
|------|------|---------------:|---------------:|----------:|---------:|----------|
| qwen3_dynamic_manual_scope | basic | 4.04 | 5.37 | **+1.33** | +32.8% | [trace](swimlane/proxy_baseline/basic/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_manual_scope | double_buffer | 4.17 | 4.86 | **+0.69** | +16.5% | [trace](swimlane/proxy_baseline/double_buffer/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | basic | 4.03 | 5.34 | **+1.32** | +32.7% | [trace](swimlane/proxy_baseline/basic/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | double_buffer | 4.07 | 4.86 | **+0.79** | +19.4% | [trace](swimlane/proxy_baseline/double_buffer/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | basic | 2.16 | 2.92 | **+0.75** | +34.9% | [trace](swimlane/proxy_baseline/basic/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll | double_buffer | 2.26 | 2.46 | **+0.19** | +8.6% | [trace](swimlane/proxy_baseline/double_buffer/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | basic | 2.16 | 2.89 | **+0.73** | +34.0% | [trace](swimlane/proxy_baseline/basic/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | double_buffer | 2.23 | 2.39 | **+0.15** | +6.9% | [trace](swimlane/proxy_baseline/double_buffer/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

### 观察

- **basic 开销约 +33% ~ +35%**（qwen3 +1.32~1.33 ms，paged +0.73~0.75 ms）：在 core 顺序与 kernel 时长均取自实际上板泳道的前提下，剩余时间主要来自 AICPU dispatch 环、ring 背压、handshake 等路径。
- **double_buffer 将开销降至 +6.9% ~ +19.4%**（qwen3 +0.69~0.79 ms，paged +0.15~0.19 ms），与 §4 双缓冲 span 收益一致。
- qwen3 绝对开销大于 paged，因 task 数更多（3096 vs 1920）、依赖图更密；paged double_buffer 开销已接近 **+6%**，说明双缓冲 dispatch 模型已覆盖大部分可消除的调度延迟。

---

## 6. paged attention：proxy vs simpler 泳道对比

把 paged_attention 两个变体的 **proxy**（esl_proxy 上板，本报告 §2/§3）与 **simpler**（真实 V200 runtime 上板）泳道放在一起对比，看 proxy 模型对真实运行时的复现程度。

- **proxy**：`report/swimlane/proxy/<mode>/<case>/l2_swimlane_trace.json`（Perfetto，仅 AICore View）。
- **simpler**：`report/swimlane/simpler/schedN/*_merged_swimlane.json`（Perfetto，含 AICore / AICPU / Scheduler / Orchestrator 多泳道）；`schedN` 为 AICPU 调度线程数，`sched2`=2 线程、`sched3`=3 线程。
- 指标统一为 **AICore span**：只取 AICore View 泳道全部 kernel 切片的 `max(ts+dur) - min(ts)`。四份 simpler 泳道均 `1920` tasks、golden **PASS**，与 proxy 同 case、同 batch=480、N_UNROLL=64。

| 变体 | proxy basic | proxy double_buffer | simpler sched2（2 线程） | simpler sched3（3 线程） |
|------|-----------:|--------------------:|------------------------:|------------------------:|
| paged_attention_unroll | 2.92 | 2.46 | 2.40 | 2.20 |
| paged_attention_unroll_manual_scope | 2.89 | 2.39 | 2.34 | 2.19 |

（单位 ms，AICore span）

simpler 泳道：
[paged_unroll sched2](swimlane/simpler/sched2/paged_unroll_sched2_merged_swimlane.json) ·
[paged_manual sched2](swimlane/simpler/sched2/paged_manual_sched2_merged_swimlane.json) ·
[paged_unroll sched3](swimlane/simpler/sched3/paged_unroll_sched3_merged_swimlane.json) ·
[paged_manual sched3](swimlane/simpler/sched3/paged_manual_sched3_merged_swimlane.json)

观察：

- **proxy double_buffer ≈ simpler sched2**：两变体上 proxy 双缓冲仅比真实 2 线程调度慢约 **+2.3% ~ +2.4%**（unroll 2.46 vs 2.40、manual 2.39 vs 2.34），说明「2 路 outstanding 的双缓冲 dispatch」很好地复现了真实 runtime 双调度线程下的 AICore 占用与并行度。
- **simpler sched3（3 线程）最快**：2.20 / 2.19 ms，比 proxy double_buffer 还低约 **9% ~ 12%**。第 3 个调度线程带来的额外并行,proxy 当前「2 路 outstanding」模型尚未覆盖。
- **proxy basic（单缓冲）最慢**：2.92 / 2.89 ms，比 simpler sched3 高约 **+32%**，与 §4 中单缓冲相对双缓冲的差距一致——单缓冲是 proxy 侧的下界参考，而非真实运行时的表现。
- manual_scope 与 auto(unroll) 两变体在各档配置下几乎同速（差 ≤3%），符合 paged_attention 依赖链简单（每 batch 一条 qk→sf→pv→online）、手动/自动依赖等价的预期。

---

## 7. 泳道图查看

泳道文件均为 Perfetto 格式，拖入 https://ui.perfetto.dev/ 查看。proxy 每个 case 目录仅保留 **`l2_swimlane_trace.json`**（仅 AICore View）；simpler 为 runtime 直出的 **`*_merged_swimlane.json`**（含 AICPU 泳道）。§5 基线泳道在 `proxy_baseline/` 下，与 `proxy/` 并列、互不覆盖。

```
report/swimlane/
├── proxy/                                # esl_proxy 上板（l2_swimlane_trace.json）
│   ├── basic/
│   │   ├── qwen3_dynamic_manual_scope/
│   │   ├── qwen3_dynamic_tensormap/
│   │   ├── paged_attention_unroll/
│   │   └── paged_attention_unroll_manual_scope/
│   └── double_buffer/
│       ├── qwen3_dynamic_manual_scope/
│       ├── qwen3_dynamic_tensormap/
│       ├── paged_attention_unroll/
│       └── paged_attention_unroll_manual_scope/
├── proxy_baseline/                       # §5 顺序保持基线（l2_swimlane_trace.json）
│   ├── basic/
│   │   └── <case>/                       # 同上四个 case
│   └── double_buffer/
│       └── <case>/
└── simpler/                              # 真实 V200 runtime（*_merged_swimlane.json）
    ├── sched2/                           # 2 调度线程
    │   ├── paged_unroll_sched2_merged_swimlane.json
    │   └── paged_manual_sched2_merged_swimlane.json
    └── sched3/                           # 3 调度线程
        ├── paged_unroll_sched3_merged_swimlane.json
        └── paged_manual_sched3_merged_swimlane.json
```

---

## 8. 复现命令

```bash
# Sim 基准（10 次中位数）
bash tools/run_sim_benchmark.sh

# 上板 basic + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --npu

# 上板 double_buffer + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu

# §5 调度开销基线泳道（需先有 proxy/ 实际上板泳道；不覆盖 proxy/）
python3 tools/baseline_swimlane.py
```
