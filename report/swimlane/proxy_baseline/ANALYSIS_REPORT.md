# proxy_baseline 双基线泳道图构建与实测耗时差值分析报告

> 自动生成，数据源：`scheme1_diff_analysis.json` / `scheme2_diff_analysis.json`（权威数值）。
> 构建规则与约束详见 [`BASELINE_SPEC.md`](BASELINE_SPEC.md)。单位 us。

## 1. 背景与方法

基于 `report/swimlane/proxy/` 下 52 个**原始实测泳道图**，构建两套理想基线，消除任务间调度空闲、仅保留任务真实执行时长，量化实测相对基线的耗时延迟。

- **方案一 · 固定拓扑零调度间隔**：完全继承实测拓扑（泳道归属、每核顺序、DAG 依赖），仅将“前置完成→后置启动”的调度间隔置零，整体左移。`start = max(所有前置完成, 本核空闲)`。

- **方案二 · 依赖约束最优拓扑**：仅 DAG 依赖为硬约束；无依赖任务在同类型核（aic↔aic、aiv↔aiv）自由重排 + 跨核迁移，HEFT 临界路径优先贪心列表调度最小化 makespan。

两套基线均**保留每子任务真实执行时长与子任务数不变**。`Δ = 实测 makespan − 基线 makespan`，即实测相对基线的耗时延迟；方案一 Δ 为纯调度空闲损耗，方案二 Δ 为“调度空闲 + 排布不优”的双重冗余。

> **DAG 完整性**：依赖取自 orchestration 声明的**完整数学 DAG**（`add_predecessors` 裁剪前的 `depall` 日志），而非运行时裁剪后的 `succeed` 日志——后者会丢弃“前置已提前完成”的依赖边（如 q/k/v_proj←rmsnorm），导致后继任务在基线中错误地提前启动。

> **泳道图渲染**：基线 records 已对 cycle 统一施加正偏移（`BASELINE_CYCLE_ORIGIN`），规避 `swimlane_converter` “最小非零戳为原点”导致首个依赖间隔被压没的问题；经全 104 个 trace 校验，**依赖违反 0 处**、trace makespan 与 records makespan 完全一致。

## 2. 全流程总差汇总（全部 52 配置）

### 2.1 顶层 basic / double_buffer (≡tier0)

| config | 实测 | 方案一 | Δ1 | Δ1% | 方案二 | Δ2 | Δ2% |
|---|--:|--:|--:|--:|--:|--:|--:|
| basic/paged_attention_unroll | 2170.4 | 2133.6 | 36.8 | 1.7% | 2102.7 | 67.6 | 3.1% |
| basic/paged_attention_unroll_manual_scope | 2167.2 | 2134.5 | 32.8 | 1.5% | 2099.0 | 68.2 | 3.1% |
| basic/qwen3_dynamic_manual_scope | 5184.2 | 4082.2 | 1102.0 | 21.3% | 3930.5 | 1253.6 | 24.2% |
| basic/qwen3_dynamic_tensormap | 4601.1 | 4136.7 | 464.4 | 10.1% | 3930.7 | 670.3 | 14.6% |
| double_buffer/paged_attention_unroll | 2504.5 | 2290.8 | 213.6 | 8.5% | 2095.8 | 408.7 | 16.3% |
| double_buffer/paged_attention_unroll_manual_scope | 2443.8 | 2212.2 | 231.6 | 9.5% | 2098.9 | 344.9 | 14.1% |
| double_buffer/qwen3_dynamic_manual_scope | 5360.7 | 4344.0 | 1016.6 | 19.0% | 4017.1 | 1343.6 | 25.1% |
| double_buffer/qwen3_dynamic_tensormap | 4878.7 | 4103.9 | 774.8 | 15.9% | 3955.6 | 923.1 | 18.9% |

### 2.2 lane2_shared

