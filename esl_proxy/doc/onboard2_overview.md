# esl_proxy onboard2 分支总览

本文档描述 esl_proxy `onboard2` 分支相对 `base` 分支引入的改动，覆盖三个方面：
1. 代码入口与执行方式
2. onboard2 新增代码的主要逻辑
3. 对 QuteMiao 原有代码的修改及原因

---

## 一、代码入口与执行方式

esl_proxy 有两个互斥的构建/运行模式，由编译宏 `ESL_PROXY_ONBOARD_HOST` 选择。入口均在 `esl_proxy/src/main.c`。

### 1.1 CPU 模拟模式（默认，Makefile 构建）

**入口**：`esl_proxy/src/main.c` 的 `main(void)`（`#else /* !ESL_PROXY_ONBOARD_HOST */` 分支）。

**构建命令**：
```bash
cd esl_proxy/esl_proxy
make                              # 默认 CASE=qwen3_dynamic_manual_scope.h
make CASE=qwen3_dynamic_tensormap.h run
make CASE=paged_attention_unroll.h run
make CASE=paged_attention_unroll_manual_scope.h run
```

**执行流程**（`main.c`）：
1. `mem_pool_init` / `ring_buf_init` / `init_predecessors` / `init_ctrl_t` —— 初始化内存池、环形缓冲、前驱表、调度控制结构
2. `platform_bringup()` —— sim 平台初始化（无 MMIO）
3. `platform_workers_start(72)` —— 启动 fake AICore worker 线程（若 `ESL_PROXY_FAKE_KERNEL=1`）
4. `pthread_create` 启动 `CUTTER_THREAD_CNT` 个 cutter 线程 + `DISPATCH_THREAD_CNT` 个 dispatch 线程
5. `aicpu_orchestration_entry(0)` —— 调用由 `ORCH_CASE` 选定的编排 case（如 qwen3_dynamic_manual_scope.h），构造 DAG 任务
6. `atomic_store(&g_orch_is_done, true)` —— 通知 dispatch 进入 phase2（回收剩余完成事件）
7. `pthread_join` 等待 cutter/dispatch 结束
8. 校验 `g_completed_cnt == g_task_id`，打印 PASS/FAIL

**平台后端**：链接 `src/platform/sim/*.c`。`platform_*` 钩子为 no-op 或 host 原生实现（如 `get_time_ns` 用 `clock_gettime`）。

### 1.2 上板模式（CMake 构建，CANN 运行）

**入口**：`esl_proxy/src/main.c` 的 `main(argc, argv)`（`#if ESL_PROXY_ONBOARD_HOST` 分支），直接转发到 `esl_onboard_run`（`src/platform/onboard/host_onboard.c`）。

**构建命令**（需 `ASCEND_HOME_PATH`）：
```bash
source $ASCEND_HOME_PATH/bin/setenv.bash
bash build/build_aicpu.sh        # 交叉编译 AICPU .so（含 algorithm 源）
bash build/build_aicore.sh       # 编译 AICore kernel .o
bash build/build_onboard_host.sh # 编译 host runner
```

**运行命令**（经 task-submit 独占设备）：
```bash
task-submit --timeout 60 --max-time 60 --device auto --device-num 1 \
  --env ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0 \
  --run "cd <root> && bash tools/run_onboard.sh"
```

**指定样例**：通过环境变量 `ESL_PROXY_ORCH_CASE` 选择编排 case（编译进 AICPU .so）。
默认 `paged_attention_unroll_manual_scope.h`。可选值与 CPU 模拟一致：
`qwen3_dynamic_manual_scope.h` / `qwen3_dynamic_tensormap.h` /
`paged_attention_unroll.h` / `paged_attention_unroll_manual_scope.h`。
qwen3 系列还可用 `QWEN3_SPMD_TIER`（0=非 SPMD .. 4=全 SPMD，默认 0）控制 SPMD 展开粒度。

```bash
# 指定 case + SPMD tier（需重新构建，不带 --skip-build）
task-submit --timeout 60 --max-time 60 --device auto --device-num 1 \
  --env ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0 \
  --env ESL_PROXY_ORCH_CASE=qwen3_dynamic_manual_scope.h \
  --env QWEN3_SPMD_TIER=0 \
  --run "cd <root> && bash tools/run_onboard.sh"
```

注意：case 改变需重新编译 AICPU .so（不能加 `--skip-build`），因为 case 头文件是编译期
通过 `-DORCH_CASE=<case>.h` 注入 `main.c` 的。`run_onboard.sh` 检测到
`ESL_PROXY_ORCH_CASE` 后会 export 成 `ORCH_CASE` 传给 `build_aicpu.sh`。

