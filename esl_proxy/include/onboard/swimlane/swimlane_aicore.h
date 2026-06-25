/*
 * L2 swimlane AICore kernel hooks (aicore_kernel.cpp).
 */
#ifndef ESL_PROXY_SWIMLANE_AICORE_H
#define ESL_PROXY_SWIMLANE_AICORE_H

#include "kernel_args.h"
#include "swimlane/swimlane_device.h"

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#define ESL_SWIMLANE_IS_FLAG_ON(flags) GET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_AICORE_RECORD_TASK(head, local, task_token_raw, reg_task_id, start_ts, end_ts) \
    l2_swimlane_aicore_record_task((head), (local), (task_token_raw), (reg_task_id), (start_ts), (end_ts))

#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx) \
    do { \
        set_aicore_profiling_flag((k_args)->enable_profiling_flag); \
        if (ESL_SWIMLANE_IS_FLAG_ON((k_args)->enable_profiling_flag) && \
            (k_args)->l2_swimlane_aicore_rotation_table != 0) { \
            __gm__ uint64_t *_esl_sw_rot = \
                reinterpret_cast<__gm__ uint64_t *>((k_args)->l2_swimlane_aicore_rotation_table); \
            set_l2_swimlane_aicore_head_slot(&_esl_sw_rot[(block_idx)]); \
        } else { \
            set_l2_swimlane_aicore_head_slot(nullptr); \
        } \
    } while (0)

#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) L2SwimlaneAicoreLocalState var = {nullptr, UINT32_MAX, 0}
#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) uint64_t start_var = get_sys_cnt_aicore()

#define ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, exec_payload, task_id, start_time) \
    do { \
        __gm__ L2SwimlaneActiveHead *_esl_sw_head = get_l2_swimlane_aicore_head(); \
        if (_esl_sw_head != nullptr) { \
            uint64_t _esl_sw_token = (exec_payload) != nullptr ? (uint64_t)(exec_payload)->task.id : (uint64_t)(task_id); \
            ESL_SWIMLANE_AICORE_RECORD_TASK(_esl_sw_head, &(swim_local), _esl_sw_token, (task_id), (start_time), \
                                            get_sys_cnt_aicore()); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx) ((void)(k_args), (void)(block_idx))
#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) ((void)0)
#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) ((void)0)
#define ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, exec_payload, task_id, start_time) ((void)0)

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_AICORE_H */
