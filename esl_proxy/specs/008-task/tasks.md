# 任务：Task

**输入**：来自 `/specs/008-task/` 的设计文档

**前置条件**：plan.md（必需）、spec.md（用户故事必需）

**测试**：功能规格说明中未要求测试

**组织方式**：任务按用户故事分组，以便每个故事可独立实现与测试。

## 格式：`[ID] [P?] [Story] Description`

- **[P]**：可并行运行（不同文件，无依赖）
- **[Story]**：该任务所属的用户故事（例如 US1、US2、US3）
- 在描述中包含精确的文件路径

## 路径约定

- 源代码：仓库根目录下的 `include/dag/`
- 仅头文件 C 库：所有实现位于 `.h` 文件中
- 每个功能单一头文件：`task.h`

---

## Phase 1：Setup（项目初始化）

**目的**：验证仅头文件库的目录结构

- [X] T001 按 plan.md 验证 include/dag/ 目录存在

---

## Phase 2：Foundational（核心类型与常量）

**目的**：所有任务操作所需的核心类型与常量

**关键**：必须在任何用户故事可被测试前完成

- [X] T002 [P] 在 include/dag/task.h 中定义 RING_SIZE (4096) 与 RING_MASK 常量
- [X] T003 [P] 将 task_id_t 定义为 uint16_t，作为 16 位 TaskID
- [X] T004 [P] 实现 ring_idx() 辅助函数，通过 TaskID & (RING_SIZE - 1) 进行 O(1) 索引

---

## Phase 3：用户故事 1 - 仅包含任务描述信息（优先级：P1） 🎯 MVP

**目标**：任务描述符仅包含描述字段——无执行状态

**独立测试**：验证任务描述符仅包含描述字段（id、type、mode、kernel、index、count、prio、data），不含执行状态字段

### 用户故事 1 的实现

- [X] T005 [P] [US1] 在 include/dag/task.h 中定义仅含描述字段（id、type、mode、kernel、index、count、prio、data）的 struct task_desc
- [X] T006 [US1] 添加注释说明 task_desc 中没有执行状态字段

---

## Phase 4：用户故事 2 - 任务类型定义（优先级：P1）

**目标**：含 CUBE、VECTOR、MIX 取值的任务类型枚举

**独立测试**：验证 task_type_t 枚举具有 CUBE=0、VECTOR=1、MIX=2 取值

### 用户故事 2 的实现

- [X] T007 [P] [US2] 在 include/dag/task.h 中定义 task_type_t 枚举（TASK_TYPE_CUBE=0、TASK_TYPE_VECTOR=1、TASK_TYPE_MIX=2）

---

## Phase 5：用户故事 3 - 任务组织模式（优先级：P1）

**目标**：含 Single、Group、SPMD_SYNC、SPMD_ASYNC 取值的组织模式枚举

**独立测试**：验证 org_mode_t 枚举的取值正确

### 用户故事 3 的实现

- [X] T008 [P] [US3] 在 include/dag/task.h 中定义 org_mode_t 枚举（ORG_MODE_SINGLE=0、ORG_MODE_GROUP=1、ORG_MODE_SPMD_SYNC=2、ORG_MODE_SPMD_ASYNC=3）

---

## Phase 6：用户故事 4 - 任务描述符中的 SPMD INDEX（优先级：P1）

**目标**：基准 INDEX 存储在任务描述符中；每个实例的 INDEX 在调度过程中导出

**独立测试**：验证任务描述符具有用于基准 INDEX 的 index 字段

### 用户故事 4 的实现

- [X] T009 [P] [US4] 添加内联辅助函数，用于导出每个实例的 INDEX：base_index + instance_number（在 include/dag/task.h 中）

---

## Phase 7：用户故事 5 - 任务描述符复用（优先级：P1）

**目标**：同一任务描述符实例可被复用于多次提交

**独立测试**：验证任务描述符结构是自包含且无状态的

### 用户故事 5 的实现

- [X] T010 [P] [US5] 在 task.h 中添加注释，记录任务描述符复用语义（描述符在创建后为 const，执行状态分开管理）

---

## Phase 8：用户故事 6 - 任务描述与执行状态的清晰分离（优先级：P1）

**目标**：任务描述符由 Orchestrator 拥有；执行状态由 Dispatcher/Executor 拥有

**独立测试**：验证任务描述符不引用执行状态

### 用户故事 6 的实现

- [X] T011 [P] [US6] 添加头文件注释，记录所有权分离：任务描述符（Orchestrator）vs 执行状态（Dispatcher/Executor）

---

## Phase 9：打磨与横切关注点

**目的**：完成头文件以供分发

- [X] T012 [P] 添加头文件保护宏（dag/task.h）
- [X] T013 [P] 验证所有类型使用简洁命名（类型/函数上无 dag_ 前缀）

---

## 依赖与执行顺序

### 阶段依赖

- **Setup (Phase 1)**：无依赖——可立即开始
- **Foundational (Phase 2)**：依赖 Setup 完成——阻塞所有用户故事
- **用户故事 (Phase 3-8)**：均依赖 Foundational 阶段完成，可并行运行

### 用户故事依赖

- **用户故事 1-6**：在 Foundational 阶段完成后均可并行运行（故事相互独立）

### 每个用户故事内部

- 核心类型定义先于辅助函数
- 头文件完成后再进入打磨

---

## 并行机会

- T002、T003、T004 可并行运行（不同定义）
- T005、T007、T008 可并行运行（不同类型定义）
- T009、T010、T011 可并行运行（不同辅助函数/注释）
- T012、T013 可并行运行（打磨任务）

---

## 实施策略

### MVP 优先（用户故事 1）

1. 完成 Phase 1：Setup
2. 完成 Phase 2：Foundational
3. 完成 Phase 3：用户故事 1
4. **停止并验证**：验证任务描述符结构

### 增量交付

1. 完成 Setup + Foundational → 基础就绪
2. 加入用户故事 1 → 独立测试
3. 加入用户故事 2-6 → 每个增加一个独立的类型/辅助
4. 打磨 → 完成头文件以供分发

---

## 备注

- [P] 任务 = 不同文件或同一文件内的不同定义
- [Story] 标签将任务映射到具体用户故事，便于可追溯性
- 仅头文件库：无 .c 实现文件
- 简洁命名：类型/函数不使用 dag_ 前缀
- Trust the Caller：不做校验，非法输入时未定义行为
