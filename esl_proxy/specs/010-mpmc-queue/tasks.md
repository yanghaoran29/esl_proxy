# 任务列表：MPMC 队列（BlkRing 非阻塞）

**输入**：来自 `/specs/010-mpmc-queue/` 的设计文档

**前置条件**：plan.md（必需）、spec.md（用户故事所需）

**组织方式**：任务按用户故事分组，以便每个故事可独立实现与测试。

## 格式：`[ID] [P?] [Story] 描述`

- **[P]**：可并行执行（不同文件，无依赖）
- **[Story]**：该任务所属的用户故事（例如 US1、US2、US3）
- 在描述中包含确切的文件路径

## 阶段 1：环境搭建（合并的 1 头文件 + 1 C 文件设计）

**目的**：创建包含 BlkRing 非阻塞队列实现的项目结构

- [X] T001 [P] 创建 include/dag/mpmc_queue.h，含 BlkRing 槽位状态枚举与队列结构体
- [X] T002 [P] 创建 include/dag/mpmc_queue.c，用于存放带默认容量的全局队列定义

---

## 阶段 2：基础（BlkRing 核心基础设施）

**目的**：在任何用户故事可被实现之前必须完成的核心基础设施

**BlkRing 设计**：原子槽位状态（EMPTY/FILL/COMPLETE），无 CAS 重试循环

- [X] T003 [P] 在 mpmc_queue.h 中定义 slot_state_t 枚举（EMPTY=0, FILL=1, COMPLETE=2）
- [X] T004 [P] 在 mpmc_queue.h 中定义带数据缓冲区与原子状态的 blkring_slot_t 结构体
- [X] T005 [P] 在 mpmc_queue.h 中定义 mpmc_queue_t 结构体，含 slots 数组、capacity、producer_idx、consumer_idx
- [X] T006 实现 mpmc_init()——分配 slots 数组，初始化所有状态为 EMPTY
- [X] T007 实现 mpmc_idx()——pos % capacity 用于循环访问
- [X] T008 实现 slot_state_load()——原子加载槽位状态
- [X] T009 实现 slot_state_store()——原子存储槽位状态
- [X] T010 实现 blkring_produce()——以单次原子状态转换入队（无 CAS 重试）
- [X] T011 实现 blkring_consume()——以单次原子状态转换出队（无 CAS 重试）
- [X] T012 [P] 在 mpmc_queue.c 中定义全局 mpmc_queue_t g_ready_queues[3][4]
- [X] T013 [P] 在 mpmc_queue.c 中定义全局 mpmc_queue_t g_complete_queue
- [X] T014 实现 ready_queue_get(type, mode) 内联访问器
- [X] T015 实现 complete_enqueue() 与 complete_dequeue() 内联访问器

**检查点**：BlkRing 基础设施就绪——无 CAS 重试的真正非阻塞

---

## 阶段 3：用户故事 1 - 通过 MPMC 队列进行任务下发（优先级：P1）

**目标**：使用 BlkRing 的基本入队/出队操作

**独立测试**：验证多生产者/多消费者可并发入队/出队且无丢失

- [X] T016 [US1] blkring_produce() 将项目写入槽位并将状态从 EMPTY 转换为 FILL
- [X] T017 [US1] blkring_consume() 从槽位读取项目并将状态从 FILL→COMPLETE→EMPTY 转换

---

## 阶段 4：用户故事 2 - 带反压的有界队列（优先级：P1）

**目标**：队列容量限制与反压行为

**独立测试**：验证队列达到容量时入队返回 MPMC_FULL

- [X] T018 [US2] 当无可用 EMPTY 槽位时 blkring_produce() 返回 MPMC_FULL
- [X] T019 [US2] blkring_consume() 在 COMPLETE→EMPTY 转换后创建 EMPTY 槽位

---

## 阶段 5：用户故事 3 - FIFO 顺序（优先级：P2）

**目标**：顺序入队/出队的 FIFO 顺序

**独立测试**：验证项目按入队顺序出队

- [X] T020 [US3] 通过 producer/consumer 索引保持顺序入队/出队的 FIFO 顺序

---

## 阶段 6：用户故事 4 - 非阻塞出队选项（优先级：P2）

**目标**：非阻塞出队操作

**独立测试**：验证队列为空时出队立即以空状态返回

- [X] T021 [US4] 当无可用 FILL 槽位时 blkring_consume() 立即返回 MPMC_EMPTY

---

## 阶段 7：用户故事 5 - 内存高效的循环实现（优先级：P3）

**目标**：带环绕行为的循环缓冲区

**独立测试**：验证缓冲区环绕与内存复用

- [X] T022 [US5] mpmc_idx() 使用 pos % capacity 正确环绕索引

---

## 阶段 8：用户故事 6 - 批量入队（优先级：P2）

**目标**：批量入队多个项目

**独立测试**：验证可在单次调用中入队 10 个项目的批次

- [X] T023 [US6] blkring_produce_batch() 遍历项目，每槽位单次原子操作
- [X] T024 [US6] blkring_produce_batch() 返回实际入队数
- [X] T025 [US6] 当数量超过可用 EMPTY 槽位时返回部分批次

