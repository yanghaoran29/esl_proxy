# 功能规格说明：Dispatch

**功能分支**：`004-dispatch`

**创建时间**：2026-05-22

**状态**：草稿

**输入**：用户描述："dispatch负责任务下发通过共享内存获取orchestrator输出 + 多个dispatch分别管理多个不同的executor，通过work-stealing机制实现负载均衡 + dispatch通过共享内存从orchestrator和cutter获取可以下发的任务 + dispatch通过共享内存将下发的任务写给executor，包括taskID和index + executor仅返回1bit给dispatch告诉它在槽位A中的任务已执行完成 + Dispatch包含多个线程，每个线程管理60个CUBE Executor和60个Vector Executor"

## 用户场景与测试 *(必填)*

### 用户故事 1 - 通过共享内存进行任务分发 (优先级：P1)

系统操作员使用 Dispatch 组件将任务从 Orchestrator 分发到工作节点。Dispatch 通过共享内存读取 Orchestrator 的输出（任务图拓扑结构与就绪任务），实现进程或节点之间的零拷贝任务分发。

**优先级理由**：共享内存通信对于高性能、低延迟的任务分发至关重要，可避免数据拷贝开销。

**独立测试**：可通过让 Dispatch 从共享内存中读取任务信息，并验证数据与 Orchestrator 写入的内容一致来进行测试。

**验收场景**：

1. **假设** Orchestrator 已计算出任务拓扑结构与就绪任务，**当** Dispatch 访问共享内存时，**则** 它能够获取完整的任务图结构
2. **假设** Dispatch 从共享内存中获取了任务信息，**当** 它将任务分发给 worker 时，**则** 任务数据无需额外拷贝即可传输
3. **假设** 共享内存中包含有效的任务图，**当** Dispatch 读取时，**则** 数据应反映 Orchestrator 输出的当前状态

---

### 用户故事 1b - 通过共享内存向 Executor 下发任务 (优先级：P1)

系统操作员依赖 Dispatch 通过共享内存向 Executor 写入任务信息。Dispatch 将 taskID 和 index 写入共享内存，使 Executor 能够获取并执行任务，而无需额外的协调开销。

**优先级理由**：向共享内存写入任务元数据是 Dispatch 向 Executor 通知可执行任务的方式——这是核心分发协议。

**独立测试**：可通过让 Dispatch 向共享内存写入 taskID 和 index，并验证 Executor 读取到正确值来进行测试。

**验收场景**：

1. **假设** Dispatch 选择了一个待分发任务，**当** 它向共享内存写入时，**则** taskID 和 index 被存储在 Executor 可访问的共享内存区域中
2. **假设** Dispatch 向共享内存写入了任务信息，**当** Executor 读取时，**则** 它获取到与写入内容匹配的正确 taskID 和 index
3. **假设** Dispatch 为 Executor E1 向共享内存写入，**当** Executor E2 从同一区域读取时，**则** 它无法看到 E1 的任务数据（适当的隔离性）

---

### 用户故事 1c - 通过共享内存进行 Executor 完成通知 (优先级：P1)

系统操作员依赖 Executor 通过共享内存使用最小的 1-bit 信号向 Dispatch 通知任务完成。在执行完 Slot A 中的任务后，Executor 将共享内存中的一个原子位置位，表示任务已完成。Dispatch 轮询该位以判断 Slot A 中的任务是否已完成。

**优先级理由**：Executor 完成通知闭合了反馈回路——Dispatch 必须在任务完成时接收到信号，以便将其从队列中移除，并使 Cutter 能够解析新就绪任务的依赖关系。

**独立测试**：可通过让 Executor 在共享内存中设置完成位，并验证 Dispatch 读取到正确值来进行测试。

**验收场景**：

1. **假设** Executor 完成了 Slot A 中的任务，**当** 它设置完成位时，**则** 该位被存储在 Dispatch 可访问的共享内存区域中
2. **假设** Executor 将完成位设置为 1，**当** Dispatch 读取时，**则** 它获取到值 1，表示任务已完成
3. **假设** Dispatch 读取到完成位为 1，**当** 它处理该通知时，**则** 它得知 Slot A 中的任务已完成，可以提交下一个任务
4. **假设** Executor 尚未完成任务，**当** Dispatch 读取完成位时，**则** 它获取到值 0，表示任务正在执行中

