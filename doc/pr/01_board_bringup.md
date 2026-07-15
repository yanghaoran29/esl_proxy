# PR-A：上板 bring-up（basic + fake kernel）

相对基线：`pr/A0-layout`。

---

## 宏观：本 PR 完成了什么

在 NPU 上用 **fake kernel** 打通 **paged attention** 调度闭环（验收：`task/subtask/completed = 1920`）。相对 `main` 的 Fake Return，本 PR 补齐：

1. **platform/onboard**：Host/AICPU/AICore 三端构建与拉起（先 AICore 后 AICPU）  
2. **握手 + 512B payload + 门铃/COND**：AICPU prepare/`wmb`/publish，AICore ACK→执行→FIN  
3. **algorithm 手术补丁**：dispatch 真完成回收；BSS 替代 AICPU 堆；PA/qwen3 case 接 fake 时长参数  

能力收窄为 **basic**（无 SPMD / MIX / DB / sim）。相对 simpler：协议形状对齐（Handshake / PTO2 payload / ACK-FIN），执行体用忙等代替真实算子。

---

## 微观：各部分怎么做

### 1. `EslHandshake` / 512B payload

- **为什么修改**：Host/AICPU/AICore 需要统一运行时与 per-core payload 布局。
- **怎么修改**：新增 `EslHandshake`（含 regs_ready）、`EslRuntime`、512B `EslDispatchPayload`。
- **相关代码**：

```24:39:esl_proxy/include/algorithm/runtime.h
typedef struct EslHandshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile int32_t core_type;
    volatile uint32_t physical_core_id;
    volatile uint32_t aicpu_regs_ready;
    volatile uint32_t aicore_regs_ready;
} __attribute__((aligned(64))) EslHandshake;
```

- **对应 simpler**：`Handshake`、`PTO2DispatchPayload`。
- **相比 simpler 的区别**：esl 多 `*_regs_ready`；payload `args` 服务 fake。对照：

esl_proxy：

```24:39:esl_proxy/include/algorithm/runtime.h
typedef struct EslHandshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile int32_t core_type;
    volatile uint32_t physical_core_id;
    volatile uint32_t aicpu_regs_ready;
    volatile uint32_t aicore_regs_ready;
} __attribute__((aligned(64))) EslHandshake;
```

simpler（无 regs_ready）：

```98:104:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/runtime.h
struct Handshake {
    volatile uint32_t aicpu_ready;  // AICPU ready signal: 0=not ready, 1=ready
    volatile uint32_t aicore_done;  // AICore ready signal: 0=not ready, core_id+1=ready
    volatile uint64_t task;         // Init: PTO2DispatchPayload* (set before aicpu_ready); runtime: unused
    volatile CoreType core_type;    // Core type: CoreType::AIC or CoreType::AIV (reported by AICore with aicore_done)
    volatile uint32_t physical_core_id;  // Physical core ID (reported by AICore with aicore_done)
} __attribute__((aligned(64)));
```

esl_proxy payload（简化布局 + fake 用 args）：

```31:39:esl_proxy/include/algorithm/runtime.h
typedef struct EslDispatchPayload {
    uint64_t function_bin_addr;
    uint64_t args[50];
    int32_t local_block_idx;
    int32_t local_block_num;
    /* AsyncCtx / GlobalContext / not_ready ... */
    volatile uint32_t not_ready;
    uint8_t reserved_payload_abi_pad[4];
} __attribute__((aligned(64))) EslDispatchPayload; /* 512B */
```

simpler payload（真实算子 ABI）：

```83:111:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto2_dispatch_payload.h
struct alignas(64) PTO2DispatchPayload {
    uint64_t function_bin_addr; /**< Kernel entry address in GM (set by Scheduler). */
    LocalContext local_context;
    volatile uint64_t src_payload;
    uint64_t args[PTO2_DISPATCH_MAX_ARGS];
    GlobalContext global_context;
```

---

### 2. 握手 `esl_handshake_*`

