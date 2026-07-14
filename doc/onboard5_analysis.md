# onboard5 分支分析文档

## 1. 整体架构

### 1.1 源码目录结构

```
esl_proxy/
├── Makefile                        # 构建系统
├── cases/                          # 测试用例定义
│   ├── org.h                       # 组织模式枚举（QuteMiao 原创）
│   ├── qwen3_dynamic_manual_scope.h
│   ├── qwen3_dynamic_tensormap.h
│   ├── paged_attention_unroll.h
│   └── paged_attention_unroll_manual_scope.h
├── include/
│   ├── algorithm/                  # 算法层头文件
│   │   ├── conf.h                  # 配置参数（QuteMiao 原创，yanghaoran29 修改）
│   │   ├── dispatch.h              # 公开 API：ctrl_t / worker / poll / init / spmd_on_ready（MIX/SPMD 助手均为 dispatch.c 内 static）
│   │   ├── cutter.h                # Cutter 接口
│   │   ├── executor.h              # Executor 类型定义（QuteMiao 原创）
│   │   ├── handshake.h             # AICPU-AICore 握手机制（yanghaoran29 新增）
│   │   ├── ring_buf.h              # Ring Buffer 管理（QuteMiao 原创，yanghaoran29 修改）
│   │   ├── task.h                  # 任务描述符（QuteMiao 原创）
│   │   ├── queue.h / mpmc_queue.h  # 锁-free 队列（QuteMiao 原创）
│   │   ├── mem_pool.h / spin.h     # 内存池/自旋（QuteMiao 原创）
│   │   ├── tensor.h / tensormap.h  # Tensor 类型（QuteMiao 原创）
│   │   └── memory_barrier.h        # 内存屏障（yanghaoran29 新增）
│   ├── platform/                   # 平台层头文件（yanghaoran29 新增）
│   │   ├── platform_config.h       # L1: 共享配置（拓扑/寄存器/任务编码）
│   │   ├── worker_map.h            # L1: 72 核拓扑与映射
│   │   ├── platform.h              # L2: 后端选择
│   │   ├── platform_regs.h         # L0: 寄存器访问接口
│   │   ├── sim/                    # Sim 后端头文件
│   │   └── onboard/                # Onboard 后端头文件
│   └── swimlane/                   # 泳道 trace 头文件（yanghaoran29 新增）
└── src/
    ├── main.c                      # 主入口（QuteMiao 原始框架，yanghaoran29 修改线程模型）
    ├── algorithm/
    │   ├── dispatch.c              # 单文件调度实现（区序 0-1-2-5-6-4-3；basic/double_buffer 由局部 #if ESL_DISPATCH_DOUBLE_BUFFER 区分）
    │   ├── dispatch_payload.c      # Payload 组装（yanghaoran29 新增）
    │   ├── cutter.c                # DAG 依赖解析（QuteMiao 原始框架，yanghaoran29 修改）
    │   ├── executor.c              # Executor 初始化（QuteMiao 原创）
    │   ├── handshake.c             # AICPU-AICore 握手状态机（yanghaoran29 新增）
    │   └── shm.c                   # g_ctrl_t / g_shared_ready / init_ctrl_t（yanghaoran29 新增）
    └── platform/
        ├── sim/                    # Sim 后端实现（yanghaoran29 新增）
        │   ├── platform_init.c     # 平台初始化
        │   ├── platform_regs.c     # 寄存器访问原语
        │   ├── platform_sim.c      # Sim 运行时
        │   ├── sim_core_regs.c     # 72 核寄存器表
        │   ├── cache_ops.c         # Cache 维护操作
        │   ├── aicore.c            # AICore 仿真
        │   ├── device_runner.c     # 线程化设备执行器
        │   ├── device_runner_instant.c # 即时设备执行器
        │   └── log.c               # 日志基础设施
        └── onboard/
            └── aicpu_runtime.c     # Onboard AICPU 运行时（yanghaoran29 新增）
```

### 1.2 模块关系与数据流

```
                    ┌─────────────┐
                    │  main.c     │  主线程入口
                    │ (orch线程)  │
                    └──────┬──────┘
                           │ aicpu_orchestration_entry()
                           │ 提交任务到 g_basic_buf + g_predecessors
                           ▼
    ┌──────────────────────────────────────────┐
    │           Ring Buffer 层                  │
    │  g_basic_buf[RING_SIZE]    (任务描述符)   │
    │  g_predecessors[RING_SIZE] (前驱列表)     │
    │  g_state_buf[RING_SIZE]    (任务状态)     │
    │  g_successor_buf[RING_SIZE](后继列表)     │
    └──────────────┬───────────────────────────┘
                   │
         ┌─────────┴─────────┐
         ▼                   ▼
  ┌──────────────┐   ┌──────────────┐
  │ cutter_worker│   │dispatch_worker│  N 个 lane，每个 lane 1:1 配对
  │  (cutter.c)  │   │ (dispatch.c)  │
  └──────┬───────┘   └──────┬────────┘
         │                  │
         │ resolve_dep      │ dispatch_poll (Phase-1)
         │ add_successors   │ send_task/send_task_mix (Phase-4)
         │ stage_ready      │ dispatch_prefetch
         │                  │
         ▼                  ▼
  ┌──────────────────────────────────────┐
  │  g_shared_ready[TASK_TYPE_CNT]        │  共享就绪队列（每种类型一个）
  │  (CUBE / VECTOR / MIX)                │
  └──────────────────┬───────────────────┘
                     │ batch_dequeue
                     ▼
  ┌──────────────────────────────────────┐
  │  g_executors[EXE_TYPE_CNT][AIC_CNT]   │  执行器数组（每个核每个类型）
  │  g_ctrl_t[DISPATCH_THREAD_CNT]        │  每_lane 控制结构
  │  g_next_block[RING_SIZE] (non-atomic)│  SPMD 块游标
  └──────────────────┬───────────────────┘
                     │ esl_publish_subtask_to_core()
                     ▼
  ┌──────────────────────────────────────┐
  │           平台层 (L0-L3)              │
  │  Sim: 共享内存寄存器 + pthread        │
  │  Onboard: CANN rtsLaunchCpuKernel     │
  └──────────────────────────────────────┘
```

### 1.3 线程模型

onboard5 分支仅包含**并行式（overlapped）模型**，`ESL_ORCH_FIRST=0`（见 [conf.h](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L31)）：

```
线程数 = 2 × ESL_LANE_CNT + 1
       = N 个 cutter 线程 + N 个 dispatch 线程 + 1 个 orchestrator 线程
```

