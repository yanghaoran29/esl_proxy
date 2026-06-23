/*
 * KernelArgs / EslKernelArgs — shared CANN kernel argument layout (Host, AICPU, AICore).
 */
#ifndef PLATFORM_COMMON_KERNEL_ARGS_H_
#define PLATFORM_COMMON_KERNEL_ARGS_H_

#include <stdint.h>

#include "esl_runtime.h"

#if defined(__DAV_VEC__) || defined(__DAV_CUBE__)
#define __may_used_by_aicore__ __gm__
#else
#define __may_used_by_aicore__
#endif

typedef struct DeviceArgs DeviceArgs;
typedef EslRuntime Runtime;

#ifdef __cplusplus
extern "C" {
#endif

struct KernelArgs {
    uint64_t unused[5];
    DeviceArgs *device_args;
    __may_used_by_aicore__ EslRuntime *runtime_args;
    uint64_t regs;
    uint64_t ffts_base_addr;
    uint64_t dump_data_base;
    uint64_t dep_gen_data_base;
    uint64_t scope_stats_data_base;
    uint32_t log_level;   /* DLOG_DEBUG(0)..DLOG_ERROR(3), see CANN log_types.h */
    uint32_t log_info_v;  /* min sub-level for LOG_INFO_V1..V9 (DLOG_INFO); V0 uses DLOG_DEBUG */
    uint32_t enable_profiling_flag;
    uint32_t _pad;
    uint64_t device_wall_data_base;
    uint32_t device_id;
};

typedef struct KernelArgs KernelArgs;
typedef struct KernelArgs EslKernelArgs;

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_COMMON_KERNEL_ARGS_H_ */
