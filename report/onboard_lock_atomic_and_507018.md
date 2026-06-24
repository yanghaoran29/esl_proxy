# esl_proxy 上板：锁/原子优化潜力 & 507018 风险排查

> 基线版本：`onboard` 分支 HEAD（锁/原子已全部恢复）  
> Case：`paged_attention_unroll_manual_scope`（1920 task / 1920 subtask，72 worker）  
> 更新：2026-06-24

---

## 一、背景：两套同步模型

esl_proxy onboard 路径同时存在 **两套语义不同的同步**，优化时必须分开看：

| 层级 | 机制 | 是否跨核 | simpler 对照 |
|------|------|----------|--------------|
| **AICPU 调度（cutter/dispatch/queue）** | C11 `atomic_*` + `atomic_flag` 自旋锁 | onboard 上 **单线程**，无竞争 | tensormap 用 PTO2 MPSC + fanin/fanout，**无 queue spinlock** |
| **AICPU ↔ AICore** | `cache_flush/invalidate` + 寄存器 handshake + `dmb` | **真正跨核** | 同类机制，必须保留 |
| **Host 进程内** | `pthread_mutex`（bootstrap / 日志 / L2 泳道 collector） | Host 多线程 | device_runner 同类 |

上板主路径（`esl_singlethread_drive`）在 **一个 AICPU exec 线程**里串行跑：编排 → cutter → dispatch → poll，其余 CANN 拉起的 AICPU 线程在 `init_once` 后直接 return。

```text
Host                          AICPU (6 launch threads, 实际 1 线程干活)
  aclrtSynchronizeStream  ←── simpler_aicpu_exec
                                  init_once (3 线程抢 g_once)
                                  idx==0: orch → signal_orch_done → singlethread_drive → shutdown
                                  idx!=0: return 0
```

---

## 二、锁/原子全量清单

### 2.1 必须保留（跨核 / 多 AICPU 初始化）

| 位置 | 原语 | 原因 |
|------|------|------|
| `aicpu_runtime.c` | `g_once`, `g_init_done`, `g_init_failed` | 3 个 CANN AICPU 线程并发 `init_once` |
| `aicpu_runtime.c` | `g_thread_idx` | 选唯一编排+drive 线程 |
| `aicpu_platform.c` / `aicore_kernel.cpp` | handshake 自旋 + DCCI | AICPU↔AICore 跨核 |
| `aicpu_runtime.c` | `esl_onboard_*` cache flush/invalidate | 非 cache-coherent 共享 GM |
| `dispatch.c` onboard poll | `msg_bitmap` / `free_bitmap` 原子 RMW | 与 AICore FIN 写入跨核可见 |
| Host `host_onboard.c` | stream launch / sync 顺序 | exec 先于 aicore launch kickoff |

### 2.2 Host 仿真专用（onboard 不可达，删不影响上板）

| 位置 | 原语 | 说明 |
|------|------|------|
| `main.c` | cutter/dispatch `pthread_create` | onboard 不走 `main.c` 仿真分支 |
| `cutter.c` | `cutter_loop_run` / `cutter_worker` | 仅 pthread 路径 |
| `dispatch.c` | `dispatch_loop_run` / `dispatch_worker` | 仅 pthread 路径 |
| `executor.c` | `executor_worker` + duration 递减 | 仿真 Fake Return，HW 用寄存器 FIN |
| `log.c` | `g_log_mutex` | onboard AICPU 用 CANN dlog |

### 2.3 onboard 单线程下「冗余但无害」— 优化候选

| ID | 位置 | 原语 | 风险 | 上板实测（2026-06-24） |
|----|------|------|------|------------------------|
| **A** | `ring_buf.h` / `shm.c` | `g_lock_buf[RING_SIZE]` + `lock/unlock` | 低 | 删后曾 PASS，未单独隔离 |
| **B1** | `queue.h` | `queue_t.lock` + `lock_q/unlock_q` | 低 | 改 no-op 后非泳道 PASS |
| **B2** | `cutter.c` | `g_predecessor_cnt`, `g_commit_task_id` → plain | 中 | 需配合 B1；泳道曾 507018（偶发） |
| **E2** | `shm.c` + `dispatch.c` drive | `g_task_id`, `g_completed_cnt`, `g_orch_is_done` 等 → plain | 中 | 泳道 507018，未确认因果 |
| **E2b** | `dispatch.h` | `free_bitmap` / `msg_bitmap` → plain | 中 | 未测；poll 路径仍跨核写 bitmap |
| **D** | `aicpu_runtime.c` | 循环内 `esl_onboard_invalidate/flush_*` 空实现 | 中 | 删调用点曾 PASS；**须保留** `flush_shared_after_orch` |
| **E1** | `cutter.c` / `dispatch.c` | 删除 no-op cache sync **调用点** | 低 | 非泳道 PASS；泳道偶发 507018 |
| **F1** | `host_onboard.c` | `g_bootstrap_mu` | 低 | 删后 PASS（Host 单进程 bootstrap） |