| config | 实测 | 方案一 | Δ1 | Δ1% | 方案二 | Δ2 | Δ2% |
|---|--:|--:|--:|--:|--:|--:|--:|
| lane2_shared/basic/paged_attention_unroll/tier0 | 2178.2 | 2135.9 | 42.3 | 1.9% | 2111.0 | 67.2 | 3.1% |
| lane2_shared/basic/paged_attention_unroll_manual_scope/tier0 | 2166.5 | 2126.3 | 40.1 | 1.9% | 2107.1 | 59.4 | 2.7% |
| lane2_shared/basic/qwen3_dynamic_manual_scope/tier0 | 4807.2 | 4247.4 | 559.8 | 11.6% | 3943.8 | 863.4 | 18.0% |
| lane2_shared/basic/qwen3_dynamic_manual_scope/tier1 | 4367.4 | 4066.2 | 301.2 | 6.9% | 3961.5 | 405.9 | 9.3% |
| lane2_shared/basic/qwen3_dynamic_manual_scope/tier2 | 4475.4 | 4064.9 | 410.5 | 9.2% | 3927.6 | 547.8 | 12.2% |
| lane2_shared/basic/qwen3_dynamic_manual_scope/tier3 | 4315.7 | 4017.4 | 298.3 | 6.9% | 3968.6 | 347.1 | 8.0% |
| lane2_shared/basic/qwen3_dynamic_manual_scope/tier4 | 4338.2 | 4055.9 | 282.2 | 6.5% | 3964.5 | 373.6 | 8.6% |
| lane2_shared/basic/qwen3_dynamic_tensormap/tier0 | 4467.1 | 4088.6 | 378.5 | 8.5% | 3957.3 | 509.8 | 11.4% |
| lane2_shared/basic/qwen3_dynamic_tensormap/tier1 | 4423.6 | 4095.4 | 328.3 | 7.4% | 3961.3 | 462.4 | 10.5% |
| lane2_shared/basic/qwen3_dynamic_tensormap/tier2 | 4324.9 | 4002.7 | 322.2 | 7.4% | 3933.5 | 391.5 | 9.1% |
| lane2_shared/basic/qwen3_dynamic_tensormap/tier3 | 4476.1 | 4125.7 | 350.3 | 7.8% | 3965.8 | 510.2 | 11.4% |
| lane2_shared/basic/qwen3_dynamic_tensormap/tier4 | 4433.9 | 4058.1 | 375.8 | 8.5% | 3954.5 | 479.4 | 10.8% |
| lane2_shared/double_buffer/paged_attention_unroll/tier0 | 2180.2 | 2145.5 | 34.7 | 1.6% | 2100.2 | 80.0 | 3.7% |
| lane2_shared/double_buffer/paged_attention_unroll_manual_scope/tier0 | 2172.3 | 2135.5 | 36.8 | 1.7% | 2106.4 | 65.9 | 3.0% |
| lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier0 | 5108.3 | 4215.9 | 892.4 | 17.5% | 4011.8 | 1096.5 | 21.5% |
| lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier1 | 4465.9 | 4032.7 | 433.2 | 9.7% | 3967.7 | 498.2 | 11.2% |
| lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier2 | 4506.9 | 4138.3 | 368.5 | 8.2% | 4035.1 | 471.8 | 10.5% |
| lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier3 | 4339.9 | 4034.6 | 305.2 | 7.0% | 3922.1 | 417.7 | 9.6% |
| lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier4 | 4325.9 | 4049.6 | 276.3 | 6.4% | 3947.9 | 378.0 | 8.7% |
| lane2_shared/double_buffer/qwen3_dynamic_tensormap/tier0 | 4629.5 | 4125.3 | 504.2 | 10.9% | 3931.9 | 697.6 | 15.1% |
| lane2_shared/double_buffer/qwen3_dynamic_tensormap/tier1 | 4509.7 | 4166.0 | 343.7 | 7.6% | 3985.6 | 524.2 | 11.6% |
| lane2_shared/double_buffer/qwen3_dynamic_tensormap/tier2 | 4339.6 | 4070.5 | 269.1 | 6.2% | 3995.5 | 344.1 | 7.9% |
| lane2_shared/double_buffer/qwen3_dynamic_tensormap/tier3 | 4374.3 | 4025.7 | 348.6 | 8.0% | 3952.2 | 422.1 | 9.7% |
| lane2_shared/double_buffer/qwen3_dynamic_tensormap/tier4 | 4283.7 | 4052.5 | 231.2 | 5.4% | 3963.1 | 320.6 | 7.5% |

### 2.3 spmd (lane 单/共享外)