`tools/run_onboard.sh` 的流程：
1. （未带 `--skip-build` 时）依次调三个 build 脚本
2. 启动 `build/onboard/host/esl_onboard_runner`，传入 `-d $TASK_DEVICE --dispatcher ... --aicpu ... --aicore ...`
3. runner 通过 CANN ACL 加载 AICPU dispatcher .so 和 AICore kernel .o，在 NPU 上完成握手 → AICore launch → AICPU exec → 完成回收
4. D2H 拷回 `device_wall` 统计，打印 `task_cnt/subtask_cnt/completed_cnt` 和 PASS/FAIL

**平台后端**：AICPU 侧链接 `src/platform/onboard/*.c`；AICore 侧链接 `aicore_entry.cpp`/`aicore_executor.cpp`。`platform_*` 钩子执行真实 `dc civac`/`dc cvac` 缓存操作和 MMIO 寄存器读写。

### 1.3 关键构建产物

| 产物 | 路径 | 作用 |
|------|------|------|
| CPU 模拟可执行 | `esl_proxy/bin/esl_proxy` | host 单进程多线程模拟 |
| AICPU dispatcher .so | `build/onboard/aicpu/libesl_aicpu_dispatcher.so` | 上板 AICPU 侧调度器入口 |
| AICPU kernel .so | `build/onboard/aicpu/libaicpu_kernel.so` | 上板 AICPU 侧 algorithm + 平台代码 |
| AICore kernel .o | `build/onboard/aicore/aicore_kernel.o` | 上板 AICore 侧 kernel |
| host runner | `build/onboard/host/esl_onboard_runner` | 上板 host 侧启动器 |

---

## 二、onboard2 新增代码的主要逻辑

onboard2 相对 base 新增约 9000 行，核心是把 esl_proxy 从"host 单进程模拟器"扩展为"sim/onboard 双后端、可在真实 NPU 上运行"的统一框架。改动分以下几块。

### 2.1 平台 HAL 抽象层（`include/platform/platform.h`）

新增统一的 platform HAL 头文件，algorithm 层只调 `platform_*` 接口，不感知 sim/onboard 差异：

- 时间：`get_time_ns()`（sim: `clock_gettime`；onboard: 读硬件计时器）
- 日志：`platform_main_log_vwrite`
- worker 管理：`platform_workers_start/stop`、`platform_pick_phys_worker`、`platform_worker_block_dim`
- 任务下发/完成：`platform_issue_block`、`platform_pop_completion`
- 缓存同步：`cache_invalidate_range`/`cache_flush_range`（sim: no-op；onboard: `dc civac`/`dc cvac`）
- 调度状态发布/消费：`platform_publish_task_slot`/`platform_publish_predecessor_cnt`/`platform_publish_atomic_u64`/`platform_consume_task_slot` 等
- 队列锁钩子：`platform_queue_lock_prepare`/`platform_queue_unlock_publish`
- tracing 钩子：`platform_cutter_loop_enter/iter`、`platform_dispatch_loop_enter/exit` 等

两套后端实现：
- **sim**：`src/platform/sim/*.c`，全部 no-op 或 host 原生
- **onboard**：`src/platform/onboard/*.c`，真实 MMIO + DCCI 缓存操作

### 2.2 AICore bridge 协议（`include/algorithm/aicore_bridge.h` + 两套实现）

新增 backend 中立的 AICore 桥接层，统一握手、下发 payload、完成轮询三个动作：

- `aicore_bridge_init/shutdown`
- `aicore_bridge_dispatch_task(bridge, tid, task_id, core, slot, exe_type, block_idx)` —— 下发一个任务到 AICore
- `aicore_bridge_poll_completions(bridge, tid)` —— 轮询硬件完成寄存器，置位 msg_bitmap

sim 实现（`src/platform/sim/aicore_bridge.c`）走 fake_aicore；onboard 实现（`src/platform/onboard/aicore_bridge.c`）走真实寄存器 `REG_ID_COND`/`REG_ID_DATA_MAIN_BASE`。

### 2.3 onboard AICPU/AICore bring-up

- **AICPU 侧**（`src/platform/onboard/aicpu_runtime.c`、`aicpu_dispatcher.c`、`aicpu_platform_init.c`）：72 worker 模型，3 个 AICPU 线程角色（cutter/dispatch/orch），通过 `esl_onboard_trace` 写设备 trace 缓冲
- **AICore 侧**（`src/platform/onboard/aicore_entry.cpp`、`aicore_executor.cpp`）：AICore kernel 入口，72 worker busy-wait 执行 fake kernel，完成后写完成寄存器
- **握手**（`src/algorithm/handshake.c`、`aicore_handshake.c`）：host 与 AICore 启动时交换 physical_core_id / aicpu_regs_ready，确保 AICore 就绪后才下发
- **host runner**（`src/platform/onboard/host_onboard.c`）：通过 CANN ACL 加载 .so/.o、launch stream、同步、D2H 统计

