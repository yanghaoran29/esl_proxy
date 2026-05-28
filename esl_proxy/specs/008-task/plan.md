# 实施计划：Task

**分支**：`008-task` | **日期**：2026-05-26 | **规格说明**：[link](spec.md)

**输入**：Task 功能，其任务描述符仅包含描述信息。任务类型 CUBE/VECTOR/MIX，组织模式 Single/Group/SPMD_SYNC/SPMD_ASYNC。依赖信息：后继数量、后继节点、前驱数量。后继存储：基础条目（3 个内联）+ 通过 2 字节 next 指针的扩展条目。运行时信息：输入/输出数据地址、kernel 地址。命名按宪法第 XI 条不使用 `dag` 前缀。

## 概要

Task 是 DAG 引擎中的基本执行单元。任务描述符仅包含任务描述信息（id、type、mode、kernel、base index、count、prio、data）。单独存储的依赖信息包含后继数量、后继节点与前驱数量。单独存储的运行时信息包含输入/输出数据地址与 kernel 地址。命名按宪法第 XI 条避免使用 `dag` 前缀。

## 技术背景

**语言/版本**：C11（`-std=c11`）

**主要依赖**：仅标准 C 库

**存储**：4 个独立的 Ring Buffer（由 ring buffer 组件管理）：
- Task State Ring Buffer
- Task Basic Info Ring Buffer
- Task Dependency Ring Buffer
- Task Runtime Info Ring Buffer

**测试**：通过依赖注入进行单元测试

**目标平台**：跨平台（Linux/macOS）

**项目类型**：用于 DAG 调度的仅头文件 C 库

**性能目标**：
- 任务访问：通过 TASKID & RING_SIZE 实现 O(1)
- 紧凑存储：16 位 TaskID

**约束**：
- 仅头文件——所有实现都在头文件中，无 .c 文件
- 任务描述符仅包含描述字段，不含执行状态
- 命名按宪法第 XI 条不使用 `dag` 前缀

**规模/范围**：
- TASKID：16 位（2 字节）
- 后继存储：基础条目（3 个内联）+ 通过 2B next 指针的扩展条目

## 宪法检查

*门禁：必须在 Phase 0 研究前通过。*

| 原则 | 合规要求 |
|-----------|----------------------|
| Modern C11 | 仅 C11 标准；`_Generic`、原子操作、`restrict` |
| Callback-Based Async | 通过原子位完成；函数指针 |
| DAG-Based Task Scheduling | DAG 结构；Work-Stealing 调度器 |
| Zero-Copy Task Data Flow | Ring Buffer 中的缓冲区描述符 |
| Lock-Free Concurrency | 仅 C11 原子操作；热路径中无 mutex |
| No Blocking in Hot Paths | 无同步 I/O；带 continuation 的异步等待 |
| Deterministic Scheduling | 相同 DAG + 输入 → 相同结果 |
| Testability | 通过函数指针进行依赖注入 |
| Header-Only Library | 全部实现在头文件中；`static inline` 函数 |
| Trust the Caller | 不做校验；非法输入时未定义行为 |
| Concise Naming | 无冗余前缀；无 `dag` 前缀；名称在上下文内 |

## 项目结构

### 源代码 (include/dag/) - 仅 Task 相关

```text
include/dag/
└── task.h             # Single header: task_desc, task_type_t, org_mode_t, helpers
```

**仅头文件强制要求**：所有源文件均为头文件（`.h`）。无 `.c` 实现文件。

**命名约定**：文件名仅在头文件保护宏中使用 `dag_` 前缀。类型名与函数名不使用 `dag_` 前缀。

## Phase 1：设计

### 任务描述符字段（仅描述）

```c
struct task_desc {
    uint16_t    id;        // 2 bytes - Task identifier
    task_type_t type;      // CUBE/VECTOR/MIX
    org_mode_t  mode;      // SINGLE/GROUP/SPMD_SYNC/SPMD_ASYNC
    void       *kernel;    // KERNEL code pointer
    uint32_t    index;     // base INDEX for SPMD
    uint32_t    count;     // number of instances
    uint32_t    prio;      // scheduling priority
    void       *data;      // user context pointer
};
// NO execution state fields
```

### 任务类型枚举

```c
typedef enum {
    TASK_TYPE_CUBE   = 0,
    TASK_TYPE_VECTOR = 1,
    TASK_TYPE_MIX    = 2,
} task_type_t;
```

### 组织模式枚举

```c
typedef enum {
    ORG_MODE_SINGLE     = 0,
    ORG_MODE_GROUP      = 1,
    ORG_MODE_SPMD_SYNC  = 2,
    ORG_MODE_SPMD_ASYNC = 3,
} org_mode_t;
```

### 依赖信息结构

```c
// Base entry: 3 inline successors + overflow pointer
struct dep_base {
    uint16_t succ[3];   // 3 inline successor TaskIDs
    uint16_t next;      // 2B pointer to extension entry (0 = none)
    uint16_t pred_cnt;  // Predecessor count
    uint16_t succ_cnt;  // Successor count
};
```

- **后继数量**：直接后继节点的数量
- **后继节点**：后继 TaskID 列表（基础条目 3 个内联 + 通过 2B next 指针的扩展）
- **前驱数量**：直接前驱节点的数量

### 运行时信息（Runtime Ring Buffer）

- **输入数据地址**：指向输入数据缓冲区的指针
- **输出数据地址**：指向输出数据缓冲区的指针
- **Kernel 地址**：指向要执行的 kernel 代码的指针

### 关键设计决策

1. **单一头文件**：所有任务类型与结构体集中在一个 `task.h` 头文件
2. **任务描述符仅含描述**：无状态、无完成状况、无 executor 分配
3. **TaskID 16 位**：紧凑表示以提高 Ring Buffer 效率
4. **依赖信息独立存储**：后继数量、后继节点、前驱数量存储在 successor Ring Buffer 中
5. **运行时信息独立存储**：输入/输出地址与 kernel 地址存储在 runtime Ring Buffer 中
6. **类型上无 dag 前缀**：`task_desc`、`task_type_t`、`org_mode_t` —— `dag_` 仅用于头文件保护宏

---

**状态**：计划完成。可进入 `/speckit-tasks`。
