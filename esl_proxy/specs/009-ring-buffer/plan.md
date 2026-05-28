# 实现计划：任务环形缓冲区

**分支**：`009-ring-buffer` | **日期**：2026-05-26 | **规格说明**：[链接](spec.md)

**输入**：4 个用于任务数据存储的全局可见 Ring Buffer。包含 `dag/task.h`。Ring Buffer 大小为 4096，通过 TaskID & (RING_SIZE - 1) 实现 O(1) 索引。带非空检查的状态缓冲区插入：若非空则失败，若为空则插入。

## 概述

四个全局可见的 Ring Buffer 提供以 TaskID 索引的任务数据 O(1) 存储。包含 `dag/task.h` 以提供任务描述符类型。所有操作均使用 C11 原子操作实现无锁。状态缓冲区插入在写入前使用原子 CAS 检查是否为空。

## 技术上下文

**语言/版本**：C11（`-std=c11`）

**主要依赖**：仅标准 C 库（`<stdint.h>`、`<stdatomic.h>`）

**存储**：内存中 4 个 Ring Buffer（状态、基本信息、依赖、运行时）

**测试**：通过依赖注入进行单元测试

**目标平台**：跨平台（Linux/macOS）

**项目类型**：用于 DAG 调度的仅头文件 C 库

**性能目标**：
- Ring Buffer 访问：通过 TASKID & (RING_SIZE - 1) 实现 O(1)
- 紧凑存储：16 位 TaskID
- 无锁操作：仅使用 C11 原子操作
- 原子条件插入：C11 比较并交换

**约束条件**：
- 热路径中无互斥锁/自旋锁
- 所有输入均假定为有效（信任调用者）
- Ring Buffer 大小固定为 4096（2 的幂）
- 仅头文件库设计，.c 文件仅用于全局定义
- 命名不使用 `dag` 前缀（依据宪法 XI）
- 4 个全局可见的环形缓冲区
- 状态插入：非空返回错误，空则插入成功

**规模/范围**：
- DAG 中最多 10,000 个任务
- TASKID：16 位（2 字节）

## 宪法检查

*门控：必须在阶段 0 调研之前通过。在阶段 1 设计之后重新检查。*

| 原则 | 合规要求 |
|-----------|----------------------|
| 现代 C11 | 仅 C11 标准；`_Generic`、原子操作、`restrict` |
| 基于回调的异步 | 通过原子位完成；函数指针 |
| 基于 DAG 的任务调度 | DAG 结构；Work-Stealing 调度器 |
| 零拷贝任务数据流 | Ring Buffer 中的缓冲区描述符 |
| 无锁并发 | 仅 C11 原子操作；热路径中无互斥锁 |
| 热路径中不阻塞 | 无同步 I/O；带续延的异步等待 |
| 确定性调度 | 相同 DAG + 输入 → 相同结果 |
| 可测试性 | 通过函数指针依赖注入 |
| 仅头文件库 | 所有实现位于头文件中；API 使用 `static inline` 函数；全局状态位于单个 .c 文件 |
| 信任调用者 | 不进行验证；对无效输入的行为未定义 |
| 简洁命名 | 无冗余前缀；无 `dag` 前缀；在上下文内命名 |

## 项目结构

### 源代码（include/dag/）

```text
include/dag/
├── ring_buf.h     # Ring Buffer API (static inline functions)
└── ring_buf.c     # Global ring buffer definitions
```

**仅头文件强制要求**：所有 API 位于头文件中并使用 `static inline`。唯一的 .c 文件用于全局变量定义。

**命名约定**：文件名仅在头文件保护宏中使用 `dag_` 前缀。类型名和函数名不使用 `dag_` 前缀。

## 阶段 1：设计

### ring_buf.h - Ring Buffer API

```c
#ifndef DAG_RING_BUF_H
#define DAG_RING_BUF_H

#include <stdint.h>
#include <stdatomic.h>
#include "task.h"

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)

typedef enum {
    RING_CAT_STATE  = 0,
    RING_CAT_BASIC  = 1,
    RING_CAT_DEP    = 2,
    RING_CAT_RUNTIME = 3,
} ring_cat_t;

/*
 * O(1) index computation via bitwise AND
 * Requires RING_SIZE to be power of 2
 */
static inline uint32_t ring_idx(uint16_t id) {
    return id & RING_MASK;
}

/*
 * Conditional state insert - atomic empty check + insert
 * Returns: 0 on success, negative on error (non-empty or race)
 */
static inline int state_put_if_empty(uint32_t idx, uint32_t val) {
    _Atomic uint32_t *entry = &g_state_buf[idx];
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        entry, &expected, val,
        memory_order_acquire, memory_order_acquire
    ) ? 0 : -1;
}

/*
 * 4 globally visible ring buffers - direct access via variable name
 * Usage: g_state_buf[ring_idx(task_id)]
 */
extern _Atomic uint32_t g_state_buf[RING_SIZE];
extern _Atomic task_desc_t g_basic_buf[RING_SIZE];
extern _Atomic dep_base_t g_dep_buf[RING_SIZE];
extern _Atomic void *g_runtime_buf[RING_SIZE];

#endif
```

### ring_buf.c - 全局定义

```c
#include "ring_buf.h"

_Atomic uint32_t g_state_buf[RING_SIZE];
_Atomic task_desc_t g_basic_buf[RING_SIZE];
_Atomic dep_base_t g_dep_buf[RING_SIZE];
_Atomic void *g_runtime_buf[RING_SIZE];
```

### 关键设计决策

1. **单一头文件 API**：所有 Ring Buffer 访问器均位于 `ring_buf.h` 中，作为 static inline 函数
2. **4 个全局缓冲区**：在 `ring_buf.c` 中定义为 `g_<name>` 模式
3. **条件插入**：使用 `atomic_compare_exchange_strong` 原子地检查是否为空（expected=0）并插入
4. **Ring Buffer 大小 4096**：2 的幂以便高效位掩码索引
5. **类型上无 dag 前缀**：`ring_idx`、`ring_cat_t`、`state_put_if_empty` —— dag_ 仅用于头文件保护宏
6. **包含 task.h**：使用来自 `dag/task.h` 的 `task_desc_t`、`dep_base_t`

---

**状态**：计划完成。已准备好执行 `/speckit-tasks`。