---

### 用户故事 2 - 双源任务获取 (优先级：P1)

系统操作员依赖 Dispatch 从两个来源获取可执行任务：Orchestrator（用于初始就绪任务）和 Cutter（用于依赖解析后新就绪的任务）。Dispatch 从这两个共享内存区域读取数据，以构建其分发队列。

**优先级理由**：Dispatch 必须同时处理来自 Orchestrator 的初始任务图和来自 Cutter 的依赖已解析的就绪任务——这是完整的任务获取流水线。

**独立测试**：可通过让 Orchestrator 和 Cutter 都向共享内存写入就绪任务，并验证 Dispatch 同时从两者读取来进行测试。

**验收场景**：

1. **假设** Orchestrator 在共享内存中有就绪任务，**当** Dispatch 读取时，**则** 它从 Orchestrator 的区域获取任务
2. **假设** Cutter 已解析依赖并具有就绪任务，**当** Dispatch 读取时，**则** 它从 Cutter 的区域获取任务
3. **假设** Orchestrator 和 Cutter 都具有就绪任务，**当** Dispatch 获取任务时，**则** 它将来自两个来源的任务合并到分发队列中
4. **假设** Dispatch 从两个来源读取，**当** 它进行分发时，**则** 队列中不存在任务重复

---

### 用户故事 3 - Orchestrator 输出集成 (优先级：P1)

系统操作员依赖 Dispatch 从 Orchestrator 接收实时更新。Orchestrator 将任务执行结果和就绪任务信息写入共享内存，Dispatch 读取该输出以确定下一组要分发的任务。

**优先级理由**：通过共享内存实现 Orchestrator 与 Dispatch 之间的无缝集成，构建 DAG 引擎的核心流水线。

**独立测试**：可通过向共享内存写入数据并让 Dispatch 读取并验证来进行测试。

**验收场景**：

1. **假设** Orchestrator 产生输出并将其写入共享内存，**当** Dispatch 从同一共享内存区域读取时，**则** 它接收到正确的 Orchestrator 输出
2. **假设** Orchestrator 使用新就绪任务更新共享内存，**当** Dispatch 轮询或接收到通知时，**则** 它能立即开始分发新任务

---

### 用户故事 4 - 多 Dispatch 管理 (优先级：P1)

系统操作员配置多个 Dispatch 实例，每个 Dispatch 管理自己的一组 Executor。每个 Dispatch 独立运行，从共享内存中读取任务信息并将任务分发给其分配的 Executor。

**优先级理由**：多个 Dispatch 实例可实现分布式任务处理与横向扩展并行性。

**独立测试**：可通过创建多个 Dispatch 实例并验证每个实例管理自己的 Executor 集来进行测试。

**验收场景**：

1. **假设** 创建了多个 Dispatch 实例，**当** 每个 Dispatch 被分配一个独立的 Executor 集时，**则** 每个 Dispatch 仅管理其分配到的 Executor
2. **假设** Dispatch A 拥有 Executor {E1, E2}，Dispatch B 拥有 Executor {E3, E4}，**当** 提交任务时，**则** Dispatch A 仅向 E1/E2 分发，Dispatch B 仅向 E3/E4 分发
3. **假设** 一个 Dispatch 管理 N 个 Executor，**当** 它分发任务时，**则** 任务在其 N 个 Executor 之间均匀分布

---

### 用户故事 4b - Dispatch 工作线程的 Executor 分配 (优先级：P1)

系统操作员依赖 Dispatch 中的每个工作线程管理一组固定的 Executor。每个线程被分配 60 个具备 CUBE 能力的 Executor 和 60 个具备 VECTOR 能力的 Executor，使该线程能够将 CUBE、VECTOR 和 MIX 任务下发到相应的 Executor。

**优先级理由**：每线程固定的 Executor 分配可实现可预测的分发容量，并使系统能高效处理特定的工作负载组合（CUBE、VECTOR、MIX）。

**独立测试**：可通过验证每个 Dispatch 工作线程在其管理下恰好有 60 个 CUBE Executor 和 60 个 VECTOR Executor 来进行测试。

**验收场景**：