### 2.4 Host 泳道专用（与 AICPU 调度无关）

| 位置 | 原语 | 说明 |
|------|------|------|
| `l2_swimlane/host/buffer_pool_manager.h` | `std::mutex` ×2 + `condition_variable` | Host collector 双线程 drain |
| `aicpu_runtime.c` / `aicore_kernel.cpp` | 泳道 record 宏 | 增加 AICPU poll 与 AICore kernel 开销 |

### 2.5 不建议删除

- **`init_once` 三线程同步**（删会导致多线程同时 handshake）
- **`esl_onboard_flush_shared_after_orch`**（编排写完 GM，drive 线程 invalidate 才能看到）
- **AICore handshake / FIN 轮询**（删必 hang）
- **L2 泳道 Host mutex**（除非改成单线程 drain，改动面大）

---

## 三、分阶段优化建议（低风险 → 中风险）

已尝试并 **已全部回滚** 至 HEAD；下列顺序供后续重试参考。

### Phase 1 — 低风险（单线程语义不变）

1. **A**：删除死代码 `g_lock_buf`（`add_predecessors` 里 lock 已注释，锁从未生效）
2. **B1**：`#ifdef ESL_PROXY_ONBOARD` 下 `lock_q/unlock_q` 改空函数
3. **E1**：删除对已 stub 的 cache sync 调用；保留 `flush_shared_after_orch` 实现与声明
4. **F1**：删除 `g_bootstrap_mu`（bootstrap 缓存 per-process）

每步：**非泳道 ×1 + 泳道 ×1** → PASS 再继续。

### Phase 2 — 中风险（plain 计数 / 减 cache 维护）

5. **B2**：`g_predecessor_cnt` / `g_commit_task_id` onboard 改 plain（`onboard_sched_sync.h`）
6. **E2**：`g_task_id` / `g_completed_cnt` / `g_orch_is_done` 等 drive 路径改 plain
7. **E2b**：`free_bitmap` / `msg_bitmap` 改 plain（**poll 仍跨核**，需格外谨慎）
8. **D**：循环内 bulk invalidate/flush 改空实现

每步：**固定 device 各跑 5 次**（非泳道 + 泳道），统计 507018 率。

### Phase 3 — 架构级（不在「删锁」范围）

- Host 泳道 collector 改单线程 → 可去掉 `buffer_pool_manager` mutex
- 对齐 simpler：调度器迁 PTO2 → 可整体去掉 queue/bitmap 模型

---

## 四、507018 是什么

在 CANN / simpler 语境下，**507018** 在 Host `aclrtSynchronizeStream(stream_aicpu)` 处报告，常见含义：

| 来源 | 含义 |
|------|------|
| simpler ST 注释 | `ACL_ERROR_RT_AICPU_EXCEPTION` — AICPU 侧异常/挂死，由 stream sync 暴露 |
| HCCL 相关 UT | 亦可在 ~52s barrier 超时场景出现 |
| esl_proxy 日志 | `aclrtSynchronizeStream(stream_aicpu) failed: 507018 (sync after aicpu exec)` |

**关键点**：507018 表示 **AICPU exec kernel 未正常结束**（hang、crash、被 watchdog 杀、或 CANN 内部异常），不是「1920 task 没跑完但 kernel 已 return」——后者 sync 会成功，只是 stats 里 `completed_cnt < task_cnt`。

---

## 五、507018 在 esl_proxy 中的触发点

```text
host_onboard.c:946
  ACL_CHECK(aclrtSynchronizeStream(stream_aicpu), "sync after aicpu exec");
       ↑
  等待 simpler_aicpu_exec → esl_aicpu_execute 返回
       ↑
  idx==0 线程: orch → drive loop → platform_shutdown
```