---

## 阶段 9：用户故事 7 - 批量出队（优先级：P2）

**目标**：批量出队多个项目

**独立测试**：验证可在单次调用中出队 10 个项目的批次

- [X] T026 [US7] blkring_consume_batch() 遍历项目，每槽位单次原子操作
- [X] T027 [US7] blkring_consume_batch() 返回实际出队数
- [X] T028 [US7] 当可用项少于请求时返回部分批次

---

## 阶段 10：用户故事 8 - 批量大小限制与部分结果（优先级：P2）

**目标**：精确的部分批次处理

**独立测试**：验证当可用项少于请求时批量 API 返回精确数量

- [X] T029 [US8] blkring_produce_batch() 正确处理部分批次
- [X] T030 [US8] blkring_consume_batch() 正确处理部分批次

---

## 阶段 11：用户故事 9 - 按 TaskType+OrgMode 划分的 ReadyQueue（优先级：P1）

**目标**：二维 ReadyQueue 矩阵（task_type × org_mode）

**独立测试**：验证任务根据 type 和 org_mode 路由到正确队列

- [X] T031 [US9] 实现 ready_enqueue(type, mode, item) 内联函数
- [X] T032 [US9] 实现 ready_dequeue(type, mode, item) 内联函数
- [X] T033 [US9] 12 种队列组合（3 类型 × 4 mode）可访问

---

## 阶段 12：用户故事 10 - 全局 ReadyQueue 矩阵访问（优先级：P1）

**目标**：通过二维索引的 O(1) 查找

**独立测试**：验证 ready_queue_get() 在 O(1) 内返回正确队列

- [X] T034 [US10] ready_queue_get(type, mode) 返回队列指针
- [X] T035 [US10] 通过直接数组索引实现 O(1) 访问

---

## 阶段 13：用户故事 11 - 用于任务完成追踪的 CompleteQueue（优先级：P1）

**目标**：记录已完成任务通知

**独立测试**：验证完成通知可被入队与出队

- [X] T036 [US11] 实现 complete_enqueue() 内联函数
- [X] T037 [US11] 实现 complete_dequeue() 内联函数
- [X] T038 [US11] 在 CompleteQueue 中记录完成通知

---

## 阶段 14：用户故事 12 - 全局 CompleteQueue 访问（优先级：P1）

**目标**：CompleteQueue 全局可见

**独立测试**：验证 g_complete_queue 全局可访问

- [X] T039 [US12] 全局变量 g_complete_queue 存在
- [X] T040 [US12] 工作线程可在无队列引用下入队

---

## 阶段 15：完善与横切关注点

**目的**：验证与清理

- [X] T041 [P] 所有队列实现可使用 clang -std=c11 编译
- [X] T042 [P] 使用 C11 atomics（_Atomic、atomic_load/store，无 CAS）
- [X] T043 验证 BlkRing 真正非阻塞——无 compare-and-swap 重试循环

---

## 依赖与执行顺序

### 阶段依赖

- **环境搭建（阶段 1）**：已完成
- **基础（阶段 2）**：已完成——BlkRing 核心阻塞所有用户故事
- **用户故事（阶段 3-14）**：全部已完成
- **完善（阶段 15）**：已完成

### 摘要

所有 43 项任务已完成，采用 BlkRing 非阻塞设计，使用原子槽位状态（EMPTY/FILL/COMPLETE）。

---

## 实现策略

### BlkRing 非阻塞设计

1. **槽位状态**：每个槽位拥有原子状态（EMPTY/FILL/COMPLETE）
2. **入队（blkring_produce）**：
   - 使用 producer_idx 找到下一个 EMPTY 槽位
   - 通过 atomic_compare_exchange_strong（CAS）认领
   - 向槽位写入数据
   - CAS 成功后状态即为 FILL
3. **出队（blkring_consume）**：
   - 使用 consumer_idx 找到下一个 FILL 槽位
   - 通过 atomic_compare_exchange_strong（CAS）认领
   - 从槽位读取数据
   - 标记为 COMPLETE 后置为 EMPTY 以便槽位复用
4. **无重试循环**：每次槽位操作为单次 CAS 尝试

### 最终结构

```text
include/dag/
├── mpmc_queue.h     # BlkRing APIs (slot state, produce, consume, batch)
├── mpmc_queue.c     # Global defs (READY_QUEUE_CAPACITY=1024, COMPLETE_QUEUE_CAPACITY=1024)
├── task.h           # Task types (task_type_t, org_mode_t)
├── ring_buf.h/c     # Ring buffer (separate feature)
```

---

## 备注

- BlkRing 通过每槽位单次 CAS 设计提供非阻塞
- 所有队列操作均为 O(1)，CAS 尝试次数有界（每次检查的槽位 1 次）
- 使用 atomic_compare_exchange_strong 进行槽位认领（单次尝试，非重试循环）
- 无锁操作仅使用 C11 atomics
- 热路径中无 mutex
- 纯头文件库设计，单个 .c 文件用于全局定义
- 默认容量：所有队列均为 1024（ReadyQueue 每个、CompleteQueue）
