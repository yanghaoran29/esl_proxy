# PR-B：Sim 后端

相对基线：`pr/A-board-bringup`。

---

## 宏观：本 PR 完成了什么

在 **无 NPU** 的主机上复用 PR-A 同一套 algorithm（orch/cutter/dispatch + fake kernel + 门铃/COND 协议），用 **pthread + 内存模拟寄存器** 跑通 paged attention + basic dispatch。  
新增 `platform/sim/**`，`aicore_execute_wrapper` 进入与 onboard 相同的 `aicore_execute`。相对 simpler：对应 `platform/sim` 与共享 runtime 执行入口，实现更薄（C/pthread）。

---

## 微观：各部分怎么做

### 1. 新增 `platform/sim/**`

- **为什么修改**：无板时需同一套 algorithm 做主机回归。
- **怎么修改**：新增 `include/platform/sim/**`、`src/platform/sim/**`。
- **相关代码**：`esl_proxy/src/platform/sim/` 目录树。
- **对应 simpler**：`simpler/src/a2a3/platform/sim/{host,aicore,aicpu}/`。
- **相比 simpler 的区别**：C/pthread 薄实现 vs C++ sim 栈。对照：

esl_proxy：

```text
esl_proxy/src/platform/sim/device_runner.c
esl_proxy/src/platform/sim/aicore_wrapper.cpp
esl_proxy/src/platform/sim/platform_regs.c
```

simpler：

```text
simpler/src/a2a3/platform/sim/host/device_runner.cpp
simpler/src/a2a3/platform/sim/aicore/kernel.cpp
```

---

### 2. wrapper → `aicore_execute`

- **为什么修改**：sim/onboard 必须同一核侧状态机。
- **怎么修改**：wrapper 绑 TLS 后调 `aicore_execute`。
- **相关代码**：

```13:29:esl_proxy/src/platform/sim/aicore_wrapper.cpp
extern "C" void aicore_execute_wrapper(EslRuntime *runtime, int block_idx, CoreType core_type,
                                       uint32_t physical_core_id, uint64_t regs_table) {
    aicore_execute(runtime, block_idx, core_type);
}
```

- **对应 simpler**：sim `kernel.cpp` → runtime `aicore_executor`。
- **相比 simpler 的区别**：esl 直接复用 PR-A C 入口。对照：

esl_proxy：

```13:29:esl_proxy/src/platform/sim/aicore_wrapper.cpp
extern "C" void aicore_execute_wrapper(EslRuntime *runtime, int block_idx, CoreType core_type,
                                       uint32_t physical_core_id, uint64_t regs_table) {
    aicore_execute(runtime, block_idx, core_type);
}
```

simpler：

```11:14:simpler/src/a2a3/platform/sim/host/device_runner.cpp
/**
 * a2a3 sim DeviceRunner implementation — wired against a2a3's aicore_execute
 * signature + dep_gen support. Shared arena/tensor/callable lifecycle lives
 * on SimDeviceRunnerBase; see device_runner_base.cpp.
 */
```

---

### 3. 主机内存模拟寄存器

- **为什么修改**：无 MMIO 时仍走同一 `read_reg`/`write_reg`。
- **怎么修改**：`SimCoreReg` 存 COND / DATA_MAIN_BASE。
- **相关代码**：

```26:40:esl_proxy/src/platform/sim/platform_regs.c
uint64_t read_reg(uint64_t reg_base_addr, RegId reg) {
    SimCoreReg *cr = sim_core_reg_at(reg_base_addr);
    if (reg == REG_ID_COND) return (uint64_t)cr->cond;
    if (reg == REG_ID_DATA_MAIN_BASE) { /* EXIT / doorbell from host memory */ }
}
```

- **对应 simpler**：sim HAL 抽象寄存器（算法不绑死 onboard）。
- **相比 simpler 的区别**：实现为进程内结构体。对照：

esl_proxy：

```26:30:esl_proxy/src/platform/sim/platform_regs.c
uint64_t read_reg(uint64_t reg_base_addr, RegId reg) {
    SimCoreReg *cr = sim_core_reg_at(reg_base_addr);
    if (reg == REG_ID_COND) return (uint64_t)cr->cond;
```

simpler：

```text
simpler/src/a2a3/platform/sim/…  # sim 寄存器/上下文（cpu_sim_context 等）
# 与 onboard 共用 runtime aicore_execute / Handshake 协议
```

---

### 4. pthread AICore workers

- **为什么修改**：需要并发核侧循环配合 dispatch。
- **怎么修改**：`esl_sim_aicore_workers_start` 每核一线程。
- **相关代码**：`src/platform/sim/device_runner.c`。
- **对应 simpler**：`platform/sim/host/device_runner.cpp` pthread。
- **相比 simpler 的区别**：启动 API 贴合 Makefile。对照：

esl_proxy：

```text
esl_sim_aicore_workers_start(runtime) → pthread → aicore_execute_wrapper
```

simpler：

```11:14:simpler/src/a2a3/platform/sim/host/device_runner.cpp
/**
 * a2a3 sim DeviceRunner implementation — wired against a2a3's aicore_execute
 * signature + dep_gen support.
 */
```