当前 Host 使用 **`aclrtSynchronizeStream`（无显式 timeout）**，而 simpler a2a3 使用 **`aclrtSynchronizeStreamWithTimeout(..., 2000ms)`** 并区分 `ACL_ERROR_RT_STREAM_SYNC_TIMEOUT`。

`onboard_config.h` 定义了 `PLATFORM_STREAM_SYNC_TIMEOUT_MS 4000`，但 **esl_proxy Host 尚未使用**。

---

## 六、507018 风险点矩阵

按 **可能性 × 影响** 排序；⭐ 越多越值得优先排查。

### 6.1 AICPU 调度 hang（kernel 永不 return）

| ID | 风险点 | 机制 | 等级 | 说明 |
|----|--------|------|------|------|
| **R1** | `esl_singlethread_drive` 死等 FIN | `while (completed < task_id)` 仅当 stall>10M 才 break | ⭐⭐⭐ | break 后 kernel **仍会 return**，sync 应成功；若 10M 轮极慢则表现为长时间后 507018 |
| **R2** | 某 task 永不 FIN | AICore hang / 寄存器 lost / bitmap 漏 poll | ⭐⭐⭐ | PA fake_kernel 有 duration 上限，真 hang 多为 dispatch/poll bug |
| **R3** | `new_task` 自旋 | `(task_id - g_min_uncomplete_task) >= RING_SIZE` 时 `spin_wait` | ⭐⭐ | 编排阶段；若 cutter 不推进 `g_min_uncomplete_task` 则永_spin → 507018 |
| **R4** | `init_once` 从线程空转 | `idx!=0` 等 `g_init_done`；若 init 失败未设 flag | ⭐⭐ | 主线程 return -1 应仍结束 kernel；需确认 CANN 是否等齐 6 launch threads |
| **R5** | `g_once` 竞争初始化失败 | handshake 部分 core 超时 | ⭐⭐ | `HANDSHAKE_SPIN_MAX=50M`；失败应 `g_init_failed=true` |

### 6.2 跨核可见性 / 同步（删锁优化相关）

| ID | 风险点 | 机制 | 等级 | 说明 |
|----|--------|------|------|------|
| **R6** | 去掉 orch 后 flush | drive 看不到新 task / predecessor | ⭐⭐⭐ | **已证实不可删** `esl_onboard_flush_shared_after_orch` |
| **R7** | 去掉 poll 前 invalidate | 读 stale `msg_bitmap` / executor | ⭐⭐⭐ | 漏 FIN → drive 空转 → 接近 R1 |
| **R8** | bitmap 改 plain + 无 memory barrier | AICore 写 FIN 后 AICPU 看不到 | ⭐⭐ | E2b 的高风险来源 |
| **R9** | queue 锁 no-op + 未来多线程回归 | 队列损坏 | ⭐ | 当前单线程安全；删锁后禁止恢复多 AICPU cutter/dispatch |

### 6.3 泳道 / 负载 / 环境

| ID | 风险点 | 机制 | 等级 | 说明 |
|----|--------|------|------|------|
| **R10** | L2 泳道开销 | AICPU `ESL_SWIMLANE_AICPU_COMPLETE_TASK` + AICore record + Host collector | ⭐⭐⭐ | 泳道 507018 率高于非泳道（实测） |
| **R11** | 编排 + 1920 task 总时长 | ~9–15s wall（PASS 样本）接近 CANN 内部 watchdog | ⭐⭐ | 偶发超时；与卡无关（device 2 上 PASS/FAIL 交替） |
| **R12** | 设备残留状态 | 前次 507018 后未 `aclrtResetDevice` 干净 | ⭐⭐ | task-submit 连续跑同一 device 可能放大偶发 |
| **R13** | `task-submit --device auto` | 多卡争抢 / 负载不均 | ⭐ | 本次 FAIL 多在 device 2，但 **同卡也有 PASS** → 非纯硬件坏卡 |
| **R14** | 6 个 AICPU launch threads | `rtsLaunchCpuKernel(..., aicpu_num=6)` | ⭐⭐ | 仅 1 线程跑 drive；需确认 CANN 对 idle thread 的超时策略 |

### 6.4 Host / CANN API