### 2.4 调度快照同步（`include/algorithm/sched_sync.h`）

新增 algorithm 层 inline 头文件，集中管理跨 AICPU 核的共享调度状态缓存同步：

- `invalidate_sched_snapshot()` —— 每轮读前 invalidate 5 个全局计数器 + `g_predecessor_cnt[]` + 全部 ready/completed queue
- `publish_counters()` —— 写后 flush 5 个计数器 + `g_predecessor_ring.tail` + `dmb ishst`
- `publish_task_slot(task_id)` —— flush `g_basic_buf`/`g_predecessors`/`g_predecessor_cnt` 对应 slot
- `advance_task_id()` —— `publish_task_slot(finished)` + `atomic_fetch_add(g_task_id,1)` + `publish_counters()`

sim 下全部 no-op；onboard 下是正确性必需（非一致性 L1 cache）。

### 2.5 dispatch API 扩展（`include/algorithm/dispatch.h`）

新增 6 个 dispatch API 函数，支撑 onboard AICore bridge 集成：

| 函数 | 角色 |
|------|------|
| `dispatch_bind(bridge)` | 依赖注入 AICore bridge，维持算法/平台分层 |
| `dispatch_tick_begin(tid)` | 每轮读前 invalidate 调度快照缓存 |
| `dispatch_poll(tid)` | 拉 HW AICore 完成事件到 msg_bitmap |
| `dispatch_submit(...)` | 核心动作一：下发任务给 AICore + slot 占用语义打包 |
| `dispatch_drain_completions(...)` | 核心动作二：解码 msg_bitmap 回收已完成任务 |
| `dispatch_after_push_completed(...)` | 回收写后 publish 计数器 |

### 2.6 swimlane 工具链（`include/swimlane/` + `src/swimlane/`）

新增 L2 swimlane 采集与 Perfetto 转换工具：
- `swimlane_aicpu.c` / `host_swimlane.c` —— 在 AICPU/host 侧采集每任务的时序
- `swimlane_to_perfetto.py` —— 转成 Perfetto trace JSON

### 2.7 构建系统（`build/`）

新增三个构建脚本 + CMake：
- `build/build_aicpu.sh` —— 交叉编译 AICPU .so（含 `src/algorithm/cutter.c`/`dispatch.c`/`shm.c`）
- `build/build_aicore.sh` —— 编译 AICore kernel .o
- `build/build_onboard_host.sh` —— 编译 host runner

### 2.8 task_desc payload 拆分

`task.h` 新增 `struct task_payload`（`Tensor tensors[16]` + `int64_t scalars[32]` + 计数），把原本内联在 `task_desc` 里的 `data[16]`/`scalar[32]` 拆出。`ring_buf.h` 的 `add_tensor_addr(id, addr)` 改为 `add_tensor(id, Tensor*)`，存储完整 Tensor（含 shape/owner_task_id），供 onboard `esl_pack_dispatch_input` 构造下发 payload。

---

## 三、修改 QuteMiao 代码的原因

QuteMiao 是 base 分支上 `include/algorithm/` 与 `src/algorithm/` 主要代码的作者。onboard2 对这些代码的修改分两类：**功能性 BUG 修复**（必需）和**平台抽象适配**（onboard 必需）。纯格式改动和 QuteMiao 的注释/死代码已按"非必要不修改、QuteMiao 提交不得删除"原则回退，此处只列保留的功能性修改。

### 3.1 `conf.h` —— 容量溢出修复

| 修改 | 原值 | 新值 | 原因 |
|------|------|------|------|
| `NODE_BUFF_SIZE` | 8192 | 65536 | qwen3 attention DAG 前驱边数过多，8192 溢出前驱环 |
| `CON_NODE_CNT` | 32 | 256 | 共享 scope 生产者（如 RMSNorm tile）扇出过大，32 溢出 `g_successor_buf[].node[]` 破坏后继图 |
| `WORKER_LOG` 守卫 | 无 | `#ifndef WORKER_LOG` | 允许构建脚本 `-DWORKER_LOG=0` 覆盖 |

### 3.2 `ring_buf.h` `add_predecessors` —— 原子指针步长 BUG

