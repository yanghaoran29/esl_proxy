# 实施计划：Dispatch

**分支**：`008-task` | **日期**：2026-05-25 | **规格说明**：[link](spec.md)

**输入**：Dispatch 组件，使用 taskID（2 字节）、后继存储（数量 + ID 列表）、每节点 3 个 taskID。

## 概述

Dispatch 通过共享内存分发任务，按工作线程管理 Executor 池（60 个 CUBE + 60 个 VECTOR），并实现 Work-Stealing 进行负载均衡。任务数据使用 16 位 TaskID 与紧凑的后继存储结构。

## 技术背景

**语言/版本**：C11（`-std=c11`）

**主要依赖**：仅标准 C 库

**存储**：共享内存中的环形缓冲区（4096 个槽位，O(1) 访问）

**测试**：通过依赖注入进行单元测试；混沌测试

**目标平台**：跨平台（Linux/macOS）

**项目类型**：用于 DAG 调度的仅头文件 C 库

**性能目标**：
- 任务下发延迟：<10 μs
- 共享内存访问：<1 μs
- Work-Stealing 重分配：<100 μs

**约束条件**：
- 热路径上无 mutex/spinlock
- 假设所有输入均有效（Trust the Caller）
- 环形缓冲区大小固定为 4096（2 的幂）
- 仅头文件 —— 所有实现位于头文件中，无 .c 文件

**规模/范围**：
- DAG 中最多 10,000 个任务
- 每个工作线程 60 个 CUBE + 60 个 VECTOR Executor
- TaskID：16 位（2 字节），最大值 65535
- 后继存储：数量（1 字节）+ 最多 3 个后继 ID（每个 2 字节）
- 节点大小：8 字节（3 × 2B taskID + 1B 后继数量 + 填充）

## 章程检查

*门控：必须在第 0 阶段研究之前通过。*

| 原则 | 合规要求 |
|-----------|----------------------|
| Modern C11 | 仅 C11 标准；`_Generic`、原子操作、`restrict` |
| Callback-Based Async | 通过原子位完成通知；函数指针 |
| DAG-Based Task Scheduling | DAG 结构；Work-Stealing 调度器 |
| Zero-Copy Task Data Flow | 环形缓冲区中的缓冲区描述符 |
| Lock-Free Concurrency | 仅使用 C11 原子操作；热路径无 mutex |
| No Blocking in Hot Paths | 无同步 I/O；带续延的异步等待 |
| Deterministic Scheduling | 相同 DAG + 输入 → 相同结果 |
| Testability | 通过函数指针进行依赖注入 |
| Header-Only Library | 所有实现位于头文件中；`static inline` 函数 |
| Trust the Caller | 不做校验；非法输入下行为未定义 |

## 项目结构

```text
specs/004-dispatch/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── checklists/
    └── requirements.md
```

### 源代码 (include/dag/)

```text
include/dag/
├── dag.h                    # Core DAG types
├── dag_task.h              # Task descriptor (2B taskID)
├── dag_task_types.h        # CUBE/VECTOR/MIX
├── dag_org_modes.h        # SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC
├── dag_ringbuffer.h        # Ring Buffer implementation
├── dag_ringbuffer_ring.h   # 4 ring buffers: basic, successors, io, state
├── dag_executor.h          # Executor with 2-slot cache
├── dag_dispatch.h          # Dispatch with Work-Stealing
└── dag_spmd.h               # SPMD barrier synchronization
```

**仅头文件强制要求**：所有源文件均为头文件（`.h`）。无 `.c` 实现文件。

## 第 0 阶段：研究

1. **TaskID 16 位打包**：TaskID 适配 2 字节，可表达 65535 个唯一 task ID
2. **紧凑后继存储**：后继节点存储数量（1 字节）+ 后继 ID 列表
3. **节点容量**：单个槽位存储 3 个 taskID（6 字节）+ 后继数量（1 字节）= 至少 7 字节

## 第 1 阶段：设计

### Task ID 结构

```c
typedef uint16_t dag_task_id_t;  // 2 bytes, max 65535
```

### 后继节点结构

```c
struct dag_successor_node {
    uint8_t   successor_cnt;     // 1 byte: number of successors (max 3)
    dag_task_id_t successors[3]; // 3 × 2 bytes = 6 bytes
};
// Total: 7 bytes minimum, aligned to 8 bytes
```

### 紧凑存储布局

| 字段 | 大小 | 范围 |
|-------|------|-------|
| task_id | 2 字节 (uint16_t) | 0 - 65535 |
| successor_cnt | 1 字节 (uint8_t) | 0 - 3 |
| successors[] | 3 × 2 字节 | 3 × (0 - 65535) |

### 关键设计决策

1. **16 位 TaskID**：紧凑表达节省环形缓冲区中的内存
2. **嵌入式后继数量**：无需单独查找
3. **固定 3 个后继上限**：对大多数 DAG 工作负载足够；简化实现
4. **环形缓冲区索引**：`task_id & 0x0FFF` 在 4096 槽位环形缓冲区中提供 O(1) 访问

---

**状态**：计划完成。可执行 `/speckit-tasks`。
