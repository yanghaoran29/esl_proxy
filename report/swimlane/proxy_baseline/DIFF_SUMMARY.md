# proxy_baseline 实测-基线耗时差值汇总

单位 us。scheme1 = 固定拓扑零调度间隔；scheme2 = 依赖约束最优拓扑。
`Δ` = 实测 makespan − 基线 makespan（实测相对基线的延迟）。

| config | measured | scheme1 | Δ1 | Δ1% | scheme2 | Δ2 | Δ2% |
|---|--:|--:|--:|--:|--:|--:|--:|
| basic/paged_attention_unroll | 2170.4 | 2133.6 | 36.8 | 1.7% | 2102.7 | 67.6 | 3.1% |
| basic/paged_attention_unroll_manual_scope | 2167.2 | 2134.5 | 32.8 | 1.5% | 2099.0 | 68.2 | 3.1% |
| basic/qwen3_dynamic_manual_scope | 5184.2 | 4082.2 | 1102.0 | 21.3% | 3930.5 | 1253.6 | 24.2% |
| basic/qwen3_dynamic_tensormap | 4601.1 | 4136.7 | 464.4 | 10.1% | 3930.7 | 670.3 | 14.6% |
| double_buffer/paged_attention_unroll | 2504.5 | 2290.8 | 213.6 | 8.5% | 2095.8 | 408.7 | 16.3% |
| double_buffer/paged_attention_unroll_manual_scope | 2443.8 | 2212.2 | 231.6 | 9.5% | 2098.9 | 344.9 | 14.1% |
| double_buffer/qwen3_dynamic_manual_scope | 5360.7 | 4344.0 | 1016.6 | 19.0% | 4017.1 | 1343.6 | 25.1% |
| double_buffer/qwen3_dynamic_tensormap | 4878.7 | 4103.9 | 774.8 | 15.9% | 3955.6 | 923.1 | 18.9% |
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
