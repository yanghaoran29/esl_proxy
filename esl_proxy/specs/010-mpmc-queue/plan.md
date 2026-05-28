# 实现计划：MPMC 队列（BlkRing 非阻塞）

**分支**：`010-mpmc-queue` | **日期**：2026-05-26 | **规格**：[link](spec.md)

**输入**：使用 blkring（块环）非阻塞实现的无锁 MPMC 队列

## 摘要

一个使用 C11 atomics 并采用 blkring（块环形缓冲区）非阻塞设计的有界多生产者多消费者（MPMC）队列。blkring 方法使用原子操作进行槽位状态管理，而非传统的 head/tail 索引，从而在有界容量下实现真正的非阻塞行为。二维 ReadyQueue 矩阵（task_type × org_mode）用于任务下发。全局 CompleteQueue 用于记录任务完成。所有队列类型共用单个头文件。

**实现**：采用原子槽位状态追踪的 BlkRing 非阻塞实现

## 技术上下文

**语言/版本**：C11 (`-std=c11`)

**主要依赖**：仅标准 C 库（`<stdint.h>`、`<stdatomic.h>`、`<stdbool.h>`、`<stddef.h>`、`<stdlib.h>`、`<string.h>`）

**存储**：固定容量的内存 BlkRing 循环缓冲区

**测试**：通过依赖注入进行单元测试

**目标平台**：跨平台（Linux/macOS）

**项目类型**：用于 DAG 调度的纯头文件 C 库

**性能目标**：
- O(1) 入队与出队
- 支持 4+ 生产者与 4+ 消费者并发
- 批量操作每次调用处理 10+ 项目
- 真正的非阻塞（无 compare-and-swap 重试循环）

**约束**：
- 采用原子槽位状态的 BlkRing 非阻塞设计
- 仅使用 C11 atomics（热路径中无 mutex）
- 所有输入假设有效（Trust the Caller）
- 命名遵循 Constitution XI（无冗余前缀）
- 纯头文件库设计，仅用一个 .c 文件存放全局定义
- 总计 1 个头文件 + 1 个 c 文件
- 默认容量：每个队列 1024

**规模/范围**：
- 队列容量：100-10000（可配置，默认 1024）
- 12 个 ReadyQueue（3 种任务类型 × 4 种 org mode）
- 1 个 CompleteQueue
- 涵盖 MPMC + ReadyQueue + CompleteQueue 的 12 个用户故事

## 章程检查

*门槛：阶段 0 研究之前必须通过。阶段 1 设计之后须重新检查。*

| 原则 | 合规要求 |
|-----------|----------------------|
| Modern C11 | 仅 C11 标准（`-std=c11`）；必须使用 `_Generic`、atomics、`restrict` 指针；禁止不安全做法 |
| Callback-Based Async Architecture | 所有 API 异步并使用回调；热路径中不阻塞；函数指针 + userdata 取代 C++ lambda |
| DAG-Based Task Scheduling | 所有任务构成 DAG；环为缺陷；调度器须遵守依赖顺序；要求 Work-Stealing |
| Zero-Copy Task Data Flow | 缓冲区描述符（指针+大小）、共享内存、就地变换；拷贝需要基准测试证明 |
| Lock-Free Concurrency | 要求 C11 atomics；热路径中禁止 mutex/spinlock；用于任务分发的无锁 SPSC 队列 |
| No Blocking in Hot Paths | 无同步 I/O 或阻塞等待；所有等待均异步并入队续延任务；要求有界超时 |
| Deterministic Scheduling | 相同 DAG+输入产生相同结果；禁止隐藏的全局状态（时间、随机、环境变量） |
| Testability & Reproducibility | 通过函数指针的依赖注入；要求支持 mock 调度器；鼓励混沌测试 |
| Header-Only Library | 所有实现位于头文件中；`static inline` 函数；无二进制依赖 |
| Trust the Caller | 所有输入假设正确；不验证、不处理异常、不测试边缘情况；非法输入时行为未定义 |

**理由**：这是一个采用 Work-Stealing 调度器的 C 语言高性能异步 DAG 引擎。纯头文件 C 设计可确保最大化内联并消除链接开销。BlkRing 在无 CAS 重试循环的情况下提供非阻塞保证。

## 项目结构

### 源码（include/dag/）

```text
include/dag/
├── mpmc_queue.h     # All queue APIs (MPMC, ReadyQueue matrix, CompleteQueue) - BlkRing non-block
└── mpmc_queue.c    # Global queue instance definitions only
```

**BlkRing 非阻塞设计**：
- 每个槽位拥有原子状态（EMPTY/FILL/COMPLETE）
- 入队向槽位写入数据并原子地将状态更新为 FILL
- 出队读取槽位状态并原子地标记 COMPLETE，然后置为 EMPTY
- 无 compare-and-swap 重试循环——每次状态转换仅一次原子操作
- 生产者和消费者索引追踪槽位以实现 O(1) 访问

**默认容量**：
- ReadyQueue：每队列 1024（12 个队列 ≈ 总缓冲区 12KB）
- CompleteQueue：1024

## 复杂度跟踪

> **仅当章程检查存在需要说明的违规时填写**

| 违规 | 为何需要 | 拒绝更简方案的原因 |
|-----------|------------|-------------------------------------|
| BlkRing 复杂度 | 无 CAS 重试的真正非阻塞 | 简单的原子 head/tail 在争用下存在 CAS 重试 |