1. **假设** 一个 Dispatch 拥有多个工作线程，**当** 每个线程被初始化时，**则** 每个线程恰好管理 60 个 CUBE Executor 和 60 个 VECTOR Executor
2. **假设** 某工作线程拥有 60 个可用的 CUBE Executor 和 60 个可用的 VECTOR Executor，**当** 一个 CUBE 任务被下发时，**则** 该线程将任务路由到其 60 个 CUBE Executor 之一
3. **假设** 某工作线程拥有 60 个可用的 CUBE Executor 和 60 个可用的 VECTOR Executor，**当** 一个 VECTOR 任务被下发时，**则** 该线程将任务路由到其 60 个 VECTOR Executor 之一
4. **假设** 某工作线程拥有 60 个可用的 CUBE Executor 和 60 个可用的 VECTOR Executor，**当** 一个 MIX 任务被下发时，**则** 该线程寻找一个同时具备 CUBE 和 VECTOR 能力且空闲的 Executor 并将任务下发给它
5. **假设** 一个 MIX 任务被下发到双能力 Executor，**当** CUBE 与 VECTOR 子任务均完成时，**则** MIX 任务转换为 COMPLETED 状态
6. **假设** 一个 MIX 任务被下发到双能力 Executor，**当** 任一子任务报告失败时，**则** MIX 任务转换为 FAILED 状态

---

### 用户故事 6 - Work-Stealing 负载均衡 (优先级：P1)

系统操作员依赖 Work-Stealing 机制在不同 Dispatch 管理的 Executor 之间均衡负载。当某个 Dispatch 的 Executor 繁忙而另一个 Dispatch 的 Executor 空闲时，空闲的 Executor 从繁忙的 Executor 处窃取工作。

**优先级理由**：Work-Stealing 确保所有 Dispatch 下的 Executor 得到高效利用，避免空闲浪费。

**独立测试**：可通过构造一个场景，使一个 Dispatch 的所有 Executor 繁忙而另一个 Dispatch 有空闲 Executor，然后验证 Work-Stealing 是否发生来进行测试。

**验收场景**：

1. **假设** Dispatch A 的所有 Executor 繁忙，Dispatch B 有空闲 Executor，**当** Work-Stealing 被触发时，**则** Dispatch B 的 Executor 从 Dispatch A 的队列中窃取任务
2. **假设** Work-Stealing 已启用，**当** 空闲的 Executor 请求工作时，**则** 它从最繁忙的 Dispatch 队列中窃取
3. **假设** 多个 Dispatch 参与 Work-Stealing，**当** 任务被窃取时，**则** 窃取过程遵守任务依赖约束

---

### 用户故事 7 - 共享内存同步 (优先级：P2)

系统操作员需要 Dispatch 与 Orchestrator 安全地协调对共享内存的访问。两个组件必须同步，避免读取过期数据或同时写入同一区域。

**优先级理由**：适当的同步可确保数据一致性，避免损坏或丢失更新。

**独立测试**：可通过模拟并发访问并验证适当的同步行为来进行测试。

**验收场景**：

1. **假设** Orchestrator 正在向共享内存写入，**当** Dispatch 尝试读取时，**则** 适当的同步机制确保 Dispatch 等待或读取到一致的数据
2. **假设** 两个组件均需访问共享内存，**当** 它们通过同步原语进行协调时，**则** 不会发生数据损坏

---

### 用户故事 7 - 共享内存同步 (优先级：P2)

系统操作员需要 Dispatch 与 Orchestrator 安全地协调对共享内存的访问。两个组件必须同步，避免读取过期数据或同时写入同一区域。

**优先级理由**：适当的同步可确保数据一致性，避免损坏或丢失更新。

**独立测试**：可通过模拟并发访问并验证适当的同步行为来进行测试。

**验收场景**：

1. **假设** Orchestrator 正在向共享内存写入，**当** Dispatch 尝试读取时，**则** 适当的同步机制确保 Dispatch 等待或读取到一致的数据
2. **假设** 两个组件均需访问共享内存，**当** 它们通过同步原语进行协调时，**则** 不会发生数据损坏

---

### 用户故事 8 - 分布式任务分发 (优先级：P2)

系统操作员可以通过添加更多的 Dispatch-Executor 对来扩展系统。Work-Stealing 机制自动在所有可用的 Executor 之间均衡负载，无论它们由哪个 Dispatch 管理。

