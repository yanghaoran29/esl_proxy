/*
 * AICore kernel — merged entry + executor + fake_kernel (ccec).
 */
#include "aicore.h"
#include "kernel_args.h"
#include "esl_runtime.h"
#include "onboard_config.h"
#include "onboard_tensor.h"


#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

#ifdef __DAV_VEC__
#define KERNEL_ENTRY(x) x##_0_mix_aiv
#else
#define KERNEL_ENTRY(x) x##_0_mix_aic
#endif

extern "C" __attribute__((weak)) __aicore__ void fake_kernel(__gm__ EslFakeDispatchPayload *payload)
{
    if (payload == nullptr) {
        return;
    }

    const uint16_t task_id = payload->task.id;
    uint16_t tensor_cnt = payload->task.tensor_cnt;
    uint16_t scalar_cnt = payload->task.scalar_cnt;
    if (tensor_cnt > ESL_ONBOARD_MAX_TENSOR_ARGS) {
        tensor_cnt = ESL_ONBOARD_MAX_TENSOR_ARGS;
    }
    if (scalar_cnt > ESL_ONBOARD_MAX_SCALAR_ARGS) {
        scalar_cnt = ESL_ONBOARD_MAX_SCALAR_ARGS;
    }

    __gm__ int64_t *args = reinterpret_cast<__gm__ int64_t *>(payload->args);

    uint64_t duration = static_cast<uint64_t>(payload->duration_ticks);
    duration += static_cast<uint64_t>(task_id & 0x1Fu);

    for (uint16_t i = 0; i < tensor_cnt; ++i) {
        __gm__ EslOnboardTensor *t = reinterpret_cast<__gm__ EslOnboardTensor *>(args[i]);
        if (t != nullptr) {
            dcci(t, SINGLE_CACHE_LINE);
            duration += static_cast<uint64_t>(t->shapes[0] & 0x3u);
        }
    }

    for (uint16_t i = 0; i < scalar_cnt; ++i) {
        duration += static_cast<uint64_t>(args[tensor_cnt + i]) & 0x7u;
    }

    uint64_t start;
    asm volatile("MOV %0, SYS_CNT\n" : "+l"(start));

    for (;;) {
        uint64_t now;
        asm volatile("MOV %0, SYS_CNT\n" : "+l"(now));
        if (now - start >= duration) {
            break;
        }
    }
}

__aicore__ __attribute__((weak)) void aicore_execute(__gm__ EslRuntime *runtime, int block_idx, CoreType worker_core_type)
{
    __gm__ EslHandshake *my_hank = (__gm__ EslHandshake *)(&runtime->workers[block_idx]);

    while (my_hank->aicpu_ready == 0) {
        dcci(my_hank, SINGLE_CACHE_LINE);
        SPIN_WAIT_HINT();
    }

    my_hank->physical_core_id = get_physical_core_id();
    OUT_OF_ORDER_STORE_BARRIER();
    my_hank->aicore_regs_ready = 1;
    dcci(&my_hank->aicore_regs_ready, SINGLE_CACHE_LINE, CACHELINE_OUT);
    while (my_hank->aicpu_regs_ready == 0) {
        dcci(&my_hank->aicpu_regs_ready, SINGLE_CACHE_LINE);
        SPIN_WAIT_HINT();
    }

    write_reg(REG_ID_COND, AICORE_IDLE_VALUE);

    my_hank->core_type = static_cast<int32_t>(worker_core_type);
    OUT_OF_ORDER_STORE_BARRIER();
    my_hank->aicore_done = static_cast<uint32_t>(block_idx + 1);
    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);

    __gm__ EslFakeDispatchPayload *payload_base =
        reinterpret_cast<__gm__ EslFakeDispatchPayload *>(my_hank->task);

    uint32_t reg_val = AICPU_IDLE_TASK_ID;
    uint32_t last_reg_val = AICPU_IDLE_TASK_ID;

    while (true) {
        reg_val = static_cast<uint32_t>(read_reg(REG_ID_DATA_MAIN_BASE));
        if (reg_val == AICORE_EXIT_SIGNAL) {
            write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
            break;
        }

        if (reg_val == AICPU_IDLE_TASK_ID || reg_val == last_reg_val) {
            SPIN_WAIT_HINT();
            continue;
        }

        uint32_t task_id = reg_val;
        __gm__ EslFakeDispatchPayload *exec_payload = payload_base + (task_id & 1u);
        dcci(exec_payload, ENTIRE_DATA_CACHE);

        write_reg(REG_ID_COND, MAKE_ACK_VALUE(task_id));

        fake_kernel(exec_payload);

        last_reg_val = reg_val;
        write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
    }

    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);
}


#ifdef __DAV_VEC__
[[block_local]] int block_idx_aiv;
[[block_local]] CoreType core_type_aiv;
#else
[[block_local]] int block_idx_aic;
[[block_local]] CoreType core_type_aic;
#endif

extern "C" __global__ __aicore__ void KERNEL_ENTRY(aicore_kernel)(__gm__ KernelArgs *k_args)
{
#ifdef __DAV_VEC__
    block_idx_aiv = get_block_idx() * get_subblockdim() + get_subblockid() + get_block_num();
    core_type_aiv = CoreType::AIV;
    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aiv, core_type_aiv);
#else
    block_idx_aic = get_block_idx();
    core_type_aic = CoreType::AIC;
    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aic, core_type_aic);
#endif
}
