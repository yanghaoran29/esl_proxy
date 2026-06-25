/*
 * AICore-side per-core profiling state (set/get). Storage lives in aicore_kernel.cpp.
 */
#ifndef ESL_PROXY_AICORE_PROFILING_STATE_H
#define ESL_PROXY_AICORE_PROFILING_STATE_H

#include <stdint.h>

#include "swimlane_types.h"

__aicore__ void set_aicore_profiling_flag(uint32_t flag);
__aicore__ uint32_t get_aicore_profiling_flag(void);
__aicore__ void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr);
__aicore__ __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void);

#endif /* ESL_PROXY_AICORE_PROFILING_STATE_H */