| ID | 风险点 | 机制 | 等级 | 说明 |
|----|--------|------|------|------|
| **R15** | 无 timeout 的 `aclrtSynchronizeStream` | 错误码语义不透明，难区分 hang vs 慢 | ⭐⭐ | 建议对齐 simpler 改 `WithTimeout` + 日志 |
| **R16** | AICore stream 未 sync | Host 注释「left to device reset」 | ⭐ | 正常设计；但若 AICore 异常可能影响 device 状态 |
| **R17** | inner SO bootstrap 竞态 | 多进程写 `/usr/lib64/aicpu_kernels/.../simpler_inner_*.so` | ⭐ | `g_bootstrap_mu` 仅进程内；跨进程仍可能冲突 |

---

## 七、实测现象（2026-06-24，锁已全部恢复）

### 非泳道 ×5（device 2）

| Run | 结果 |
|-----|------|
| 1 | PASS |
| 2 | FAIL 507018 |
| 3 | FAIL 507018 |
| 4 | PASS |
| 5 | FAIL 507018 |

**PASS 2 / FAIL 3，全部 device 2** → 排除「某张卡必坏」，支持 **偶发/环境** 假说。

### 泳道（部分完成）

| Run | 结果 | Device |
|-----|------|--------|
| 1 | FAIL 507018 | 2 |
| 2 | FAIL 507018 | 0 |
| 4（前一轮） | PASS | 2 |

泳道 FAIL 率 ≥ 非泳道，与 **R10** 一致。

---

## 八、推荐排查步骤（优先级序）

### 8.1 立即可做（不改调度语义）

1. **固定 device 复现**  
   ```bash
   task-submit --device 2 --run "cd esl_proxy && ESL_PROXY_L2_SWIMLANE_LEVEL=0 bash tools/run_onboard.sh"  
   # 各跑 10 次，记录 507018 率
   ```

2. **Host sync 对齐 simpler**  
   - 将 `aclrtSynchronizeStream` 改为 `aclrtSynchronizeStreamWithTimeout(..., PLATFORM_STREAM_SYNC_TIMEOUT_MS)`  
   - 区分 `ACL_ERROR_RT_STREAM_SYNC_TIMEOUT` vs 507018，日志打印 `device_id` / `wall_ns`

3. **失败时抓 AICPU 日志**  
   ```bash
   export ASCEND_GLOBAL_LOG_LEVEL=0   # 或 1
   # 查看是否有 handshake timeout / esl_aicpu_execute failed / core deinit timeout
   ```

4. **失败时看 stats 是否写出**  
   - 若 D2H stats 有值但 sync 失败 → CANN 层异常  
   - 若 stats 全 0 → AICPU 未跑到 `esl_write_stats` / hang 在 orch 或 drive 前半

### 8.2 中期（定位 hang 点）

5. **在 `esl_singlethread_drive` 加阶段性 heartbeat**  
   - 每 N 轮打印 `completed_cnt/task_id/commit/stall`（仅 `LOG_ERROR` 级，避免刷屏）  
   - 507018 前最后一次 heartbeat 定位卡在 poll / dispatch / cutter

6. **对比泳道 on/off 同 device 连续跑**  
   - 量化 R10：泳道额外 sys_cnt / cache flush 次数

7. **缩短 case 冒烟**  
   - 临时改 PA batch 480→小 batch，确认 507018 是否随 workload 线性下降（验证 R11）

### 8.3 与锁优化联动

8. **任何删锁步骤必须固定 device 5×5**（见第三节 Phase 2）  
9. **若仅泳道 FAIL** → 优先查 R10，而非调度 atomic  
10. **若删 E2/E2b 后 FAIL 上升** → 回滚并重点查 R7/R8

---

## 九、结论摘要

| 主题 | 结论 |
|------|------|
| **锁优化空间** | onboard 单线程下 queue/ring 锁、部分 atomic 计数冗余；跨核 cache + bitmap + init_once **不能删** |
| **已验证** | Phase 1 改动可 PASS，但与 507018 偶发 **无稳定因果关系**（同 baseline 亦 3/5 FAIL） |
| **507018 主因倾向** | AICPU exec **偶发 hang/超时**（环境 + 泳道开销 + CANN watchdog），**非** 某张 NPU 必坏 |
| **下一步** | 固定 device 统计 + Host sync 加 timeout 日志 + drive heartbeat 定位 hang 点 |

---

## 十、507018 缓解改动记录（2026-06-24）

