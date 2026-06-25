/*
 * L2 swimlane onboard hooks for AICore (aicore_kernel.cpp).
 * ESL_PROXY_ENABLE_L2_SWIMLANE conditionals live here only.
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_AICORE_ONBOARD_H
#define ESL_PROXY_ESL_SWIMLANE_AICORE_ONBOARD_H

#include "kernel_args.h"
#include "l2_swimlane/aicore/l2_swimlane_collector_aicore.h"
#include "l2_swimlane/esl_swimlane_api.h"
#include "onboard_config.h"

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

__aicore__ static inline __gm__ L2SwimlaneActiveHead *esl_swimlane_onboard_resolve_head(
    __gm__ uint64_t *rotation_table, int worker_idx, uint32_t profiling_flag)
{
    if (!ESL_SWIMLANE_IS_FLAG_ON(profiling_flag) || rotation_table == nullptr) {
        return nullptr;
    }
    if (worker_idx < 0 || worker_idx >= RUNTIME_MAX_WORKER) {
        return nullptr;
    }
    __gm__ uint64_t *slot = &rotation_table[worker_idx];
    dcci(slot, SINGLE_CACHE_LINE);
    const uint64_t head_addr = *slot;
    if (head_addr == 0U) {
        return nullptr;
    }
    return reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(head_addr);
}

#define ESL_SWIMLANE_AICORE_KERNEL_ROTATION_TABLE(k_args, rotation_table_var) \
    do { \
        if (ESL_SWIMLANE_IS_FLAG_ON((k_args)->enable_profiling_flag) && \
            (k_args)->l2_swimlane_aicore_rotation_table != 0) { \
            (rotation_table_var) = reinterpret_cast<__gm__ uint64_t *>((k_args)->l2_swimlane_aicore_rotation_table); \
        } \
    } while (0)

#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) L2SwimlaneAicoreLocalState var = {nullptr, UINT32_MAX, 0}

#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) uint64_t start_var = get_sys_cnt_aicore()

#define ESL_SWIMLANE_AICORE_TASK_RECORD(rotation_table, block_idx, profiling_flag, swim_local, exec_payload, task_id, \
                                         start_time) \
    do { \
        __gm__ L2SwimlaneActiveHead *_esl_sw_head = \
            esl_swimlane_onboard_resolve_head((rotation_table), (block_idx), (profiling_flag)); \
        if (_esl_sw_head != nullptr) { \
            uint64_t _esl_sw_token = (exec_payload) != nullptr ? (uint64_t)(exec_payload)->task.id : (uint64_t)(task_id); \
            ESL_SWIMLANE_AICORE_RECORD_TASK(_esl_sw_head, &(swim_local), _esl_sw_token, (task_id), (start_time), \
                                            get_sys_cnt_aicore()); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_AICORE_KERNEL_ROTATION_TABLE(k_args, rotation_table_var) \
    ((void)(k_args), (void)(rotation_table_var))
#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) ((void)0)
#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) ((void)0)
#define ESL_SWIMLANE_AICORE_TASK_RECORD(rotation_table, block_idx, profiling_flag, swim_local, exec_payload, task_id, \
                                         start_time) \
    ((void)0)

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_AICORE_ONBOARD_H */