| config | 实测 | 方案一 | Δ1 | Δ1% | 方案二 | Δ2 | Δ2% |
|---|--:|--:|--:|--:|--:|--:|--:|
| spmd/basic/qwen3_dynamic_manual_scope/tier0 | 5236.4 | 4079.4 | 1157.1 | 22.1% | 3944.8 | 1291.6 | 24.7% |
| spmd/basic/qwen3_dynamic_manual_scope/tier1 | 5292.1 | 4159.5 | 1132.6 | 21.4% | 4023.7 | 1268.4 | 24.0% |
| spmd/basic/qwen3_dynamic_manual_scope/tier2 | 5103.7 | 4113.2 | 990.5 | 19.4% | 3904.0 | 1199.7 | 23.5% |
| spmd/basic/qwen3_dynamic_manual_scope/tier3 | 4915.0 | 4011.8 | 903.2 | 18.4% | 3874.6 | 1040.5 | 21.2% |
| spmd/basic/qwen3_dynamic_manual_scope/tier4 | 4975.5 | 4148.9 | 826.6 | 16.6% | 3947.8 | 1027.7 | 20.7% |
| spmd/basic/qwen3_dynamic_tensormap/tier0 | 5093.2 | 4042.5 | 1050.7 | 20.6% | 3958.8 | 1134.5 | 22.3% |
| spmd/basic/qwen3_dynamic_tensormap/tier1 | 5256.0 | 4314.7 | 941.3 | 17.9% | 3950.5 | 1305.5 | 24.8% |
| spmd/basic/qwen3_dynamic_tensormap/tier2 | 4946.0 | 4128.3 | 817.6 | 16.5% | 3981.9 | 964.1 | 19.5% |
| spmd/basic/qwen3_dynamic_tensormap/tier3 | 4919.0 | 4117.5 | 801.5 | 16.3% | 3962.1 | 956.9 | 19.5% |
| spmd/basic/qwen3_dynamic_tensormap/tier4 | 4906.3 | 4026.5 | 879.8 | 17.9% | 3941.6 | 964.7 | 19.7% |
| spmd/double_buffer/qwen3_dynamic_manual_scope/tier0 | 5134.0 | 4116.7 | 1017.3 | 19.8% | 3900.7 | 1233.3 | 24.0% |
| spmd/double_buffer/qwen3_dynamic_manual_scope/tier1 | 5077.6 | 4189.8 | 887.8 | 17.5% | 3956.8 | 1120.8 | 22.1% |
| spmd/double_buffer/qwen3_dynamic_manual_scope/tier2 | 4846.6 | 4069.6 | 777.0 | 16.0% | 3881.0 | 965.6 | 19.9% |
| spmd/double_buffer/qwen3_dynamic_manual_scope/tier3 | 4863.5 | 4111.0 | 752.5 | 15.5% | 3964.6 | 898.9 | 18.5% |
| spmd/double_buffer/qwen3_dynamic_manual_scope/tier4 | 4946.5 | 4177.8 | 768.7 | 15.5% | 3948.8 | 997.7 | 20.2% |
| spmd/double_buffer/qwen3_dynamic_tensormap/tier0 | 4889.8 | 4110.2 | 779.6 | 15.9% | 3945.0 | 944.8 | 19.3% |
| spmd/double_buffer/qwen3_dynamic_tensormap/tier1 | 5346.0 | 4487.4 | 858.5 | 16.1% | 3886.0 | 1459.9 | 27.3% |
| spmd/double_buffer/qwen3_dynamic_tensormap/tier2 | 4863.4 | 4104.9 | 758.5 | 15.6% | 3967.4 | 896.0 | 18.4% |
| spmd/double_buffer/qwen3_dynamic_tensormap/tier3 | 5015.8 | 4106.3 | 909.4 | 18.1% | 3915.3 | 1100.5 | 21.9% |
| spmd/double_buffer/qwen3_dynamic_tensormap/tier4 | 4952.9 | 4159.1 | 793.8 | 16.0% | 3952.5 | 1000.4 | 20.2% |

## 3. 统计概览

- 配置数：52；两套基线均通过依赖/时长/核类型不变量校验，makespan 序 方案二 ≤ 方案一 ≤ 实测 全部成立；方案二贪心在全部 52 配置均优于方案一（0 次回退）。
- **方案一延迟占比**（调度空闲）：均值 11.8%，中位 10.5%，最大 22.1%（spmd/basic/qwen3_dynamic_manual_scope/tier0）。
- **方案二延迟占比**（双重冗余）：均值 15.1%，中位 15.7%，最大 27.3%（spmd/double_buffer/qwen3_dynamic_tensormap/tier1）。
- 方案二相对方案一的额外压缩（跨核排布优化收益）：均值 149.5 us 更短的 makespan。

