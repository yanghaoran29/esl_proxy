# esl_proxy 基准测试报告

生成时间：2026-07-01（泳道 trace 于该日由 `tools/swimlane_trace.py` 重新转换）  
条件：`QWEN3_SPMD_TIER=0`（non-spmd），四样例全部 **PASS**

> 说明：第 2、3、4 节的所有指标均直接来自 `report/swimlane/<mode>/<case>/l2_swimlane_trace.json`
> （Perfetto 格式，仅 AICore View）。`span` 为全部 AICore kernel 切片的
> `max(ts+dur) - min(ts)`，`ktasks/s = task_cnt / span`。trace 中不含 runner
> `wall_ns`，故本报告以 **span** 作为上板时间指标（替代旧版的 wall 列）。

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

泳道图目录：`report/swimlane/basic/<case>/`

| 样例 | task_cnt | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 4.16 | 744.74 | [trace](swimlane/basic/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 4.07 | 761.46 | [trace](swimlane/basic/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 2.92 | 658.58 | [trace](swimlane/basic/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 2.89 | 663.74 | [trace](swimlane/basic/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

---

## 3. 上板 NPU — 双缓冲（double_buffer dispatch）

命令：`bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu`

泳道图目录：`report/swimlane/double_buffer/<case>/`

| 样例 | task_cnt | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 3.69 | 838.31 | [trace](swimlane/double_buffer/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 3.58 | 864.19 | [trace](swimlane/double_buffer/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 2.46 | 781.11 | [trace](swimlane/double_buffer/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 2.39 | 804.72 | [trace](swimlane/double_buffer/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

---

## 4. 单缓冲 vs 双缓冲（上板 span 时间）

| 样例 | basic span (ms) | double_buffer span (ms) | 变化 |
|------|----------------:|------------------------:|-----:|
| qwen3_dynamic_manual_scope | 4.16 | 3.69 | **-11.2%** |
| qwen3_dynamic_tensormap | 4.07 | 3.58 | **-11.9%** |
| paged_attention_unroll | 2.92 | 2.46 | **-15.7%** |
| paged_attention_unroll_manual_scope | 2.89 | 2.39 | **-17.5%** |

双缓冲在四个样例上 span 时间均有明显下降，收益从约 11% 到 17%；paged_attention_unroll_manual_scope 收益最大（约 17.5%）。

---

## 5. 泳道图查看

每个 case 目录仅保留 **`l2_swimlane_trace.json`**（Perfetto 格式，拖入 https://ui.perfetto.dev/）。

```
report/swimlane/
├── basic/
│   ├── qwen3_dynamic_manual_scope/
│   ├── qwen3_dynamic_tensormap/
│   ├── paged_attention_unroll/
│   └── paged_attention_unroll_manual_scope/
└── double_buffer/
    ├── qwen3_dynamic_manual_scope/
    ├── qwen3_dynamic_tensormap/
    ├── paged_attention_unroll/
    └── paged_attention_unroll_manual_scope/
```

---

## 6. 复现命令

```bash
# Sim 基准（10 次中位数）
bash tools/run_sim_benchmark.sh

# 上板 basic + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --npu

# 上板 double_buffer + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu
```
