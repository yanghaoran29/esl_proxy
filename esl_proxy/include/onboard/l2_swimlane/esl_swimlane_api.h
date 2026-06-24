/*
 * Optional L2 swimlane hooks — compile with -DESL_PROXY_ENABLE_L2_SWIMLANE=1.
 * When disabled (default), every macro expands to a no-op / zero / NULL.
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_API_H
#define ESL_PROXY_ESL_SWIMLANE_API_H

#include <stdint.h>

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#include "aicpu/l2_swimlane_collector_aicpu.h"
#include "aicore/aicore_profiling_state.h"
#include "aicore/l2_swimlane_collector_aicore.h"
#include "common/l2_swimlane_profiling.h"

#ifdef __cplusplus
extern "C" {
#endif

void esl_swimlane_host_set_level(int level);
int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix);
void esl_swimlane_host_start(void);
void esl_swimlane_host_stop_and_export(void);
void esl_swimlane_host_finalize(void);
uint64_t esl_swimlane_host_data_base(void);
uint64_t esl_swimlane_host_rotation_table(void);
void esl_swimlane_host_set_core_types(const int32_t *core_types, int count);

#ifdef __cplusplus
}
#endif

#define ESL_SWIMLANE_ENABLED() is_l2_swimlane_enabled()
#define ESL_SWIMLANE_LEVEL() ((uint32_t)get_l2_swimlane_level())

#define ESL_SWIMLANE_HOST_SET_LEVEL(level) esl_swimlane_host_set_level(level)
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    esl_swimlane_host_init((workers), (aicpu_threads), (device_id), (prefix))
#define ESL_SWIMLANE_HOST_START() esl_swimlane_host_start()
#define ESL_SWIMLANE_HOST_STOP_EXPORT() esl_swimlane_host_stop_and_export()
#define ESL_SWIMLANE_HOST_FINALIZE() esl_swimlane_host_finalize()
#define ESL_SWIMLANE_HOST_DATA_BASE() esl_swimlane_host_data_base()
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() esl_swimlane_host_rotation_table()
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) esl_swimlane_host_set_core_types((types), (count))

#define ESL_SWIMLANE_AICPU_SET_BASE(addr) set_platform_l2_swimlane_base(addr)
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) set_platform_l2_swimlane_aicore_rotation_table(addr)
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) set_l2_swimlane_enabled(on)
#define ESL_SWIMLANE_AICPU_INIT(workers) l2_swimlane_aicpu_init(workers)
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    l2_swimlane_aicpu_flush((thread_idx), (cores), (core_num))

#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) \
    l2_swimlane_aicpu_on_aicore_dispatch((core_id), (thread_idx))
#define ESL_SWIMLANE_AICPU_COMPLETE_TASK(core_id, thread_idx, reg_task_id, dispatch_ts, finish_ts) \
    l2_swimlane_aicpu_complete_task((core_id), (thread_idx), (reg_task_id), (dispatch_ts), (finish_ts))

#define ESL_SWIMLANE_AICPU_INIT_PHASE(workers, sched_threads, orch_threads) \
    l2_swimlane_aicpu_init_phase((workers), (sched_threads), (orch_threads))
#define ESL_SWIMLANE_AICPU_RECORD_SCHED_PHASE(thread_idx, kind, start, end, loop_iter, tasks, hit, miss, las, sas, lae, \
                                              sae) \
    l2_swimlane_aicpu_record_sched_phase((thread_idx), (kind), (start), (end), (loop_iter), (tasks), (hit), (miss), \
                                         (las), (sas), (lae), (sae))
#define ESL_SWIMLANE_AICPU_SET_ORCH_THREAD(idx) l2_swimlane_aicpu_set_orch_thread_idx(idx)
#define ESL_SWIMLANE_AICPU_RECORD_ORCH_PHASE(start, end, task_id, submit_idx) \
    l2_swimlane_aicpu_record_orch_phase((start), (end), (task_id), (submit_idx))

#define ESL_SWIMLANE_AICORE_RECORD_TASK(head, local, task_token_raw, reg_task_id, start_ts, end_ts) \
    l2_swimlane_aicore_record_task((head), (local), (task_token_raw), (reg_task_id), (start_ts), (end_ts))

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) SET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_IS_FLAG_ON(flags) GET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)

#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx)                                              \
    do {                                                                                                 \
        set_aicore_profiling_flag((k_args)->enable_profiling_flag);                                      \
        __gm__ uint64_t *_esl_sw_rot =                                                                   \
            (ESL_SWIMLANE_IS_FLAG_ON((k_args)->enable_profiling_flag) &&                                \
             (k_args)->l2_swimlane_aicore_rotation_table != 0)                                         \
                ? reinterpret_cast<__gm__ uint64_t *>((k_args)->l2_swimlane_aicore_rotation_table)     \
                : nullptr;                                                                               \
        set_l2_swimlane_aicore_head_slot(_esl_sw_rot != nullptr ? &_esl_sw_rot[(block_idx)] : nullptr); \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_ENABLED() (0)
#define ESL_SWIMLANE_LEVEL() (0u)

#define ESL_SWIMLANE_HOST_SET_LEVEL(level) ((void)(level))
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    ((void)(workers), (void)(aicpu_threads), (void)(device_id), (void)(prefix), 0)
#define ESL_SWIMLANE_HOST_START() ((void)0)
#define ESL_SWIMLANE_HOST_STOP_EXPORT() ((void)0)
#define ESL_SWIMLANE_HOST_FINALIZE() ((void)0)
#define ESL_SWIMLANE_HOST_DATA_BASE() (0ULL)
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() (0ULL)
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) ((void)(types), (void)(count))

#define ESL_SWIMLANE_AICPU_SET_BASE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) ((void)(on))
#define ESL_SWIMLANE_AICPU_INIT(workers) ((void)(workers))
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    ((void)(thread_idx), (void)(cores), (void)(core_num))

#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) ((void)(core_id), (void)(thread_idx))
#define ESL_SWIMLANE_AICPU_COMPLETE_TASK(core_id, thread_idx, reg_task_id, dispatch_ts, finish_ts) \
    ((void)(core_id), (void)(thread_idx), (void)(reg_task_id), (void)(dispatch_ts), (void)(finish_ts), 0)

#define ESL_SWIMLANE_AICPU_INIT_PHASE(workers, sched_threads, orch_threads) \
    ((void)(workers), (void)(sched_threads), (void)(orch_threads))
#define ESL_SWIMLANE_AICPU_RECORD_SCHED_PHASE(thread_idx, kind, start, end, loop_iter, tasks, hit, miss, las, sas, lae, \
                                              sae) \
    ((void)(thread_idx), (void)(kind), (void)(start), (void)(end), (void)(loop_iter), (void)(tasks), (void)(hit), \
     (void)(miss), (void)(las), (void)(sas), (void)(lae), (void)(sae))
#define ESL_SWIMLANE_AICPU_SET_ORCH_THREAD(idx) ((void)(idx))
#define ESL_SWIMLANE_AICPU_RECORD_ORCH_PHASE(start, end, task_id, submit_idx) \
    ((void)(start), (void)(end), (void)(task_id), (void)(submit_idx))

#define ESL_SWIMLANE_AICORE_RECORD_TASK(head, local, task_token_raw, reg_task_id, start_ts, end_ts) \
    ((void)(head), (void)(local), (void)(task_token_raw), (void)(reg_task_id), (void)(start_ts), (void)(end_ts))

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) ((void)(flags))
#define ESL_SWIMLANE_IS_FLAG_ON(flags) (0)

#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx) ((void)(k_args), (void)(block_idx))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_API_H */