- **为什么修改**：调度前必须确认物理核与 COND。
- **怎么修改**：AICPU `aicpu_ready` → 等 `aicore_regs_ready` → 缓存 reg/cond → `aicpu_regs_ready` → 等 `aicore_done`。
- **相关代码**：`src/algorithm/handshake.c`。
- **对应 simpler**：AICPU cold-path 握手 + AICore 对端。
- **相比 simpler 的区别**：esl 用 regs_ready 双阶段缓存物理核；simpler a2a3 Handshake 无这两字段。对照：

esl_proxy：

```24:32:esl_proxy/include/algorithm/runtime.h
typedef struct EslHandshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile int32_t core_type;
    volatile uint32_t physical_core_id;
    volatile uint32_t aicpu_regs_ready;
    volatile uint32_t aicore_regs_ready;
} __attribute__((aligned(64))) EslHandshake;
```

simpler：

```98:104:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/runtime.h
struct Handshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile CoreType core_type;
    volatile uint32_t physical_core_id;
} __attribute__((aligned(64)));
```

---

### 3. prepare / publish

- **为什么修改**：门铃必须对应确定 payload 槽；批量 prepare 后 `wmb` 再 publish。
- **怎么修改**：`reg_task_id&1` 选槽；`function_bin_addr` 填 fake；publish 写 `DATA_MAIN_BASE`。
- **相关代码**：

```118:136:esl_proxy/src/algorithm/dispatch_payload.c
    reg_task_id = dispatch_next_reg_task_id(core);
    slot = (int)(reg_task_id & 1u);
    p = (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
    /* ... */
    fake_kernel_addr = runtime->func_id_to_addr_[...];
    build_payload(p, desc, block_idx, fake_kernel_addr, not_ready);
    handle.reg_task_id = reg_task_id;
    return handle;
}
void esl_publish_subtask_to_core(EslPublishHandle handle)
{
    write_reg(handle.reg_addr, REG_ID_DATA_MAIN_BASE, handle.reg_task_id);
}
```

- **对应 simpler**：`payload_per_core_[core][buf_idx]` + `write_reg(DATA_MAIN_BASE)`。
- **相比 simpler 的区别**：esl 填 fake 入口；simpler 填真实 kernel 并默认双槽数组。对照：

esl_proxy：

```122:125:esl_proxy/src/algorithm/dispatch_payload.c
    fake_kernel_addr = runtime->func_id_to_addr_[runtime->workers[core].core_type == 0
                                                      ? ESL_FAKE_KERNEL_FUNC_ID_AIC
                                                      : ESL_FAKE_KERNEL_FUNC_ID_AIV];
    build_payload(p, desc, block_idx, fake_kernel_addr, not_ready);
```

simpler：

```136:138:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/scheduler/scheduler_context.h
    // Per-core dispatch payload storage: dual-buffer for pipelining.
    // buf_idx = reg_task_id & 1; adjacent dispatches alternate automatically.
    PTO2DispatchPayload payload_per_core_[RUNTIME_MAX_WORKER][2];
```

---

### 4. fake kernel

- **为什么修改**：先验调度闭环，不引入真实算子。
- **怎么修改**：`fake_kernel_run` 按时长忙等。
- **相关代码**：

```11:19:esl_proxy/include/algorithm/fake_kernel.h
__aicore__ __attribute__((always_inline)) static inline void fake_kernel_run(uint64_t duration_ns, uint64_t jitter_mask) {
    uint64_t start = esl_aicore_now_ns();
    int64_t wait_ns = (int64_t)duration_ns + (int64_t)(start & jitter_mask) - (int64_t)((jitter_mask + 1U) / 2U);
    uint64_t end = start + (uint64_t)wait_ns;
    while (esl_aicore_now_ns() < end) { /* spin */ }
}
```

- **对应 simpler**：`execute_task` 调真实 `UnifiedKernelFunc`。
- **相比 simpler 的区别**：忙等 vs 真实算子。对照：

esl_proxy：

