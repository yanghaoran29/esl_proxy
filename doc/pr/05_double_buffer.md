# PR-E：double_buffer

相对基线：`pr/D-mix`。

---

## 宏观：本 PR 完成了什么

为 CUBE/VECTOR 打开 **per-core 双 payload 槽**（`task_id & 1`）：busy 槽已 ACK 后，可向 free 槽 **prefetch** 下一发，减小流水线气泡。  
通过 `DISPATCH=double_buffer` / onboard 开关打开；默认仍为 basic。相对 simpler：槽位索引与 `payload+(task_id&1)` 对齐，但 simpler 默认双槽，本仓用宏可选；MIX 第二槽 prefetch 与 DB 用宏互斥以降低交叉复杂度。

---

## 微观：各部分怎么做

### 1. `DISPATCH=double_buffer` 宏

- **为什么修改**：可选开双槽，默认 basic 对照。
- **怎么修改**：`-DESL_DISPATCH_DOUBLE_BUFFER=1`。
- **相关代码**：

```74:78:esl_proxy/Makefile
DISPATCH ?= basic
ifeq ($(DISPATCH),double_buffer)
CFLAGS += -DESL_DISPATCH_DOUBLE_BUFFER=1
CXXFLAGS += -DESL_DISPATCH_DOUBLE_BUFFER=1
endif
```

- **对应 simpler**：默认 `payload_per_core_[worker][2]`。
- **相比 simpler 的区别**：编译开关 vs 默认开启。对照：

esl_proxy：

```74:78:esl_proxy/Makefile
DISPATCH ?= basic
ifeq ($(DISPATCH),double_buffer)
CFLAGS += -DESL_DISPATCH_DOUBLE_BUFFER=1
```

simpler：

```136:138:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_context.h
    // Per-core dispatch payload storage: dual-buffer for pipelining.
    // buf_idx = reg_task_id & 1; adjacent dispatches alternate automatically.
    PTO2DispatchPayload payload_per_core_[RUNTIME_MAX_WORKER][2];
```

---

### 2. `dispatch_prefetch`

- **为什么修改**：busy ACK 后可向 free 槽提前下发。
- **怎么修改**：busy+free；acked 后 claim 并 publish free_slot。
- **相关代码**：

```536:602:esl_proxy/src/algorithm/dispatch.c
static int dispatch_prefetch(ctrl_t *ctrl, int type)
{
    /* busy+free; platform_reg_task_acked; claim_block; publish free_slot */
}
```

- **对应 simpler**：`pending_occupied_` / early-dispatch 第二槽。
- **相比 simpler 的区别**：挂在 `g_executors` 双槽 + 宏。对照：

esl_proxy：

```563:591:esl_proxy/src/algorithm/dispatch.c
        const uint32_t reg_busy = (uint32_t)g_executors[exe_type][core].base[busy_slot];
        if (reg_addr == 0 || !platform_reg_task_acked(reg_addr, reg_busy)) {
            continue;
        }
        /* ... */
        if (dispatch_publish_block(ctrl, exe_type, type, one, block_idx, core, free_slot) != 0) {
```

simpler：

```136:138:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_context.h
    PTO2DispatchPayload payload_per_core_[RUNTIME_MAX_WORKER][2];
```

```461:466:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
            tracker.clear_pending_occupied(bit_pos);
            // Case 4 (running ACK) or Case 2 (pending ACK): clear pending_occupied only
            tracker.clear_pending_occupied(bit_pos);
```

---

### 3. 槽位 `task_id & 1`（与 AICore 对齐）

- **为什么修改**：调度写槽与 AICore 读槽必须一致。
- **怎么修改**：prepare / AICore 均用 `& 1`。
- **相关代码**：

```69:69:esl_proxy/src/algorithm/aicore_executor.c
            __gm__ EslDispatchPayload *exec_payload = payload_base + (task_id & 1u);
```

- **对应 simpler**：同样 `payload + (task_id & 1u)`。
- **相比 simpler 的区别**：槽位语义一致；差异在默认是否开启 DB。对照：

esl_proxy：

```69:69:esl_proxy/src/algorithm/aicore_executor.c
            __gm__ EslDispatchPayload *exec_payload = payload_base + (task_id & 1u);
```

```118:120:esl_proxy/src/algorithm/dispatch_payload.c
    reg_task_id = dispatch_next_reg_task_id(core);
    slot = (int)(reg_task_id & 1u);
    p = (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
```

simpler：

```157:158:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/aicore/aicore_executor.cpp
            // Select dual-buffer slot: same bit as AICPU used when writing payload
            __gm__ PTO2DispatchPayload *exec_payload = payload + (task_id & 1u);
```

---

### 4. MIX prefetch 与 DB 宏互斥

- **为什么修改**：MIX 第二槽与 CUBE/VECTOR DB 同开状态机过复杂。
- **怎么修改**：`#if !ESL_DISPATCH_DOUBLE_BUFFER` 才 `dispatch_mix_prefetch`。
- **相关代码**：`dispatch()` 宏分支。
- **对应 simpler**：更统一的 pending/early-dispatch。
- **相比 simpler 的区别**：用宏拆两套第二槽策略。对照：

esl_proxy：

```text
#if !ESL_DISPATCH_DOUBLE_BUFFER
    total_sent += dispatch_mix_prefetch(...);
#endif
total_sent += dispatch_prefetch(...);  /* CUBE/VECTOR，DB 打开时启用 */
```

simpler：

```515:516:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_completion.cpp
        if (shape == PTO2ResourceShape::MIX) {
            // Gated MIX uses split placement ...
```
