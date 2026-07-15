# PR-A0：目录分层（无代码语义变更）

相对基线：`origin/main`。

---

## 宏观：本 PR 完成了什么

把 `main` 里平铺的算法头/源整体迁到 `include/algorithm/`、`src/algorithm/`，并改 Makefile 的 `-I`/`SRCS`，**不改调度语义**。目的是腾出与 `platform/` 并列的算法边界，让后续上板 / sim 可以按目录分流，而不与算法文件搅在一起。

相对 simpler：对齐「算法 vs 平台」分层意图（simpler 为 `runtime/` vs `platform/`），本 PR 只做搬家，不对齐功能。

---

## 微观：各部分怎么做

### 1. 头文件迁入 `include/algorithm/`

- **为什么修改**：`main` 把头文件平铺在 `include/`，后续无法与 `platform/` 分流审查。
- **怎么修改**：`include/*.h` → `include/algorithm/*.h`（纯路径移动，内容不变）。
- **相关代码**：

```text
include/conf.h, dispatch.h, cutter.h, …  →  include/algorithm/
```

- **对应 simpler**：算法侧落在 `simpler/src/a2a3/runtime/`，与 `platform/` 分开。
- **相比 simpler 的区别**：目录命名不同（`algorithm/` vs `runtime/`）；本 PR 只搬家。对照：

esl_proxy：

```text
esl_proxy/include/algorithm/*.h
esl_proxy/src/algorithm/*.c
```

simpler：

```text
simpler/src/a2a3/runtime/          # 编排 / 调度 / tensormap
simpler/src/a2a3/platform/         # onboard / sim
```

---

### 2. 源文件迁入 `src/algorithm/`

- **为什么修改**：与头文件同步，便于后续加 `src/platform/`。
- **怎么修改**：`src/{cutter,dispatch,executor,log,manager,shm}.c` → `src/algorithm/`。
- **相关代码**：

```text
src/cutter.c, dispatch.c, …  →  src/algorithm/
```

- **对应 simpler**：调度实现在 `runtime/.../scheduler/` 等。
- **相比 simpler 的区别**：A0 零语义变更（仍 Fake Return）；simpler 同树已是完整调度。对照：

esl_proxy（搬迁后路径，逻辑仍 Fake Return）：

```text
esl_proxy/src/algorithm/dispatch.c   # 本 PR 不改 Fake Return 语义
```

simpler（已是真调度）：

```180:190:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/shared/pto_runtime2_init.cpp
                &sched->ready_queues[i], arena, layout.off_ready_queue_slots[i], layout.ready_queue_capacity
```

---

### 3. Makefile：`-I` 与 `SRCS` 路径

- **为什么修改**：编译必须能找到搬迁后的头与源。
- **怎么修改**：`-I include/algorithm`；`SRCS` 改为 `src/algorithm/*.c`。
- **相关代码**：

```43:46:esl_proxy/Makefile
CFLAGS   := -g -std=c11 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=199309L \
	-I include/algorithm -I include/platform -I include/platform/sim \
	-I include/platform/onboard \
	-I cases -DORCH_CASE=$(CASE) -MMD -MP -Wno-error=implicit-function-declaration
```

- **对应 simpler**：CMake 按 `platform` / `runtime` 分 target。
- **相比 simpler 的区别**：esl 主机入口是 Makefile；simpler 用 CMake。对照：

esl_proxy：

```69:76:esl_proxy/Makefile
SRCS := \
	src/main.c \
	src/algorithm/executor.c \
	src/algorithm/dispatch.c \
	src/algorithm/cutter.c \
	src/algorithm/manager.c \
	src/algorithm/shm.c \
```

simpler（目录边界，见 platform 文档树）：

```text
simpler/src/a2a3/platform/{onboard,sim}/
simpler/src/a2a3/runtime/
```