```11:18:esl_proxy/include/algorithm/fake_kernel.h
__aicore__ __attribute__((always_inline)) static inline void fake_kernel_run(uint64_t duration_ns, uint64_t jitter_mask) {
    uint64_t start = esl_aicore_now_ns();
    int64_t wait_ns = (int64_t)duration_ns + (int64_t)(start & jitter_mask) - (int64_t)((jitter_mask + 1U) / 2U);
    uint64_t end = start + (uint64_t)wait_ns;
    while (esl_aicore_now_ns() < end) {
        /* spin */
    }
}
```

simpler：

```36:44:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/aicore/aicore_executor.cpp
__aicore__ __attribute__((always_inline)) static void execute_task(__gm__ PTO2DispatchPayload *payload) {
    if (payload == nullptr || payload->function_bin_addr == 0) {
        return;
    }
    UnifiedKernelFunc kernel = (UnifiedKernelFunc)payload->function_bin_addr;
    kernel(reinterpret_cast<__gm__ int64_t *>(payload->args));
    OUT_OF_ORDER_STORE_BARRIER();
}
```

---

### 5. AICore 执行循环

- **为什么修改**：需要门铃 → ACK → 执行 → FIN。
- **怎么修改**：读 `DATA_MAIN_BASE`；`payload+(task_id&1)`；ACK；`execute_task`（fake）；FIN。
- **相关代码**：

```55:95:esl_proxy/src/algorithm/aicore_executor.c
    while (1) {
        reg_val = (uint32_t)read_reg(REG_ID_DATA_MAIN_BASE);
        /* ... */
        __gm__ EslDispatchPayload *exec_payload = payload_base + (task_id & 1u);
        write_reg(REG_ID_COND, MAKE_ACK_VALUE(task_id));
        execute_task(exec_payload);
        write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
    }
```

- **对应 simpler**：同协议；执行体为真实 kernel，另有 `src_payload` gate。
- **相比 simpler 的区别**：esl 无真实算子 / 无完整 gated 填参。对照：

esl_proxy：

```68:95:esl_proxy/src/algorithm/aicore_executor.c
            uint32_t task_id = reg_val;
            __gm__ EslDispatchPayload *exec_payload = payload_base + (task_id & 1u);
            dcci(exec_payload, ENTIRE_DATA_CACHE);
            write_reg(REG_ID_COND, MAKE_ACK_VALUE(task_id));
            execute_task(exec_payload);
            last_reg_val = reg_val;
            write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
```

simpler：

```157:231:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/aicore/aicore_executor.cpp
            __gm__ PTO2DispatchPayload *exec_payload = payload + (task_id & 1u);
            dcci(exec_payload, ENTIRE_DATA_CACHE);
            if (exec_payload->src_payload != 0) {
                /* gated: fill args from src_payload, wait doorbell ... */
            }
            write_reg(RegId::COND, MAKE_ACK_VALUE(task_id));
            execute_task(exec_payload);
            last_reg_val = reg_val;
            write_reg(RegId::COND, MAKE_FIN_VALUE(task_id));
```

---

### 6. Dispatch：Fake Return → COND + prepare/publish

- **为什么修改**：Fake Return 无法形成硬件完成闭环。
- **怎么修改**：poll COND；从 `ctrl->ready_queue` 取货后 prepare/`wmb`/publish；basic 只发 CUBE/VECTOR。
- **相关代码**：`src/algorithm/dispatch.c`（`dispatch_poll`、`send_task`）。
- **对应 simpler**：`ready_queues[shape]` + scheduler dispatch/completion。
- **相比 simpler 的区别**：esl 用 `ctrl_t.ready_queue`；本 PR 无完整 SPMD/MIX。对照：

esl_proxy（取货）：

```text
batch_dequeue(&ctrl->ready_queue[type], ...)
→ esl_prepare_subtask_to_core → wmb → esl_publish_subtask_to_core
```

simpler（按 shape 队列）：

```180:190:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/shared/pto_runtime2_init.cpp
                &sched->ready_queues[i], arena, layout.off_ready_queue_slots[i], layout.ready_queue_capacity
```