**优先级理由**：弹性可扩展性使系统能够高效处理变化的工作负载。

**独立测试**：可通过添加新的 Dispatch-Executor 对并验证 Work-Stealing 是否重新分配负载来进行测试。

**验收场景**：

1. **假设** 系统有 N 对 Dispatch-Executor，**当** 添加一个新的 Dispatch-Executor 对时，**则** Work-Stealing 将新的 Executor 纳入窃取池
2. **假设** 一个新的 Dispatch 加入 Work-Stealing 池，**当** 负载均衡发生时，**则** 任务在所有 N+1 个 Dispatch 之间重新分配
3. **假设** 一个 Dispatch 从系统中移除，**当** 其余 Dispatch 均衡负载时，**则** 任务被重新分配给剩余的 Executor

---

### 用户故事 8 - 分布式任务分发 (优先级：P2)

系统操作员可以通过添加更多的 Dispatch-Executor 对来扩展系统。Work-Stealing 机制自动在所有可用的 Executor 之间均衡负载，无论它们由哪个 Dispatch 管理。

**优先级理由**：弹性可扩展性使系统能够高效处理变化的工作负载。

**独立测试**：可通过添加新的 Dispatch-Executor 对并验证 Work-Stealing 是否重新分配负载来进行测试。

**验收场景**：

1. **假设** 系统有 N 对 Dispatch-Executor，**当** 添加一个新的 Dispatch-Executor 对时，**则** Work-Stealing 将新的 Executor 纳入窃取池
2. **假设** 一个新的 Dispatch 加入 Work-Stealing 池，**当** 负载均衡发生时，**则** 任务在所有 N+1 个 Dispatch 之间重新分配
3. **假设** 一个 Dispatch 从系统中移除，**当** 其余 Dispatch 均衡负载时，**则** 任务被重新分配给剩余的 Executor

---

### 用户故事 9 - Dispatch 生命周期 (优先级：P2)

系统操作员创建一个连接到共享内存的 Dispatch 实例，使用它进行任务分发，并干净地关闭它。Dispatch 在关闭时正确地从共享内存中分离。

**优先级理由**：适当的生命周期管理可确保无资源泄漏与干净的系统关闭。

**独立测试**：可通过创建并销毁 Dispatch 并验证资源是否被干净释放来进行测试。

**验收场景**：

1. **假设** 一个 Dispatch 被创建并连接到共享内存，**当** 它被销毁时，**则** 所有共享内存连接均被释放
2. **假设** 一个 Dispatch 在任务执行过程中正在关闭，**当** 关闭完成时，**则** 不会发生共享内存泄漏

---

### 用户故事 9 - Dispatch 生命周期 (优先级：P2)

系统操作员创建一个连接到共享内存的 Dispatch 实例，使用它进行任务分发，并干净地关闭它。Dispatch 在关闭时正确地从共享内存中分离。

**优先级理由**：适当的生命周期管理可确保无资源泄漏与干净的系统关闭。

**独立测试**：可通过创建并销毁 Dispatch 并验证资源是否被干净释放来进行测试。

**验收场景**：

1. **假设** 一个 Dispatch 被创建并连接到共享内存，**当** 它被销毁时，**则** 所有共享内存连接均被释放
2. **假设** 一个 Dispatch 在任务执行过程中正在关闭，**当** 关闭完成时，**则** 不会发生共享内存泄漏

---

### 用户故事 10 - 任务亲和性 (优先级：P3)

系统操作员可以选择性地配置任务亲和性规则。需要特定 Executor 的任务（由于内存局部性、设备亲和性或其他约束）被路由到合适的 Dispatch。

**优先级理由**：亲和性感知可以提升 NUMA 或基于加速器的工作负载的性能。

**独立测试**：可通过配置亲和性规则并验证任务是否被路由到正确的 Dispatch 来进行测试。

**验收场景**：

1. **假设** 一个任务具有亲和性约束（例如，必须在特定 Executor 上运行），**当** 该任务被提交时，**则** 它被路由到管理该 Executor 的 Dispatch
2. **假设** 一个任务没有亲和性约束，**当** Work-Stealing 处于活动状态时，**则** 该任务可被任何空闲的 Executor 窃取

