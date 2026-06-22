/*
 * AICPU kernel entry — ported from simpler a2a3/platform/onboard/aicpu/kernel.cpp
 * Calls esl_aicpu_execute instead of tensormap aicpu_execute.
 */
#include <cstdio>

#include "aicpu/device_log.h"
#include "aicpu/device_time.h"
#include "aicpu/platform_aicpu_affinity.h"
#include "aicpu/platform_regs.h"
#include "common/kernel_args.h"
#include "common/platform_config.h"
#include "common/unified_log.h"
#include "esl_runtime.h"

extern "C" int32_t esl_aicpu_execute(EslRuntime *runtime);

static uint64_t g_device_start_cycle = 0;

extern "C" __attribute__((visibility("default"))) int simpler_aicpu_init(void *arg)
{
    init_log_switch();
    if (arg == nullptr) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    auto k_args = (KernelArgs *)arg;
    set_log_level(static_cast<int>(k_args->log_level));
    set_log_info_v(static_cast<int>(k_args->log_info_v));

    g_device_start_cycle = get_sys_cnt_aicpu();
    if (k_args->device_wall_data_base != 0) {
        *reinterpret_cast<uint64_t *>(k_args->device_wall_data_base) = 0;
    }

    LOG_INFO_V0("%s", "esl_proxy AICPU Init");
    return 0;
}

extern "C" __attribute__((visibility("default"))) int simpler_aicpu_exec(void *arg)
{
    if (arg == nullptr) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }

    auto k_args = (KernelArgs *)arg;
    EslRuntime *runtime = reinterpret_cast<EslRuntime *>(k_args->runtime_args);
    if (runtime == nullptr) {
        LOG_ERROR("%s", "Invalid runtime_args: null pointer");
        return -1;
    }

    set_log_level(static_cast<int>(k_args->log_level));
    set_log_info_v(static_cast<int>(k_args->log_info_v));
    set_platform_regs(k_args->regs);

    /* M1: CANN over-launches to PLATFORM_MAX_AICPU_THREADS_JUST_FOR_LAUNCH;
     * esl_proxy assigns cutter/dispatch/orch to the first 3 threads that
     * enter esl_aicpu_execute (see executor.cpp). Skip cluster gate so we
     * do not drop a required role by topology. */
    (void)runtime;

    int rc = esl_aicpu_execute(runtime);
    if (rc != 0) {
        LOG_ERROR("esl_aicpu_execute failed rc=%d", rc);
        return rc;
    }

    uint64_t my_end = get_sys_cnt_aicpu();
    if (k_args->device_wall_data_base != 0 && my_end > g_device_start_cycle) {
        *reinterpret_cast<uint64_t *>(k_args->device_wall_data_base) =
            static_cast<uint64_t>(cycles_to_us(my_end - g_device_start_cycle) * 1000.0);
    }

    return rc;
}
