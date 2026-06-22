/*
 * AICore kernel entry — ported from simpler a2a3/platform/onboard/aicore/kernel.cpp
 */
#include "aicore/aicore.h"
#include "aicore/aicore_profiling_state.h"
#include "common/core_type.h"
#include "common/kernel_args.h"
#include "common/l2_swimlane_profiling.h"

#ifdef __DAV_VEC__
#define KERNEL_ENTRY(x) x##_0_mix_aiv
#define block_idx block_idx_aiv
#define core_type core_type_aiv
#else
#define KERNEL_ENTRY(x) x##_0_mix_aic
#define block_idx block_idx_aic
#define core_type core_type_aic
#endif

[[block_local]] int block_idx;
[[block_local]] CoreType core_type;

[[block_local]] static uint32_t s_aicore_profiling_flag;
[[block_local]] static __gm__ uint64_t *s_l2_swimlane_aicore_head_slot;
[[block_local]] static __gm__ L2SwimlaneActiveHead *s_l2_swimlane_aicore_head;

__attribute__((weak)) __aicore__ void set_aicore_profiling_flag(uint32_t flag) { s_aicore_profiling_flag = flag; }
__attribute__((weak)) __aicore__ uint32_t get_aicore_profiling_flag() { return s_aicore_profiling_flag; }

__attribute__((weak)) __aicore__ void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr)
{
    s_l2_swimlane_aicore_head_slot = slot_ptr;
    s_l2_swimlane_aicore_head = nullptr;
}

__attribute__((weak)) __aicore__ __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head()
{
    if (s_l2_swimlane_aicore_head == nullptr && s_l2_swimlane_aicore_head_slot != nullptr) {
        s_l2_swimlane_aicore_head =
            reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(*s_l2_swimlane_aicore_head_slot);
    }
    return s_l2_swimlane_aicore_head;
}

extern __aicore__ void aicore_execute(__gm__ Runtime *runtime, int block_idx, CoreType core_type);

extern "C" __global__ __aicore__ void KERNEL_ENTRY(aicore_kernel)(__gm__ KernelArgs *k_args)
{
#ifdef __DAV_VEC__
    block_idx = get_block_idx() * get_subblockdim() + get_subblockid() + get_block_num();
    core_type = CoreType::AIV;
#else
    block_idx = get_block_idx();
    core_type = CoreType::AIC;
#endif

    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    set_aicore_profiling_flag(k_args->enable_profiling_flag);

    if (GET_PROFILING_FLAG(k_args->enable_profiling_flag, PROFILING_FLAG_L2_SWIMLANE) &&
        k_args->l2_swimlane_aicore_rotation_table != 0) {
        __gm__ uint64_t *head_table = reinterpret_cast<__gm__ uint64_t *>(k_args->l2_swimlane_aicore_rotation_table);
        set_l2_swimlane_aicore_head_slot(&head_table[block_idx]);
    } else {
        set_l2_swimlane_aicore_head_slot(nullptr);
    }

    aicore_execute(k_args->runtime_args, block_idx, core_type);
}