---

## 需求 *(必填)*

### 功能性需求

- **FR-001**：Dispatch 必须从 Orchestrator 写入的共享内存中读取任务图拓扑结构
- **FR-002**：Dispatch 必须从共享内存中读取就绪任务信息
- **FR-003**：Dispatch 必须在无必要的额外数据拷贝的前提下将任务分发给 worker
- **FR-004**：必须创建多个 Dispatch 实例，每个实例管理自己的一组 Executor
- **FR-005**：在正常负载下，每个 Dispatch 必须仅向其分配的 Executor 分发任务
- **FR-006**：系统必须实现 Work-Stealing，使空闲的 Executor 能从其他 Dispatch 窃取任务
- **FR-007**：Work-Stealing 必须遵守任务依赖约束
- **FR-008**：负载均衡决策必须基于队列深度与 Executor 可用性
- **FR-009**：Dispatch 与 Orchestrator 必须同步对共享内存的访问
- **FR-010**：系统必须支持动态地添加和移除 Dispatch-Executor 对
- **FR-011**：可选的任务亲和性规则在配置后必须将任务路由到特定的 Dispatch
- **FR-012**：Dispatch 在关闭时必须正确地从共享内存中分离
- **FR-013**：Dispatch 必须从 Orchestrator 与 Cutter 两个共享内存区域中读取就绪任务，且不出现重复
- **FR-014**：Dispatch 必须向共享内存写入 taskID 和 index，以供 Executor 获取
- **FR-015**：Dispatch 必须从共享内存中读取 1-bit 完成信号，以判断 Executor 是否已完成 Slot A 中的任务
- **FR-016**：Dispatch 中的每个工作线程必须恰好管理 60 个具备 CUBE 能力的 Executor 和 60 个具备 VECTOR 能力的 Executor
- **FR-017**：Dispatch 在下发 MIX 任务之前必须寻找一个同时具备 CUBE 和 VECTOR 能力且空闲的 Executor
- **FR-018**：当一个 MIX 任务准备好下发时，若当前没有空闲的双能力 Executor，Dispatch 必须等待
- **FR-019**：在双能力 Executor 上执行的 MIX 任务必须独立跟踪 CUBE 和 VECTOR 子任务的完成情况
- **FR-020**：在 CUBE 和 VECTOR 子任务均成功完成之前，MIX 任务不得转换为 COMPLETED 状态
- **FR-021**：若 CUBE 或 VECTOR 子任务任意一个报告失败，MIX 任务必须转换为 FAILED 状态

---

## 成功标准 *(必填)*

### 可衡量结果

- **SC-001**：Dispatch 在 Orchestrator 输出可用后 1 微秒内从共享内存读取该输出
- **SC-002**：从共享内存读取到 worker 通知之间的任务分发延迟低于 10 微秒
- **SC-003**：共享内存访问已得到适当同步，在并发访问下不出现数据损坏
- **SC-004**：当 Dispatch A 拥有 10 个挂起任务而 Dispatch B 拥有 0 个时，Work-Stealing 重新分配任务，使 Dispatch B 上每个 Executor 的任务数 <= 5
- **SC-005**：通过 Work-Stealing 完成任务再分配的时间在 100 微秒内
- **SC-006**：新增的 Dispatch-Executor 对在添加后的 10 毫秒内被纳入 Work-Stealing
- **SC-007**：Dispatch 干净关闭的耗时在 1 毫秒内，且不出现共享内存泄漏
- **SC-008**：从共享内存读取的任务数据实现零拷贝分发

---

## 假设条件

- Orchestrator 与 Dispatch 之间的共享内存区域已预先建立（通过配置或启动握手）
- Orchestrator 以 Dispatch 能理解的格式写入数据（约定好的数据结构）
- 同步使用原子操作或类似的无锁原语
- 共享内存的持久性由操作系统处理（无显式的持久化要求）
- 每个 Dispatch 拥有 Work-Stealing 机制可见的唯一标识符
- 在 Work-Stealing 过程中保持任务依赖（仅当任务的前置任务已满足时该任务才可被窃取）
- 系统假设"信任调用方"——亲和性约束由调用方正确提供
- Work-Stealing 的窃取速率可针对每个 Dispatch 或全局进行配置