- **Orchestrator 线程**：运行 `aicpu_orchestration_entry()`，提交任务到 ring buffer，与 cutter/dispatch 并行执行
- **Cutter 线程**（[cutter.c:212](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c#L212)）：解析 DAG 依赖，将就绪任务路由到 `g_shared_ready`
- **Dispatch 线程**（[dispatch.c:669](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L669)）：轮询硬件完成事件，将就绪任务分发到空闲核

**核分区**（strided 分配，见 [conf.h:43](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L43)）：
```c
#define CORE_LANE(core) ((core) % DISPATCH_THREAD_CNT)
// lane i 拥有核 { c : c % ESL_LANE_CNT == i }
```

默认 `ESL_LANE_CNT=1`：1 cutter + 1 dispatch + 1 orch = 3 线程，所有 24 个 AIC 核由一个 dispatch 管理。

### 1.4 关键数据结构

| 数据结构 | 定义位置 | 说明 |
|----------|----------|------|
| `task_desc` | [task.h:54](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h#L54) | 任务描述符：类型、模式、kernel、block数、tensor数据 |
| `ctrl_t` | [dispatch.h:24](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/dispatch.h#L24) | 每_lane控制：free_bitmap、msg_bitmap、task_id_map、completed_queue |
| `executor_t` | [executor.h:24](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/executor.h#L24) | 每核执行器：2-slot ping-pong（tasks、block_idx、base） |
| `g_basic_buf[RING_SIZE]` | ring_buf.h | 任务描述符环形缓冲，O(1) 按 task_id 索引 |
| `g_shared_ready[TASK_TYPE_CNT]` | [shm.c:36](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/shm.c#L36)（声明于 dispatch.h） | 共享就绪队列，每种任务类型一个 |
| `g_executors[EXE_TYPE_CNT][AIC_CNT]` | [executor.h:45](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/executor.h#L45) | 执行器数组，每个核每个类型一个 |
| `g_ctrl_t[DISPATCH_THREAD_CNT]` | [shm.c:35](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/shm.c#L35) | 每_lane控制结构数组 |
| `g_next_block[RING_SIZE]` | [dispatch.c:55](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L55) | 非原子SPMD块游标（`uint16_t`），pop-serialized 保证安全（为什么可以非原子见 §2.2.2-C） |
| `g_finished_blocks[RING_SIZE]` | [dispatch.c:58](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L58) | 原子SPMD完成计数器（`_Atomic uint16_t`），多lane并发完成不丢计数（为什么见 §2.2.2-C） |

### 1.5 构建系统

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `DISPATCH` | `basic` | Sim Makefile：`basic` 或 `double_buffer`（后者加 `-DESL_DISPATCH_DOUBLE_BUFFER=1`）；**始终编译同一 `dispatch.c`** |
| CMake `ESL_PROXY_DOUBLE_BUFFER` | OFF | Onboard：`ON` 时给 `aicpu_kernel` 加 `-DESL_DISPATCH_DOUBLE_BUFFER=1`（见 `cmake/sources.cmake` + `cmake/aicpu/CMakeLists.txt`） |
| `CASE` | `qwen3_dynamic_manual_scope.h` | 测试用例 |
| `QWEN3_SPMD_TIER` | `0` | SPMD任务粒度（0=非SPMD .. 4=全SPMD） |
| `LANE_CNT` | `1` | 调度lane数 |
| `SIM_AICORE` | `instant` | AICore仿真模式：`instant`(即时FIN) 或 `threaded`(72 pthread) |

`dispatch.c` 区内顺序（文件头注释）：**0** includes → **1** 静态状态 → **2** 基础（reg/poll/publish）→ **5** 调度主路径 → **6** worker → **4** MIX → **3** SPMD；2/5 之前用前向声明引用 4/3。

## 2. 算法介绍

> 本章在描述每个算法时，同时标注 simpler（`/simpler/src/a5/runtime/tensormap_and_ringbuffer/`）中对应的实现位置，便于对照阅读。对锁/内存屏障/原子操作/cache 操作，均说明其**为什么**需要加。

### 2.1 原始框架
在 main 分支上构建了 DAG 调度引擎的骨架，以下是其核心贡献：

#### 2.1.1 DAG 依赖解析框架（cutter.c 原始版本）
实现了 DAG 依赖解析的基本框架：
- `add_successors()`：将新提交任务的后继关系注册到前驱的 fanout 列表
- `resolve_dep()`：任务完成后，递减后继的前驱计数，计数归零则就绪
- `deal_completed_queue()`：cutter 线程主循环，处理完成队列
- `g_predecessors[]` / `g_successor_buf[]`：前驱/后继列表存储
- `g_state_buf[]`：任务状态（CREATING / SUBMITTED / COMPLETED）
- `g_predecessor_cnt[]`：前驱计数（原子递减）

> **与simpler的异同**：`pto_scheduler.h` 的 `release_fanin_and_check_ready()` 对应 `resolve_dep()`；`on_task_complete()` 对应 `deal_completed_queue()`（职责等价）。**同**：两者都用原子计数器做 fanin 归零检测，归零即推就绪队列。**异**：simpler 在 scheduler 完成路径里直接调 `on_task_complete`，esl_proxy 经 `completed_queue` 交给独立 cutter——cutter/dispatch 线程分流，dispatch 只负责 poll/下发、DAG fanout 留给 cutter（lane0 还串行 `add_successors`）；simpler 用 `fetch_add(1)` 与 `fanin_count` 比，esl_proxy 用 `fetch_sub(1)` 与 0 比；fanout 列表 simpler 内嵌 slot，esl_proxy 用独立 `g_successor_buf[]`。

#### 2.1.2 Dispatch 骨架（dispatch.c 原始版本）
dispatch.c 是一个骨架，包含 'Fake Return'（无真实 MMIO 下发），仅展示接口和基本数据流。

> **与simpler的异同**：`scheduler_dispatch.cpp` 全文——esl_proxy 的骨架相当于 simpler `dispatch_subtask_to_core / dispatch_block / dispatch_shape` 的接口占位。**同**：接口分层数相同（单核→单 block→多 block shape）。**异**：simpler 是完整实现（含真实 MMIO、双 slot、MIX），esl_proxy 原始版本仅是 'Fake Return' 空壳，所有真实逻辑由 yanghaoran29 在 onboard5 分支补全。

#### 2.1.3 基础设施
- `ring_buf.h`：环形缓冲管理，O(1) 按 task_id 索引任务描述符
- `task.h`：任务描述符 `task_desc`，组织模式枚举 `org_mode_t`（SINGLE / GROUP / SPMD_SYNC / SPMD_ASYNC）
- `mpmc_queue.h` / `queue.h`：多生产者多消费者锁-free 队列
- `mem_pool.h`：内存池
- `executor.h`：执行器类型定义，2-slot ping-pong
- `conf.h`：配置参数（RING_SIZE=4096, AIC_OSTD=2 等）
- `tensor.h` / `tensormap.h`：Tensor 和 TensorMap 类型

### 2.2 具体方案

#### 2.2.1 cutter

对 [cutter.c](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c) 做了以下关键修改。

> **与simpler的异同**：`pto_scheduler.h:625 release_fanin_and_check_ready` + `scheduler_cold_path.cpp` 中的 fanout 解析。**同**：两者都在任务完成时遍历 fanout 列表递减后继计数，归零则推送就绪队列。**异**：simpler 的 fanout 列表内嵌在 slot 中、由 scheduler 内部串行遍历（无需锁）；esl_proxy 的 fanout 存在独立的 `g_successor_buf[]` 中、多 lane 可并发解析同一生产者的 fanout，因此需要 per-producer 锁（`lock_fanout`）。simpler 由 pop 串行化天然避免并发，esl_proxy 选择共享队列 + 加锁以避免 pop/push 开销。

**1. 添加 `dispatch_spmd_on_ready()` 调用**（[cutter.c:66](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c#L66)）：

```c
static inline void stage_ready(uint16_t succ_id, ...) {
    task_type_t type = g_basic_buf[succ_id & RING_MASK].type;
    dispatch_spmd_on_ready(succ_id);  // ← 添加
    rq_buf[type][ready_cnt[type]++] = succ_id;
}
```

任务变为就绪时重置 SPMD 块游标，使任务可以被重新分发。

**2. Per-producer fanout 锁**（[cutter.c:33-43](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c#L33)）：

```c
static inline void lock_fanout(uint16_t p) {
    while (atomic_flag_test_and_set_explicit(&g_fanout_lock[p & RING_MASK],
                                             memory_order_acquire)) {
        spin_wait();
    }
}
```

**为什么加锁**：当任务 T 完成时，cutter 要遍历 T 的 fanout 列表（`g_successor_buf[T]`）将 T 的每个后继的前驱计数减一。多 lane 场景下，lane A 解析生产者 P1 的 fanout、lane B 解析生产者 P2 的 fanout 可并发执行；但如果 lane A 和 lane B 同时解析**同一生产者 P**的 fanout（例如 P 有 100 个后继，被两个 lane 同时触发），不加锁会导致 fanout 链表遍历与 `g_predecessor_cnt` 递减错乱。这里用 per-producer 锁（而非全局锁）使得不同生产者的 fanout 解析可以并行——`memory_order_acquire` 与 unlock 时的 `release` 配对，保证锁内对 fanout 链表和后继计数的修改对下一个获取者可见。原始版本使用全局锁，改为 per-producer 锁后不同 lane 可以并行解析不同生产者的 fanout。

**3. 静态化与内存屏障**：函数改为 `static inline`；`add_predecessors()` 末尾添加 `wmb()`（[ring_buf.h:132](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/ring_buf.h#L132)）；`g_predecessor_cnt[]` 加 `_Atomic` 限定符。

**为什么加 `wmb()`**：`add_predecessors()` 写入前驱列表后，orch 线程会执行 `advance_task_id()`（`atomic_fetch_add(&g_task_id, release)`）发布任务 ID。cutter 线程通过 `acquire` 读到新的 `g_task_id` 后会读取该任务的前驱列表。`wmb()` 是 store-store 屏障，保证前驱列表的写入先于 `g_task_id` 的 release 写入对 cutter 可见——否则 cutter 可能读到未初始化的前驱列表，误判依赖已满足。

**为什么 `g_predecessor_cnt[]` 用 `_Atomic`**：多个前驱任务可能在不同 lane 上并发完成，每个完成事件都会对同一后继的 `g_predecessor_cnt` 执行 `atomic_fetch_sub`。非原子递减会丢失更新（两个 lane 同时读到 cnt=2，都写回 1，本应为 0），导致任务永远不就绪。对应 simpler 的 `fanin_refcount.fetch_add(1, acq_rel)`（pto_scheduler.h:629）。

**4. `advance_min_uncomplete()`**（[cutter.c:49](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c#L49)）：环形缓冲背压水位线推进。

**为什么用 `atomic` + `acquire/release`**：`g_min_uncomplete_task` 由 cutter 推进（release），由 orch 在 `new_task()` 中读取（acquire）做背压检查（`while (task_id - g_min_uncomplete_task >= RING_SIZE) spin_wait()`）。acquire/release 配对保证 orch 看到 `g_min_uncomplete_task` 推进时，对应 ring buffer slot 的任务数据已被 cutter 释放并可复用——否则 orch 可能覆写一个仍在被 dispatch 读取的 slot。

#### 2.2.2 dispatch

将原有 'Fake Return' 骨架完全重写为真实实现。

> **与simpler的异同**：`scheduler_dispatch.cpp` 的 `dispatch_subtask_to_core`（L127）、`dispatch_block`（L233）、`dispatch_shape`（L266）、`dispatch_ready_tasks`（L373）、`resolve_and_dispatch`（L449）；完成轮询对应 `scheduler_completion.cpp` 的 `poll_core_completions`（L240）。**同**：两者都采用"先轮询完成再分发新任务"的 Phase 结构，都用 COND 寄存器 + task_id 序列号区分 running/pending 双 slot。**异**：simpler 将 dispatch/poll 逻辑集成在 `SchedulerContext` 类中（C++），esl_proxy 拆分为独立的 dispatch 线程函数（C11）；simpler 的双 slot 分 IDLE/PENDING 两个阶段分别 dispatch，esl_proxy 合并为 `send_task` + `dispatch_prefetch` 两次调用。

##### A. 任务下发

**1. 真实 MMIO 下发**：替换 Fake Return 为 `esl_prepare_subtask_to_core()` + `esl_publish_subtask_to_core()`。

> **与simpler的异同**：`dispatch_subtask_to_core`（scheduler_dispatch.cpp:127-205）——`build_payload` + `wmb()` + `write_reg(DATA_MAIN_BASE)`。**同**：两者都遵循"准备 payload → wmb → 写 DATA_MAIN_BASE doorbell"三步序列。**异**：simpler 的 payload 组装在 `build_payload()` 中（C++ 对象，含 `async_ctx`/`deferred_slab`），esl_proxy 由 `dispatch_payload.c` 用扁平 `data[]`/`scalar[]` 数组组装（C 结构体，无 async 上下文）；simpler 每次 dispatch 单个 `wmb()`，esl_proxy 批量 prepare 后合并为一次 `wmb()`。

**2. COND 寄存器轮询**（[dispatch.c:176](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L176)）：

```c
void dispatch_poll(int tid) {
    // 遍历所有在飞slot，读COND寄存器检测FIN/ACK
    // 单slot：直接匹配task_id + FIN state
    // 双slot：用序列号区分running/pending
}
```

> **与simpler的异同**：`scheduler_completion.cpp:240 poll_core_completions`——读 `*core.cond_ptr` 后 `rmb()`，再 `decide_slot_transition` 判断 running/pending 状态。**同**：两者都缓存 `cond_ptr` 避免每次重算地址，都在读 COND 后加 `rmb()` 防止 Device→Normal load 重排。**异**：simpler 的 `decide_slot_transition` 返回 4 种 case 的结构体（`pending_done`/`running_done`/`running_freed`/`pending_freed`），esl_proxy 直接在 `dispatch_poll` 中用 task_id 差值内联判断，无独立 transition 函数。

**3. Phase-1/Phase-4 结构**（[dispatch.c:689](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L689)）：

```c
while (!g_orch_is_done) {
    dispatch_poll(tid);           // Phase-1: 先轮询完成
    total_sent += dispatch(tid);  // Phase-4: 再分发新任务
    spin_wait();
}
```

**为什么先轮询再分发**：先收割 FIN 可以在同一迭代的 `dispatch()` 内（`dispatch_merge_msg_to_free`）立即回收释放的核并重新分发新任务，减少核空闲间隙。如果先分发再轮询，刚完成的核要等到下一轮迭代才能被复用，引入一个 `spin_wait` 周期的空转。

> **与simpler的异同**：`resolve_and_dispatch`（scheduler_dispatch.cpp:449）同样是"先 poll completion 再 dispatch ready tasks"的顺序。**同**：两者主循环都是 poll→dispatch 的顺序，保证同轮迭代内回收的核可立即复用。**异**：simpler 在 poll 和 dispatch 之间还插入 `on_scope_end`（批量释放 producer scope）和 drain mode 检查，esl_proxy 无 scope 概念，poll 后直接 dispatch。

**4. 批量 wmb + publish**（[dispatch.c:520](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L520)）：

```c
// prepare-all → one wmb() → publish-all（simpler flush_publish 习语）
if (np > 0) {
    wmb();
    for (int p = 0; p < np; p++) {
        esl_publish_subtask_to_core(pend[p].h);
        dispatch_mark_slot_complete(...);
    }
}
```

**为什么加 `wmb()`**：ARM64 是弱内存序架构。payload 数据（tensor 地址、scalar、block_idx）写入的是 Normal-cacheable 内存，而 `esl_publish_subtask_to_core()` 写的是 Device-nGnRnE 的 MMIO 寄存器（DATA_MAIN_BASE，doorbell）。没有 `wmb()` 时，CPU 可能将 MMIO 写重排到 payload 写之前，导致 AICore 收到 doorbell 但读到的 payload 是旧值/零值。`wmb()` 是 store-store 屏障，保证所有 payload 写入对 AICore 可见后才发出 doorbell。

**为什么批量**：多个子任务可以共享一次 `wmb()`（昂贵的屏障指令），减少屏障开销。这是 simpler 的 flush_publish 习语。

> **与simpler的异同**：`dispatch_subtask_to_core`（scheduler_dispatch.cpp:188）。**同**：两者都在 publish 前加 `wmb()` 保证 payload 可见性。**异**：simpler 每次 dispatch 单个 `wmb()`（1:1 屏障），esl_proxy 在多 block 场景下合并为一次 `wmb()` + 多次 publish（1:N 屏障），减少屏障开销。

**5. 基础 send_task 逻辑**（[dispatch.c:438](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L438)）处理普通 CUBE/VECTOR 任务（basic / double_buffer **共用同一函数**）：
1. 计算 `free_bitmap = free_bitmap[type][0] & free_bitmap[type][1]`（两个 slot 都空闲的核）
2. **仅 basic**（`#if !ESL_DISPATCH_DOUBLE_BUFFER`）：过滤 MIX 忙碌核 `dispatch_mix_core_busy(core)`
3. 从 `g_shared_ready[type]` 出队任务
4. `dispatch_spmd_claim_range()` claim block 范围
5. 将 claim 的 block 扇出到空闲核：
   - 选择 slot（0 或 1）
   - 设置 `g_executors[exe_type][core]` 状态
   - `dispatch_publish_block_prepare()` 准备 payload
6. 一次 `wmb()` + 批量 `esl_publish_subtask_to_core()`
7. 失败时 `dispatch_spmd_rewind()` 回退

> **与simpler的异同**：`dispatch_shape`（scheduler_dispatch.cpp:266）的 AIC/AIV 分支——`pop_ready_tasks_batch` → `dispatch_block` → `dispatch_subtask_to_core`。**同**：两者都按 shape（CUBE/VECTOR）分别从就绪队列批量 pop 任务、扇出到空闲核。**异**：simpler 用 `CoreTracker` 管理核状态（IDLE/PENDING 两阶段），esl_proxy 用 `free_bitmap[type][slot]` 位图直接匹配；simpler 支持 `enter_drain_mode`（资源不足时排空在飞任务），esl_proxy 无 drain 机制（资源不足时 `send_task` 直接返回 0，下一轮重试）。

**6. 双 slot 完成推断**（[dispatch.c:214](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L214)，在 `dispatch_poll` 内）：

同一物理核 2 个在飞任务时，单 COND 寄存器只留最新值。通过 task_id 序列号差值判断 running/pending，看到 pending 事件即可推断 running 已完成（AICore 串行执行）。

> **与simpler的异同**：`decide_slot_transition`（scheduler_completion.cpp:46-61）——用 `running_reg_task_id` / `pending_reg_task_id` 与 COND 寄存器中的 `reg_task_id` 比对，区分 4 种 case（pending FIN、pending ACK、running FIN、running ACK）。**同**：两者都依赖 AICore 串行执行不变式——看到 pending FIN 即可推断 running 已完成，用 task_id 序列号区分双 slot。**异**：simpler 将 4 种 case 编码为 `SlotTransition` 结构体（4 个 bool 字段），esl_proxy 直接在 `dispatch_poll` 中用差值比较内联处理，无独立结构体；simpler 还处理 `AICORE_EXIT_SIGNAL` 跳过逻辑，esl_proxy 无此需求（exit 信号由平台层处理）。

**双 slot 完成推断是基础设施（正确性要求），不是双缓冲优化**。双缓冲（`dispatch_prefetch`）是主动利用第二 slot 的性能优化。两者关系：双缓冲创建 2 在飞场景，完成推断处理该场景的完成事件。对应 simpler 的 `decide_slot_transition`（scheduler_completion.cpp:46-61），由 zhusy54 在 PR #477 (2026-04-12) 引入。

##### B. MIX 任务

MIX 集群 = 1 AIC + 2 AIV per block，每个 block 发送 3 个子任务：

```
物理核映射:
  AIC  = core                    (0..23)
  AIV0 = 24 + core * 2           (24, 26, 28, ...)
  AIV1 = 24 + core * 2 + 1       (25, 27, 29, ...)
```

> **与simpler的异同**：`dispatch_mix_block_to_cluster`（scheduler_dispatch.cpp:207-231）——按 `core_mask` 依次 dispatch AIC/AIV0/AIV1。**同**：两者都按 AIC→AIV0→AIV1 顺序发布同一 block 的 3 路子任务，物理核映射公式完全一致（`AIC=core`，`AIV0=BLOCK_DIM+core*2`，`AIV1=BLOCK_DIM+core*2+1`）；任务级「全部做完」时刻等价。**异**：simpler 通过 `CoreTracker::classify_mix_cluster` 区分 RUNNING/PENDING，esl_proxy 用 `occupy_cluster`/`release_cluster` 显式占满/释放三路——避免同一集群被不同 MIX block 交叉占用；完成计数上，simpler 每路 FIN 调 `on_subtask_complete`（`total_required_subtasks = block_num × popcount(active_mask)`），esl_proxy 先 `dispatch_mix_cluster_all_done` 收齐三路 FIN 再对 block 调一次 `dispatch_spmd_note_block_done`——两级收割与 `occupy`/`release` 生命周期绑定，便于延迟清 slot（见下）。

**send_task_mix**（[dispatch.c:370](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L370)，区段 5）：
1. 快照当前 lane 的空闲 MIX 集群
2. 从 `g_shared_ready[TASK_TYPE_MIX]` 出队一个 MIX 任务
3. `dispatch_spmd_claim_range()` claim 一批 block
4. 将 block 扇出到不同集群：
   - `dispatch_mix_occupy_cluster()` 占用集群的 3 个 slot
   - `dispatch_mix_prepare_cluster()` 准备 3 个子任务 payload（AIC+AIV0+AIV1）
5. `dispatch_mix_flush()` 批量发布：一次 `wmb()` + 多次 `esl_publish_subtask_to_core()`
6. 失败时 `dispatch_spmd_rewind()` 回退游标

**为什么 `dispatch_mix_occupy_cluster()` / `release_cluster()` 需要占用/释放**：MIX 集群的 3 个核（AIC+AIV0+AIV1）必须作为一个整体调度——不能让同一集群的 AIC 被一个 MIX block 占用、AIV0 被另一个 MIX block 占用，否则两个 block 的子任务会交叉完成，无法判断哪个 block 的三路子任务全部完成。`occupy_cluster()` 在 bitmap 中原子标记集群的 3 个 slot 为忙，`release_cluster()` 在三路子任务全部 FIN 后才释放。

**为什么 `dispatch_mix_flush()` 合并 `wmb()`**：一个 MIX block 要发布 3 个子任务（AIC+AIV0+AIV1），如果每路子任务各发一次 `wmb()`，3 个 block 就要 9 次 `wmb()`。合并为"prepare 全部 → 一次 `wmb()` → publish 全部"，将屏障开销从 O(3×blocks) 降到 O(1)。

**MIX 完成跟踪**：
- `dispatch_mix_cluster_all_done()`：检查 AIC+AIV0+AIV1 三个 slot 都收到 FIN
- `dispatch_mix_harvest_completed()`：收割完成的集群，调用 `dispatch_spmd_note_block_done()`
- `dispatch_mix_defer_slot_clear()`：延迟清理 MIX slot，直到全部子任务完成

**为什么延迟清理**：AICore 完成一路子任务后会写 COND 寄存器，但同一集群的其他两路可能还在执行。如果 AIC 路完成后立即释放 slot 并复用该核发新任务，新任务的 COND 写会覆盖未完成的 AIV0/AIV1 的 FIN 事件。延迟清理确保三路全部 FIN 后才释放集群。

##### C. SPMD 任务

镜像 simpler 的 `next_block_idx` 机制，采用 pop-serialized 非原子实现。

> **与simpler的异同**：`scheduler_dispatch.cpp:337-350`。**同**：两者都用非原子 `next_block_idx += claim`，都依赖 pop 串行化保证单线程独占，都有剩余 block 推回队列的机制。**异**：simpler 的 `next_block_idx` 内嵌在 `PTO2TaskSlotState` 中（C++ 对象成员），esl_proxy 的 `g_next_block` 是独立的全局数组（C11，索引 `task_id & RING_MASK`）；simpler 的 rewind 由 push 回队列后自然重入实现，esl_proxy 有显式 `dispatch_spmd_rewind()` 函数用于 publish 失败时回退。

**问题**：SPMD 任务包含多个 block（如 Qwen3 tier4 有 522 个任务，每个任务多 block），多个 dispatch lane 需要并发 claim 同一任务的不同 block 范围。

**算法**（[dispatch.c:1182](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L1182)，区段 3，文件内 `static`）：

```c
static int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block) {
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;
    if (avail <= 0) return 0;
    uint16_t cur = g_next_block[slot];        /* 非原子读——pop-serialized */
    uint32_t start = cur;
    uint32_t n;
    if (total <= 1U) { if (start > 0U) return 0; n = 1U; }
    else { if (start >= total) return 0;
           n = ((uint32_t)avail < (total - start)) ? (uint32_t)avail : (total - start); }
    g_next_block[slot] = (uint16_t)(start + n); /* 非原子写——pop-serialized */
    if (start_block) *start_block = start;
    return (int)n;
}
```

**为什么可以非原子（对齐 simpler）**：`batch_dequeue` 从 `g_shared_ready[]` 弹出任务时由队列锁串行化——同一时刻只有一个 dispatch lane 持有该任务。claim 完成后若有剩余 block，再 `batch_enqueue` 推回队列。因此 `g_next_block` 的读写只在持有任务期间发生，无需原子。这与 simpler 的 `next_block_idx += claim`（pop 后单线程独占）完全一致。`g_finished_blocks` 仍保持 `_Atomic`（多 lane 可并发完成同一 SPMD 任务的不同 block）。

**Rewind**（[dispatch.c:1213](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L1213)）：

```c
static int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block) {
    (void)claimed_end;
    g_next_block[task_id & RING_MASK] = (uint16_t)next_block; /* 直接回退 */
    return 1;
}
```

**为什么直接写回而非 CAS**：claim 成功但 publish 失败时必须回退游标，否则被跳过的 block 永远不会执行。pop-serialized 保证同一时刻只有一个 lane 持有该任务，可直接写回 `next_block`。剩余 block 由后续 `dispatch_spmd_has_remaining()` 检测并重新 claim。

**Weak `dispatch_spmd_on_ready`**（[dispatch.c:1173](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L1173)；**唯一**对外暴露的 SPMD API，见 [dispatch.h:62](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/dispatch.h#L62)）：

```c
__attribute__((weak)) void dispatch_spmd_on_ready(uint16_t task_id) {
    const uint16_t slot = task_id & RING_MASK;
    g_next_block[slot] = 0;          /* 非原子：就绪发布前重置 */
    g_finished_blocks[slot] = 0;
}
```

**为什么非原子重置足够**：`on_ready` 在任务推入 `g_shared_ready` **之前**由 cutter 调用，此时尚无 lane claim 该任务；lane 通过队列 pop（acquire）观察到游标已为 0。`g_finished_blocks` 赋 0 同样发生在首个完成者之前。

**块完成跟踪**（[dispatch.c:1242](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L1242)）：

```c
static int dispatch_spmd_note_block_done(uint16_t task_id) {
    uint16_t prev = atomic_fetch_add_explicit(&g_finished_blocks[slot], 1,
                                               memory_order_acq_rel);
    return (uint32_t)(prev + 1U) == total;  // 最后一个块完成时返回true
}
```

**为什么用 `atomic_fetch_add`**：多个 dispatch lane 可能并发完成同一 SPMD 任务的不同 block。非原子递增会丢失计数（两个 lane 同时读到 5，都写回 6，实际应为 7），导致永远检测不到"最后一个 block 完成"。`acq_rel` 保证完成计数对所有 lane 可见，且 `prev+1 == total` 的判断在最后一个完成者上返回 true，触发 cutter 的依赖释放。

> **与simpler的异同**：`on_subtask_complete`（`completed_subtasks.fetch_add(1, acq_rel)` + `prev+1 == total_required_subtasks`）。**同**：末完成者用 `fetch_add` + `prev+1 == total` 判定整任务结束；任务完成后都会走到 fanout/就绪发布。**异（计数粒度）**：simpler 的 `total_required_subtasks = block_num × popcount(active_mask)`（内嵌 slot；每路 AIC/AIV FIN 计 1），esl_proxy 的 `total = g_basic_buf[slot].count`（block 数；MIX 先在 `dispatch_mix_cluster_all_done` 收齐三路再对该 block 计 1）——两级收割与集群 occupy/release 对齐，非「只数 MIX 三路」。**异（fanout 触发点）**：simpler 在 `on_subtask_complete` 返回 true 后同线程调 `on_task_complete`；esl_proxy 在 `dispatch_spmd_note_block_done` 返回 true 后推 `completed_queue`，由 cutter 的 `deal_completed_queue`/`resolve_dep` 做等价 fanout——dispatch 与 cutter 线程分流，避免在 poll/下发热路径上走 fanout（含 lane0 的 `add_successors`）。

##### D. 双缓冲

> **与simpler的异同**：`dispatch_subtask_to_core` 的 `to_pending` 参数（scheduler_dispatch.cpp:153-162）——`running_subslot` / `pending_subslot` 双 slot；`scheduler_completion.cpp` 的 `decide_slot_transition` 处理双 slot 完成推断。**同**：两者都支持每核 2 个在飞任务（`AIC_OSTD=2`），用 `running`/`pending` 两个字段区分 slot，都要求 pending 发送前 running 已被 ACK。**异**：simpler 在 `dispatch_ready_tasks` 中分 IDLE 阶段（写 running slot）和 PENDING 阶段（写 pending slot）两趟循环，esl_proxy 用 `send_task`（填空闲核的第一个 slot）+ `dispatch_prefetch`（填已有在飞核的第二个 slot）两次调用实现等效功能；simpler 的 ACK 检查在 `CoreTracker` 状态机中，esl_proxy 直接检查 COND 寄存器中的 ACK state。

双缓冲与 basic **共用** `send_task` / `dispatch_prefetch` / `dispatch`；差异仅局部 `#if !ESL_DISPATCH_DOUBLE_BUFFER`（basic 才做 `mix_core_busy` 过滤与 `dispatch_mix_prefetch`）。Sim：`make DISPATCH=double_buffer` → `-DESL_DISPATCH_DOUBLE_BUFFER=1`；Onboard：`ESL_PROXY_DOUBLE_BUFFER=ON` 同宏。不再维护独立的 `dispatch_double_buffer.c`。

`dispatch_prefetch()`（[dispatch.c:544](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L544)）：当核有一个 slot 在飞、另一个空闲时，预取下一个任务到空闲 slot：
1. 检查核有一个 busy slot + 一个 free slot
2. **仅 basic**：若 `dispatch_mix_core_busy(core)` 则 skip
3. 验证 busy slot 已被 ACK（硬件已接收）
4. 从就绪队列出队一个任务
5. `dispatch_spmd_claim_block()` claim 单个 block
6. `dispatch_publish_block()` 发布到空闲 slot

**为什么验证 busy slot 已被 ACK**：AICore 的 2-slot ping-pong 要求两个 slot 交替使用。如果 busy slot 还没被 ACK（AICore 还没开始执行），此时往 free slot 发新任务，AICore 可能还在读 busy slot 的 payload。等到 ACK 到达后 AICore 才确认接收了第一个任务，之后才能安全切换到第二个 slot。`ACK` 状态表示"AICore 已硬件锁存了 slot-0 的 task_id，可以安全写 slot-1"。

**双 slot 完成推断**（同 §2.2.2-A.6，[dispatch.c:214](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L214)）：

**为什么用序列号区分 running/pending**：每个物理核只有一个 COND 寄存器，AICore 完成任务后写 COND（覆盖前一个事件）。当核上有 2 个在飞任务（running + pending）时，COND 寄存器只保留最后写入的值。通过 task_id 序列号差值判断：如果 COND 中的 task_id == pending 的 task_id 且 state == FIN，说明 pending 已完成；由于 AICore 串行执行（pending 必须在 running 完成后才开始），pending FIN 意味着 running 也已完成。看到 pending FIN 即可同时收割两个 slot。

#### 2.2.3 conf.h 修改

- 添加 `ESL_LANE_CNT`、`CORE_LANE()` 宏（[conf.h:24-43](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L24)）
- 强制 `ESL_ORCH_FIRST=0`（[conf.h:31](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L31)）
- 添加 `_Static_assert` 保证 cutter/dispatch 线程 1:1 配对
- `ESL_DISPATCH_DOUBLE_BUFFER`（[conf.h:50](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h#L50)，默认 0）：选择 basic vs double_buffer 策略差异

#### 2.2.4 Dispatch 主循环

`dispatch()`（[dispatch.c:617](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L617)）每次迭代执行：
```
1. dispatch_merge_msg_to_free()     ← 合并 msg_bitmap 到 free_bitmap（原 get_free_exe 已内联）
2. dispatch_push_completed_slots()  ← 收割完成任务并入 completed_queue（原 push_2_completed_queue 已内联）
3. send_task_mix()                  ← MIX 集群分发
4. [basic only] dispatch_mix_prefetch()  ← MIX 2-outstanding 填充
5. send_task(VECTOR) / send_task(CUBE)
6. dispatch_prefetch(VECTOR) / dispatch_prefetch(CUBE)
```

> **与simpler的异同**：`dispatch_ready_tasks`（scheduler_dispatch.cpp:373）。**同**：两者都按 MIX→CUBE/VECTOR 优先级分发，MIX 优先确保集群完整性。**异**：simpler 分 IDLE 阶段（填空核 running slot）和 PENDING 阶段（填在飞核 pending slot）两趟循环，还有 `has_idle_in_other_threads` 跨线程负载均衡门控；esl_proxy 将 IDLE/PENDING 合并为单次 `send_task`（填空核）+ `dispatch_prefetch`（填在飞核第二 slot），无跨线程门控。

#### 2.2.5 平台层 L0-L3 新增分层

| 层级 | 文件 | 职责 |
|------|------|------|
| L0 (HAL) | platform_regs.c, sim_core_regs.c | 寄存器读写原语（共享内存/mmap） |
| L1 (共享契约) | platform_config.h, worker_map.h | 拓扑/寄存器布局/任务编码 |
| L2 (后端选择) | platform.h | 编译时选择 sim 或 onboard |
| L3 (设备执行) | aicore.c, device_runner.c, aicpu_runtime.c | AICore 仿真/真实执行 |

**为什么分层**：sim 和 onboard 两种后端共享拓扑定义、寄存器布局、任务 ID 编码（L1），但寄存器访问原语（L0）和设备执行（L3）不同。L1 作为"契约层"保证两种后端的行为一致——sim 上验证的逻辑可以直接移植到 onboard，只需替换 L0/L3 实现。

#### 2.2.6 AICPU-AICore 握手状态机（handshake.c）

新增的 AICPU-AICore 通信机制：
- `esl_handshake_reg_addr()`：缓存物理核的寄存器地址
- `esl_handshake_cond_ptr()`：缓存 COND 寄存器指针
- `esl_prepare_subtask_to_core()`：准备子任务 payload
- `esl_publish_subtask_to_core()`：发布子任务到核（doorbell）

**为什么缓存 `cond_ptr`**：`dispatch_poll()` 每次迭代都要轮询所有在飞核的 COND 寄存器。每次都通过 `reg_offset + reg_addr` 计算指针会引入额外加法/移位开销。预先缓存指针将热路径简化为单次解引用。

> **与simpler的异同**：`scheduler_completion.cpp:254-255`——"precomputed cond_ptr (resolved once in handshake)"。**同**：两者都在握手阶段预缓存 `cond_ptr`，将热路径轮询简化为单次解引用。**异**：simpler 的握手逻辑内嵌在 `SchedulerContext` 初始化中，esl_proxy 用独立的 `handshake.c` 模块封装（`esl_handshake_reg_addr` / `esl_handshake_cond_ptr`）。

#### 2.2.7 Cache 维护（cache_ops.c）

```c
// 内存屏障
wmb();                           // 写屏障
OUT_OF_ORDER_LOAD_BARRIER();     // 乱序加载屏障
// Onboard: dc civac（数据缓存清理+无效化）
```

**为什么需要 `wmb()`（写屏障）**：ARM64 弱内存序下，Normal-cacheable 内存写（payload 数据）可能被 CPU 重排到 Device-nGnRnE MMIO 写（doorbell）之后。`wmb()` 是 store-store 屏障，保证 payload 写入先于 doorbell 写入对 AICore 可见。对应 simpler `scheduler_dispatch.cpp:188` 的 `wmb()`。

**为什么需要 `OUT_OF_ORDER_LOAD_BARRIER()` / `rmb()`（读屏障）**：ARM64 允许 Device-nGnRnE → Normal-cacheable 的 load 重排。`dispatch_poll()` 先读 COND 寄存器（Device）检测 FIN，再读任务的 payload/状态（Normal）。如果 CPU 将后续 Normal 读重排到 COND 读之前，可能读到 FIN 之前的旧 payload。`rmb()` 是 load-load 屏障，保证 COND 读完成后才读 payload。对应 simpler `scheduler_completion.cpp:261` 的 `rmb()`。

**为什么 Onboard 需要 `dc civac`**：AICPU 和 AICore 在真实 NPU 上可能有独立的 L1/L2 cache。AICPU 写 payload 到 Normal-cacheable 内存后，数据可能停留在 AICPU 的 L1/L2 cache 中，尚未刷到共享内存。AICore 从自己的 cache 层级读取时，可能命中旧值的 cache line。`dc civac`（Data Cache Clean + Invalidate by Set/Way）做两件事：
1. **Clean**：将 AICPU L1/L2 的脏 cache line 写回共享内存，使 payload 对 AICore 可见。
2. **Invalidate**：使 AICore cache 中对应 cache line 失效，强制 AICore 从共享内存重新加载最新 payload。

不执行 `dc civac` 的后果：AICore 收到 doorbell 但读到旧的/零值 payload，导致计算错误或崩溃。sim 后端不需要 `dc civac`（单进程内 pthread 共享同一地址空间，cache 一致性由硬件保证）。

> **与simpler的异同**：`simpler/src/common/platform/onboard/aicpu/cache_ops.cpp`。**同**：两者都在 onboard 后端实现 `dc civac`，都在 sim 后端省略（pthread 共享地址空间，cache 一致性由硬件保证）。**异**：simpler 的 `wmb()`/`rmb()` 内嵌在 `dispatch_subtask_to_core` 和 `poll_core_completions` 中（C++），esl_proxy 由独立的 `cache_ops.c` + `memory_barrier.h` 提供（C11 宏/内联函数），便于跨后端复用。

## 3. 运行平台

### 3.1 公共平台

#### 3.1.1 72 核拓扑

```
Block Dim = 24 (ESL_PROXY_WORKER_BLOCK_DIM)
每 block: 1 AIC + 2 AIV = 3 核
总核数: 24 AIC + 48 AIV = 72 workers

逻辑核 → 物理核映射 (esl_pick_phys_worker):
  CUBE  (exe_type=0): phys = core                    → AIC 0..23
  VECTOR(exe_type=1): phys = 24 + core*2 + lane      → AIV 0..47 (round-robin)
```

定义见 [worker_map.h:23-46](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/worker_map.h#L23)。

#### 3.1.2 寄存器布局

**COND 寄存器**（32-bit）：
```
bit 31    : 状态 (0=ACK, 1=FIN)
bits 0-30 : 任务 ID
```

**任务 ID 编码**（[platform_config.h:80-88](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/platform/platform_config.h#L80)）：

```c
#define TASK_ID_MASK    0x7FFFFFFFU
#define TASK_STATE_MASK 0x80000000U
#define EXTRACT_TASK_ID(regval)    ((int)((regval) & TASK_ID_MASK))
#define EXTRACT_TASK_STATE(regval) ((int)(((regval) & TASK_STATE_MASK) >> 31))
#define MAKE_ACK_VALUE(task_id)    ((uint64_t)((task_id) & TASK_ID_MASK))
#define MAKE_FIN_VALUE(task_id)    ((uint64_t)(((task_id) & TASK_ID_MASK) | TASK_STATE_MASK))
```

**时钟**：50 MHz（`PLATFORM_PROF_SYS_CNT_FREQ`），1 cycle = 20 ns

### 3.2 Sim 后端

Sim 后端在主机 CPU 上用 pthread 仿真整个系统：

**Instant 模式**（默认，`SIM_AICORE=instant`）：
- 无 AICore pthread，任务提交后即时 FIN
- 1 manager 线程处理所有完成
- 用于功能验证和 DAG 正确性测试

**Threaded 模式**（`SIM_AICORE=threaded`）：
- 72 个 pthread 仿真 AICore
- `fake_kernel_run()` 提供时序仿真
- 用于性能评估和泳道 trace 生成

寄存器仿真通过共享内存表（`sim_core_regs.c`）。cache 操作通过内存屏障仿真（`wmb()` / `rmb()` 仍执行以保证代码路径与 onboard 一致，但 sim 单进程内 pthread 共享地址空间，cache 一致性由硬件保证，不需要 `dc civac`——详见 §2.2.7）。

### 3.3 Onboard 后端

Onboard 后端在真实 Ascend NPU 上运行：

- **AICPU 线程启动**：通过 CANN `rtsLaunchCpuKernel()` 启动 AICPU 线程
- **角色自分配**：AICPU 线程通过线程索引自分配为 cutter / dispatch / orch
- **真实 MMIO**：通过 `ascend_hal.h` 访问硬件寄存器
- **真实 Cache 维护**：`dc civac` 指令清理+无效化数据缓存（为什么需要见 §2.2.7）
- **CANN 依赖**：`acl/acl_rt.h`，`dlog_pub.h`

```
AICPU 线程约束:
  2*ESL_LANE_CNT + 1 <= PLATFORM_MAX_AICPU_THREADS (4)
  → 并行式模型仅支持单 lane (3 线程)
```

### 3.4 运行方式

**Sim 运行**：
```bash
cd esl_proxy/esl_proxy
make                                    # 默认: qwen3, tier=0, instant
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=2 run
make DISPATCH=double_buffer run         # 双缓冲模式
make SIM_AICORE=threaded run            # 线程化仿真
make LANE_CNT=2 run                     # 2 lane
```

**Onboard 运行**：
```bash
# 通过 CMake 构建系统编译（需要 CANN SDK）
# 使用 tools/run_onboard.sh 部署到 NPU
```

## 4. 跟 simpler 的对比

### 4.1 模块映射

| esl_proxy | simpler | 对应关系 |
|-----------|---------|----------|
| [dispatch.c](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c) | scheduler_dispatch.cpp | 核心调度（单文件；区序 0125643；basic/double_buffer 局部 `#if`，无独立 double_buffer 源文件） |
| [cutter.c](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/cutter.c) | scheduler_dispatch.cpp (DAG部分) | DAG 依赖解析 |
| [executor.h](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/executor.h) | PTO2TaskSlotState | 执行器状态（2-slot） |
| [ring_buf.h](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/ring_buf.h) | ringbuffer | 任务描述符存储 |
| [task.h](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/task.h) | PTO2TaskDesc | 任务描述符 |
| [conf.h](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/include/algorithm/conf.h) | scheduler_config.h | 配置参数 |
| [handshake.c](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/handshake.c) | aicore_mailbox | AICPU-AICore 握手 |
| platform/ 层 | platform/ | 平台抽象 |
| dispatch_payload.c | payload assembly | 子任务 payload 组装 |

### 4.2 算法对应

#### 4.2.1 SPMD Range-Claim

**simpler**（scheduler_dispatch.cpp:337-350）：
```cpp
// 非原子 — 任务 slot 从就绪队列被一个线程 pop 出来，单线程访问
int32_t remaining = slot_state->logical_block_num - slot_state->next_block_idx;
int32_t claim = std::min(available, remaining);
int32_t start = slot_state->next_block_idx;
slot_state->next_block_idx += claim;  // 非原子递增
if (slot_state->next_block_idx < slot_state->logical_block_num) {
    sched_->ready_queues[shape].push(slot_state);  // 剩余块放回队列
}
```

**esl_proxy**（[dispatch.c:1182](file:///data/y00955915/Desktop/esl_proxy_main/esl_proxy/esl_proxy/src/algorithm/dispatch.c#L1182)）：
```c
// 非原子 — batch_dequeue 队列锁串行化 pop，claim 期间单 lane 独占
uint16_t cur = g_next_block[slot];
/* ... 计算 n ... */
g_next_block[slot] = (uint16_t)(start + n);
if (remaining) batch_enqueue(...);  // 有剩余则推回共享就绪队列
```

| 特性 | simpler | esl_proxy |
|------|---------|-----------|
| 原子性 | 非原子（pop 后单线程访问） | 非原子（pop-serialized，对齐 simpler） |
| 队列操作 | pop → claim → push 回 | pop → claim → 有剩余则 push 回 |
| 回退机制 | 无显式 rewind | 直接写回 `dispatch_spmd_rewind` |
| 多 lane 支持 | 通过 pop 串行化 | 通过 pop 串行化 |

#### 4.2.2 MIX 集群

两者实现相同模式：
- 集群 = 1 AIC + 2 AIV per block
- `dispatch_mix_aic_phys(core)` = core
- `dispatch_mix_aiv0_phys(core)` = BLOCK_DIM + core * 2
- `dispatch_mix_aiv1_phys(core)` = BLOCK_DIM + core * 2 + 1
- 批量 prepare + 单次 wmb + 批量 publish（flush_publish 习语）

#### 4.2.3 DAG 依赖解析与任务完成 handoff

**simpler**：completion 路径上 `on_subtask_complete` →（末完成者）同线程 `on_task_complete` → `release_fanin` → `ready_queues`

**esl_proxy**：dispatch 上 `dispatch_spmd_note_block_done` →（末完成者）`completed_queue` → cutter `deal_completed_queue` → `resolve_dep` / `stage_ready` → `g_shared_ready`

职责等价（fanin 归零后推就绪）；调用点不同是因为 esl_proxy 的 cutter/dispatch 分流。两者都使用 per-shape 共享就绪队列。

#### 4.2.4 双缓冲

两者都支持每核 2 个在飞任务（`AIC_OSTD=2`）：
- simpler: `async_wait_list` + `deferred_release`
- esl_proxy: `dispatch_prefetch` 填充第二 slot + 序列号区分 running/pending

### 4.3 关键差异

| 方面 | esl_proxy | simpler |
|------|-----------|---------|
| 语言 | C11 (`_Atomic`, `memory_order`) | C++ (`std::atomic`) |
| SPMD claim | 非原子 range-claim（pop 串行化，对齐 simpler） | 非原子（pop 串行化） |
| 线程模型 | 独立 cutter / dispatch / orch 线程 | 集成在 scheduler_dispatch |
| 平台抽象 | L0-L3 分层，Sim + Onboard 双后端 | 仅 Onboard |
| Sim 支持 | 完整 Sim 后端（instant + threaded） | 无 |
| 完成轮询 | running/pending 序列号推断 | async_wait_list 轮询 |
| 内存模型 | C11 `_Atomic` + `memory_order` | C++ `std::atomic` |
| DAG 锁 | per-producer fanout lock | 内置在 scheduler 逻辑中 |
| dispatch 模式 | 单 `dispatch.c` + `ESL_DISPATCH_DOUBLE_BUFFER` 局部 `#if` | N/A（仅一种调度实现） |

### 4.4 esl_proxy 的独特优势

1. **Sim/Onboard 双后端**：可在主机 CPU 上完整仿真，包括 72 核 pthread 时序仿真
2. **SPMD pop-serialized claim**：与 simpler 同构的非原子游标 + 显式 rewind，多 lane 靠队列锁串行化
3. **L0-L3 分层平台**：清晰的平台抽象，sim/onboard 共享 L1 契约
4. **单文件 dispatch + 宏切模式**：basic/double_buffer 不拆源文件，差异收敛在少量 `#if`
5. **公开 API 收敛**：`dispatch.h` 仅暴露 worker/poll/init/`spmd_on_ready`；MIX/SPMD 助手停留在 `dispatch.c` 内

## 附录：泳道图

重构后重新生成的 DAG 泳道图（8 个）：

| 模式 | Case | 依赖边数 | 任务类型数 | SVG 大小 |
|------|------|---------|-----------|---------|
| basic | qwen3_dynamic_manual_scope | 3030 | 3096 | 2.3MB |
| basic | qwen3_dynamic_tensormap | 12069 | 3096 | 6.1MB |
| basic | paged_attention_unroll | 1440 | 1920 | 1.3MB |
| basic | paged_attention_unroll_manual_scope | 1440 | 1920 | 1.3MB |
| double_buffer | qwen3_dynamic_manual_scope | 3032 | 3096 | 2.3MB |
| double_buffer | qwen3_dynamic_tensormap | 23006 | 3096 | 10.5MB |
| double_buffer | paged_attention_unroll | 1440 | 1920 | 1.3MB |
| double_buffer | paged_attention_unroll_manual_scope | 1440 | 1920 | 1.3MB |

文件位置：
- basic 模式：`esl_proxy/report/dag/`
- double_buffer 模式：`esl_proxy/report/dag_double_buffer/`