## 4. 逐任务差值重点（方案一，Top 延迟任务）

每配置列出 `start_delay`（实测启动−基线启动）最大的前 5 个任务，定位调度等待热点。

> 注：`span` 为任务级 min-start→max-end（跨其所有子任务）。**每个子任务的执行时长严格保留不变**（脚本 assert 校验）；对多子任务（SPMD/MIX）任务，零调度间隔会让其子任务随各核就绪时刻更分散，故基线 `span` 可能大于实测 `span`——这是子任务铺展、非时长改变，任务**完成时刻**仍 ≤ 实测。判定延迟以 `start_delay` 与 makespan 为准。

### double_buffer/qwen3_dynamic_manual_scope
实测 makespan 5360.7 us，方案一基线 4344.0 us，总延迟 1016.6 us（19.0%）。

| task | func | 实测start | 基线start | start_delay | 实测span | 基线span |
|--:|---|--:|--:|--:|--:|--:|
| t2896 | out_proj_residual | 3309.22 | 1485.9 | 1823.32 | 113.86 | 890.64 |
| t2899 | out_proj_residual | 3330.86 | 1520.16 | 1810.7 | 127.34 | 874.22 |
| t2902 | out_proj_residual | 3350.94 | 1558.4 | 1792.54 | 112.78 | 814.36 |
| t2906 | out_proj_residual | 3371.12 | 1585.02 | 1786.1 | 123.28 | 832.1 |
| t2903 | out_proj_residual | 3352.16 | 1618.56 | 1733.6 | 105.32 | 733.18 |

### lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier2
实测 makespan 4506.9 us，方案一基线 4138.3 us，总延迟 368.5 us（8.2%）。

| task | func | 实测start | 基线start | start_delay | 实测span | 基线span |
|--:|---|--:|--:|--:|--:|--:|
| t611 | silu | 2227.2 | 1603.42 | 623.78 | 3.2 | 5.0 |
| t518 | out_proj_residual | 1931.44 | 1369.16 | 562.28 | 302.5 | 516.64 |
| t523 | out_proj_residual | 1959.98 | 1415.3 | 544.68 | 219.14 | 454.62 |
| t521 | out_proj_residual | 1949.14 | 1407.28 | 541.86 | 216.94 | 461.96 |
| t522 | out_proj_residual | 1954.46 | 1413.9 | 540.56 | 325.72 | 536.6 |

### double_buffer/paged_attention_unroll
实测 makespan 2504.5 us，方案一基线 2290.8 us，总延迟 213.6 us（8.5%）。

| task | func | 实测start | 基线start | start_delay | 实测span | 基线span |
|--:|---|--:|--:|--:|--:|--:|
| t1551 | online_update | 2386.26 | 1789.2 | 597.06 | 3.06 | 3.06 |
| t1759 | online_update | 2434.54 | 1846.56 | 587.98 | 2.22 | 2.22 |
| t1431 | online_update | 2211.48 | 1626.64 | 584.84 | 2.4 | 2.4 |
| t1443 | online_update | 2285.32 | 1711.06 | 574.26 | 2.56 | 2.56 |
| t1902 | pv_matmul | 2386.88 | 1821.18 | 565.7 | 50.66 | 50.66 |

## 5. 结论

- **调度空闲（方案一）**：qwen3 类配置延迟普遍 7–22%，paged_attention basic 仅 ~2%（任务粒度小、依赖浅）；double_buffer 相对 basic 调度空闲更大，说明双缓冲派发在当前实测中未完全隐藏调度延迟。
- **排布不优（方案一→方案二额外收益）**：跨核自由迁移可再压缩数十至数百 us，paged double_buffer 尤为明显（Δ 从 ~9% 提升到 ~16%），表明实测存在核间负载不均。
- 逐任务 `start_delay` 热点集中在 **`out_proj_residual`（MIX，注意力输出残差）** 与 MLP 段的 `silu`/`down_proj`、以及 paged 的 `online_update`/`pv_matmul`——这些位于依赖链后段，累积了前序所有调度空闲，是后续调度优化的重点方向。

