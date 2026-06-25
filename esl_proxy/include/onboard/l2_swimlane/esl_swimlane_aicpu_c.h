/*
 * C-safe L2 swimlane hooks for AICPU (.c) sources.
 * Compile with -DESL_PROXY_ENABLE_L2_SWIMLANE=1 to enable collection.
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_AICPU_C_H
#define ESL_PROXY_ESL_SWIMLANE_AICPU_C_H

#include <stdbool.h>
#include <stdint.h>

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#include "tools.h"

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base);
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr);
void set_l2_swimlane_enabled(bool enable);
bool is_l2_swimlane_enabled(void);
void l2_swimlane_aicpu_init(int worker_count);
void l2_swimlane_aicpu_set_orch_thread_idx(int thread_idx);
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx);
int l2_swimlane_aicpu_complete_task(
    int core_id, int thread_idx, uint32_t reg_task_id, uint64_t dispatch_time, uint64_t finish_time);
void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num);

#ifdef __cplusplus
}
#endif

#define PROFILING_FLAG_L2_SWIMLANE (1u << 1)
#define ESL_SWIMLANE_IS_FLAG_ON(flags) ((((uint32_t)(flags)) & PROFILING_FLAG_L2_SWIMLANE) != 0u)

#define ESL_SWIMLANE_AICPU_SET_BASE(addr) set_platform_l2_swimlane_base(addr)
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) set_platform_l2_swimlane_aicore_rotation_table(addr)
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) set_l2_swimlane_enabled(on)
#define ESL_SWIMLANE_AICPU_INIT(workers) l2_swimlane_aicpu_init(workers)
#define ESL_SWIMLANE_AICPU_SET_ORCH_THREAD(idx) l2_swimlane_aicpu_set_orch_thread_idx(idx)
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    l2_swimlane_aicpu_flush((thread_idx), (cores), (core_num))
#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) \
    l2_swimlane_aicpu_on_aicore_dispatch((core_id), (thread_idx))
#define ESL_SWIMLANE_AICPU_DISPATCH_TS(arr, exe_type, core, slot) \
    ((arr)[(exe_type)][(core)][(slot)])
#define ESL_SWIMLANE_AICPU_RECORD_DISPATCH_TS(arr, exe_type, core, slot) \
    ((arr)[(exe_type)][(core)][(slot)] = esl_onboard_sys_cnt())
#define ESL_SWIMLANE_AICPU_COMPLETE_TASK(core_id, thread_idx, reg_task_id, dispatch_ts) \
    l2_swimlane_aicpu_complete_task((core_id), (thread_idx), (reg_task_id), (dispatch_ts), \
                                   esl_onboard_sys_cnt())

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_IS_FLAG_ON(flags) (0)
#define ESL_SWIMLANE_AICPU_SET_BASE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) ((void)(on))
#define ESL_SWIMLANE_AICPU_INIT(workers) ((void)(workers))
#define ESL_SWIMLANE_AICPU_SET_ORCH_THREAD(idx) ((void)(idx))
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    ((void)(thread_idx), (void)(cores), (void)(core_num))
#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) ((void)(core_id), (void)(thread_idx))
#define ESL_SWIMLANE_AICPU_DISPATCH_TS(arr, exe_type, core, slot) ((uint64_t)0)
#define ESL_SWIMLANE_AICPU_RECORD_DISPATCH_TS(arr, exe_type, core, slot) ((void)0)
#define ESL_SWIMLANE_AICPU_COMPLETE_TASK(core_id, thread_idx, reg_task_id, dispatch_ts) \
    ((void)(core_id), (void)(thread_idx), (void)(reg_task_id), (void)(dispatch_ts), 0)

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_AICPU_C_H */
