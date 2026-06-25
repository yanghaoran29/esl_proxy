/*
 * AICore executor — handshake + dispatch loop (algorithm layer, ccec).
 */
#include "aicore.h"
#include "runtime.h"
#include "onboard_config.h"
#include "fake_kernel.h"
#include "swimlane_aicore.h"
#include "aicore_profiling_state.h"

#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

__aicore__ static uint64_t esl_fake_now_sys_to_ns(void)
{
    return get_sys_cnt_aicore() * 1000000000ULL / ESL_ONBOARD_SYS_CNT_FREQ;
}

extern "C" __attribute__((weak)) __aicore__ void fake_kernel(__gm__ EslFakeDispatchPayload *payload)
{
    if (payload == nullptr) {
        return;
    }

    esl_fake_kernel_busy_wait_ns(static_cast<uint64_t>(payload->duration_ticks),
                                 static_cast<uint64_t>(payload->jitter_mask), esl_fake_now_sys_to_ns);
}

__aicore__ __attribute__((weak)) void aicore_execute(
    __gm__ EslRuntime *runtime, int block_idx, CoreType worker_core_type, uint32_t profiling_flag,
    uint64_t rotation_table)
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
    ESL_SWIMLANE_AICORE_LOCAL_STATE(swim_local);
    __gm__ L2SwimlaneActiveHead *l2_swimlane_head = nullptr;
    if (ESL_SWIMLANE_IS_FLAG_ON(profiling_flag) && rotation_table != 0U) {
        __gm__ uint64_t *head_slot =
            reinterpret_cast<__gm__ uint64_t *>(rotation_table) + block_idx;
        dcci(head_slot, SINGLE_CACHE_LINE);
        l2_swimlane_head = reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(*head_slot);
    }

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

        ESL_SWIMLANE_AICORE_TASK_BEGIN(start_time);
        fake_kernel(exec_payload);
        ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, l2_swimlane_head, exec_payload, task_id, start_time);

        last_reg_val = reg_val;
        write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
    }

    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);
}
