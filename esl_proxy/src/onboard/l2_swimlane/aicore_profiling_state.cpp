/*
 * AICore-side per-core profiling state ([[block_local]] storage + getters/setters).
 * Compiled only with -DESL_PROXY_ENABLE_L2_SWIMLANE=1.
 */
#include "aicore.h"

#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

#include "aicore/aicore_profiling_state.h"

[[block_local]] static uint32_t s_aicore_profiling_flag;
[[block_local]] static __gm__ uint64_t *s_l2_swimlane_aicore_head_slot;
[[block_local]] static __gm__ L2SwimlaneActiveHead *s_l2_swimlane_aicore_head;

__attribute__((weak)) __aicore__ void set_aicore_profiling_flag(uint32_t flag)
{
    s_aicore_profiling_flag = flag;
}

__attribute__((weak)) __aicore__ uint32_t get_aicore_profiling_flag(void)
{
    return s_aicore_profiling_flag;
}

__attribute__((weak)) __aicore__ void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr)
{
    s_l2_swimlane_aicore_head_slot = slot_ptr;
    s_l2_swimlane_aicore_head = nullptr;
}

__attribute__((weak)) __aicore__ __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void)
{
    if (s_l2_swimlane_aicore_head_slot == nullptr) {
        return nullptr;
    }
    /* Re-deref when cached head is null: AIV subblocks in the same block share
     * [[block_local]] storage; the slot pointer may be republished after the
     * first (failed) resolve. Also invalidate before read for AICPU writes. */
    if (s_l2_swimlane_aicore_head == nullptr) {
        dcci(s_l2_swimlane_aicore_head_slot, SINGLE_CACHE_LINE);
        const uint64_t head_addr = *s_l2_swimlane_aicore_head_slot;
        if (head_addr != 0U) {
            s_l2_swimlane_aicore_head = reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(head_addr);
        }
    }
    return s_l2_swimlane_aicore_head;
}
