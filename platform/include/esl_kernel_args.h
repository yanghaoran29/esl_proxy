/*
 * KernelArgs layout for esl_proxy onboard host launcher.
 * Field order matches simpler a2a3 common/kernel_args.h for AICPU ABI.
 */
#ifndef ESL_PROXY_KERNEL_ARGS_H
#define ESL_PROXY_KERNEL_ARGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct EslRuntime;

struct EslKernelArgs {
    uint64_t unused[5];
    void *device_args;
    struct EslRuntime *runtime_args;
    uint64_t regs;
    uint64_t ffts_base_addr;
    uint64_t dump_data_base;
    uint64_t l2_swimlane_data_base;
    uint64_t pmu_data_base;
    uint64_t pmu_reg_addrs;
    uint64_t dep_gen_data_base;
    uint64_t scope_stats_data_base;
    uint64_t l2_swimlane_aicore_rotation_table;
    uint32_t log_level;
    uint32_t log_info_v;
    uint32_t enable_profiling_flag;
    uint32_t _pad;
    uint64_t device_wall_data_base;
    uint32_t device_id;
};

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_KERNEL_ARGS_H */