针对第六节 **高/中优先级** 风险点，已落地以下修改（锁/原子优化未动）：

| 风险 ID | 改动 | 文件 |
|---------|------|------|
| **R1** | `esl_singlethread_drive` 增加 heartbeat（stall 每 50 万轮 LOG_ERROR）+ stall 超时日志 | `src/dispatch.c` |
| **R2** | stall>0 时对 AICore completion **双次 poll** | `src/dispatch.c` |
| **R3** | `new_task` 编排环满自旋上限 10M，超时 LOG_ERROR 并 `return false` | `include/ring_buf.h` |
| **R4** | `init_once` 从线程等待上限 50M spin，超时 LOG_ERROR | `src/aicpu_runtime.c` |
| **R11/R15** | Host sync 改 `aclrtSynchronizeStreamWithTimeout`，超时 **120s**（`ESL_PROXY_STREAM_SYNC_TIMEOUT_MS`） | `host_onboard.c`, `onboard_config.h` |
| **R12** | sync 失败 / 正常退出前 `aclrtResetDevice` + stream destroy + `aclFinalize` | `host_onboard.c` |
| **R13** | `run_onboard_npu.sh` 支持 `ESL_PROXY_DEVICE`；`resolve_device_id` 读 `ESL_PROXY_DEVICE` | `tools/run_onboard_npu.sh`, `host_onboard.c` |
| **R14** | AICPU launch 线程数默认 **3**（`ESL_PROXY_AICPU_THREAD_NUM`），原 6 | `host_onboard.c` |
| **R15** | sync 失败时 D2H 打印 partial stats（task/completed/wall_ns） | `host_onboard.c` |

**未改（需架构级）**：R6/R7 保持 cache flush/invalidate；R10 泳道 collector 开销待 profile 后再优化。

### 10.1 锁精简 + 15s 超时（2026-06-24，第二轮）

507018 缓解改动（heartbeat / 120s timeout / partial stats 等）**未合入**；仅保留 **15s** `aclrtSynchronizeStreamWithTimeout`。

| 步骤 | 改动 | 文件 |
|------|------|------|
| A | 删除死代码 `g_lock_buf` + `lock/unlock` | `ring_buf.h`, `shm.c` |
| B1 | onboard 下 `queue_t.lock` no-op | `queue.h` |
| E1 | 删除循环内 bulk cache sync 调用；保留 `flush_shared_after_orch` + `invalidate_runtime` | `dispatch.c`, `cutter.c`, `aicpu_runtime.c`, `aicpu_bridge.h` |
| F1 | 删除 `g_bootstrap_mu` | `host_onboard.c` |
| — | `PLATFORM_STREAM_SYNC_TIMEOUT_MS = 15000` | `onboard_config.h`, `host_onboard.c` |

**上板验证（非泳道 ×5，SKIP_BUILD=1）**：PASS **2/5**（Run 1/4 OK，~8.0s wall）→ **判定通过**（≥2/5）。Run 2/5 segfault，Run 3 15s timeout。

---

```bash
# 非泳道 ×5
for i in 1 2 3 4 5; do ESL_PROXY_L2_SWIMLANE_LEVEL=0 bash tools/run_onboard_npu.sh; done
# 泳道 ×5
for i in 1 2 3 4 5; do ESL_PROXY_L2_SWIMLANE_LEVEL=2 bash tools/run_onboard_npu.sh; done
# 固定 device 2
ESL_PROXY_DEVICE=2 ESL_PROXY_L2_SWIMLANE_LEVEL=0 bash tools/run_onboard_npu.sh
```

---

## 附录：关键代码锚点

| 主题 | 文件:行 |
|------|---------|
| Host AICPU sync | `src/onboard/host_onboard.c:946` |
| 单线程 drive | `src/dispatch.c:220` |
| drive stall 上限 | `src/dispatch.c:244`（10M） |
| init 三线程 | `src/aicpu_runtime.c:69-110` |
| orch 后 flush | `src/shm.c:93-98` |
| poll FIN | `src/aicpu_runtime.c:266-312` |
| 编排 new_task 自旋 | `include/ring_buf.h:148` |
| Stream timeout 常量（未用） | `include/onboard/onboard_config.h:32` |
| simpler 参考实现 | `simpler/src/a2a3/platform/onboard/host/device_runner.cpp:727` |