---

### 7. Cutter / shm：BSS；`advance_task_id`

- **为什么修改**：AICPU 不宜 `malloc`；提交可见性与编号分离。
- **怎么修改**：`state_storage` / `predecessor_storage` BSS；`advance_task_id()`。
- **相关代码**：`cutter.c` / `ring_buf.h` / `shm.c`。
- **对应 simpler**：runtime arena / 主机分配设备缓冲。
- **相比 simpler 的区别**：固定 BSS vs arena。对照：

esl_proxy：

```text
/* shm.c / cutter.c：predecessor_storage、state_storage 为静态 BSS */
```

simpler：

```180:190:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/shared/pto_runtime2_init.cpp
                &sched->ready_queues[i], arena, layout.off_ready_queue_slots[i], layout.ready_queue_capacity
```

---

### 8. `TASK_TYPE_MIX=2` / duration ns / jitter

- **为什么修改**：解 MIX/VECTOR 冲突；假核需要时长参数。
- **怎么修改**：`task.h` 枚举与字段调整。
- **相关代码**：`include/algorithm/task.h`。
- **对应 simpler**：`PTO2ResourceShape::MIX = 2`。
- **相比 simpler 的区别**：枚举值对齐思路相同，但 A 阶段不下发完整 MIX。对照：

esl_proxy：

```text
/* task.h：TASK_TYPE_MIX = 2；duration uint32_t ns；jitter_mask */
```

simpler：

```60:65:simpler/src/a2a3/runtime/tensormap_and_ringbuffer/runtime/pto_submit_types.h
enum class PTO2ResourceShape : uint8_t {
    AIC = 0,    // Single AIC
    AIV = 1,    // Single AIV
    MIX = 2,    // Full cluster (dispatch uses active_mask)
    DUMMY = 3,  // Dependency-only (no AICore dispatch)
};
```

---

### 9. Platform onboard 三端

- **为什么修改**：需要 ACL/CANN 拉起与寄存器 HAL。
- **怎么修改**：`platform/onboard/**` + cmake 三端；**先 AICore 后 AICPU**。
- **相关代码**：`host_onboard.c`、`aicore_entry.cpp`、`tools/run_onboard.sh`。
- **对应 simpler**：`device_runner.cpp` 同样 AICore→AICPU。
- **相比 simpler 的区别**：esl Host 更薄。对照：

esl_proxy：

```text
/* host_onboard：ACL 分配后 launch AICore，再 launch AICPU */
```

simpler：

```448:459:simpler/src/a2a3/platform/onboard/host/device_runner.cpp
    LOG_INFO_V0("=== launch_aicore_kernel ===");
    rc = launch_aicore_kernel(stream_aicore_, kernel_args_.device_k_args_);
    /* ... */
    LOG_INFO_V0("=== launch_aicpu_kernel %s ===", host::KernelNames::RunName);
    int aicpu_launch_n = (runtime.get_aicpu_launch_count() > 0) ? runtime.get_aicpu_launch_count() : launch_aicpu_num;
    rc = launch_aicpu_kernel(stream_aicpu_, &kernel_args_.args, host::KernelNames::RunName, aicpu_launch_n);
```

---

### 10. Cases 接 fake 参数

- **为什么修改**：冒烟图要驱动 fake 时长。
- **怎么修改**：`new_task(..., duration_ns, jitter_mask)` + `advance_task_id`。
- **相关代码**：`cases/paged_attention_unroll_manual_scope.h` 等。
- **对应 simpler**：PA/qwen3 examples + ST（真实算子）。
- **相比 simpler 的区别**：完成数 vs 数值。对照：

esl_proxy：

```c
new_task(g_task_id, TASK_TYPE_CUBE, 1, DUR_QK_MATMUL, MASK_QK_MATMUL);
```

simpler：

```text
simpler/examples/a2a3/.../paged_attention*
simpler/tests/st/a2a3/**/paged_attention*
```