原 QuteMiao 代码：
```c
uint16_t* idx = atomic_fetch_add(&g_predecessor_ring.tail, 1);
```
`g_predecessor_ring.tail` 类型是 `uint16_t* _Atomic`。GCC 的 `__atomic_fetch_add` 对指针原子对象按**字节**前进（不是按 `sizeof(uint16_t)=2` 元素步长），导致 tail 每次只前进 1 字节，相邻前驱条目（2 字节）被半覆盖，依赖图损坏。改为：
```c
uint16_t* idx = atomic_load_explicit(&g_predecessor_ring.tail, memory_order_relaxed);
atomic_store_explicit(&g_predecessor_ring.tail, idx + 1, memory_order_relaxed);
```
`idx + 1` 是标准 C 指针算术，跨编译器一致。sim 上 base 版本碰巧没暴露（host gcc 部分版本按元素步长），onboard 交叉编译器必现。

同时新增 `platform_publish_u16(idx)` 和 `platform_publish_task_slot(task_id)`：onboard 多核非一致性缓存下，orch 核写的前驱条目和 `ptr->cnt` 必须 flush 才能被 cutter 核读到。sim 下为 no-op。

### 3.3 `ring_buf.h` `new_task` —— 类型与 payload 适配

- `(task_id - g_min_uncomplete_task)` 强转 `uint32_t`：避免有符号比较误判
- `new_task` 签名增加 `duration_ns`/`jitter_mask`：onboard swimlane 测量需要 32 位 duration 和 jitter 掩码
- 写入 `g_task_payload[slot].tensor_cnt=0`/`scalar_cnt=0`：配合 payload 拆分，每任务重置
- 新增 `task_payload_materialize` + `platform_publish_task_slot`：onboard 跨核发布

### 3.4 `queue.h` —— 队列锁缓存钩子

`lock_q`/`unlock_q` 增加 `platform_queue_lock_prepare`/`platform_queue_unlock_publish`：onboard 多核下队列是共享结构，加锁前需 invalidate、解锁后需 flush，否则跨核读写队列 head/tail/cnt 看到过期值。sim 下 no-op。

### 3.5 `dispatch.c` —— 派发器 onboard 集成

- `get_completed` 重构为 `drain_completed_bitmap` + `dispatch_drain_completions`：回收逻辑提取为独立函数
- 新增 `dispatch_bind/poll/submit/tick_begin/after_push_completed`：见 §2.5
- `send_task` 增加 `block_mask = (1<<platform_worker_block_dim())-1`：onboard worker 数可配置（24 而非写死 60）
- `dispatch_worker` 增加 stall 超时检测：防 onboard 死锁诊断

### 3.9 `shm.c` —— 共享数据 onboard 适配

- 新增 `g_task_payload[RING_SIZE]`：配合 payload 拆分
- 新增 `g_subtask_cnt` weak 符号：onboard SPMD subtask 计数
- 新增 `esl_signal_orch_done`：onboard 下 orch 核通知 dispatch/cutter 结束（带 cache flush）
- `init_ctrl_t` 的 `free_bitmap` 初值改用 `platform_worker_block_dim()`：onboard worker 数可配置

### 3.10 `tensormap.h` —— `add_tensor` 签名跟随

`tm_in_ptr`/`tm_out_ptr` 等 6 个函数里的 `add_tensor_addr(tid, t->buffer_addr)` 改为 `add_tensor(tid, t)`，跟随 `ring_buf.h` 的 payload 拆分。

### 3.11 不改动的部分

以下 QuteMiao 代码虽涉及"看似可改"的地方，但**未做功能性修改**（纯格式已回退、注释/死代码已恢复）：
- `dispatch.h` 文件头注释、`// 64CORES`、`queue_t` 双空格对齐
- `dispatch.c` 的 `// TODO: add counter for spmd`、`// TODO: Work Stealing`、`// Check both slots...`、`// Fake Return`、`// atomic_store(&g_is_done, true); // return NULL;` 死代码
- `cutter.c` 的 `// WORKER_LOGF("ready,...` 死代码、`deal_completed_queue` 里的 4 行死代码注释
- `shm.c` 的 `// Keep Atomic For Multi Dispatch Thread`、`// Initialize ...` 系列注释、`0x0` 字面量
- `log.c` 的 `// Output to stdout only` 注释

---

## 四、验证状态

- **CPU 模拟**：4 个 case（qwen3_dynamic_manual_scope、qwen3_dynamic_tensormap、paged_attention_unroll、paged_attention_unroll_manual_scope）全 PASS
- **task-submit 上板**：`task_cnt=1920 subtask_cnt=1920 completed_cnt=1920 OK (all 1920 tasks / 1920 subtasks completed)`

注：`aicore` 构建脚本目前因 `runtime.h` 路径问题在本地编译失败（预存问题），上板验证用 `--skip-build` 复用已有 `aicore_kernel.o`。
