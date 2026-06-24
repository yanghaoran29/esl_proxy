/*
 * Host onboard runner helpers — init / k_args wiring for esl_onboard_run().
 * Include from host_onboard.c (add ONBOARD_SRC/l2_swimlane to -I).
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_HOST_ONBOARD_H
#define ESL_PROXY_ESL_SWIMLANE_HOST_ONBOARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esl_runtime.h"
#include "l2_swimlane/esl_swimlane_host_c.h"
#include "onboard_config.h"
#include <acl/acl_rt.h>

#if ESL_PROXY_ENABLE_L2_SWIMLANE

static inline int esl_swimlane_host_onboard_parse_level(const char *env_val)
{
    int level = 0;

    if (env_val != NULL && env_val[0] != '\0') {
        level = atoi(env_val);
        if (level < 0) {
            level = 0;
        } else if (level > 4) {
            level = 4;
        }
    }
    return level;
}

#define ESL_SWIMLANE_HOST_ONBOARD_PARSE_ENV(level_var) \
    do { \
        (level_var) = esl_swimlane_host_onboard_parse_level(getenv("ESL_PROXY_L2_SWIMLANE_LEVEL")); \
        ESL_SWIMLANE_HOST_SET_LEVEL(level_var); \
        if ((level_var) > 0) { \
            fprintf(stderr, "[esl_proxy] L2 swimlane enabled level=%d (no PMU)\n", (level_var)); \
        } \
    } while (0)

#define ESL_SWIMLANE_HOST_ONBOARD_INIT_OR(level, device_id, rc_ptr, on_fail) \
    do { \
        if ((level) > 0) { \
            *(rc_ptr) = ESL_SWIMLANE_HOST_INIT(ESL_PROXY_ONBOARD_WORKER_COUNT, ESL_PROXY_AICPU_THREAD_NUM, \
                                               (device_id), "."); \
            if (*(rc_ptr) != 0) { \
                fprintf(stderr, "[esl_proxy] swimlane init failed: %d\n", *(rc_ptr)); \
                on_fail; \
            } \
        } \
    } while (0)

#define ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(k_args_ptr, level) \
    do { \
        if ((level) > 0) { \
            (k_args_ptr)->l2_swimlane_data_base = ESL_SWIMLANE_HOST_DATA_BASE(); \
            (k_args_ptr)->l2_swimlane_aicore_rotation_table = ESL_SWIMLANE_HOST_ROTATION_TABLE(); \
            ESL_SWIMLANE_PROFILING_FLAG_ON((k_args_ptr)->enable_profiling_flag); \
        } \
    } while (0)

/* Requires ACL_CHECK(call, msg) defined at the expansion site (host_onboard.c). */
#define ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(level, dev_runtime_dev_ptr) \
    do { \
        if ((level) > 0) { \
            EslRuntime dev_runtime_host; \
            int32_t core_types[ESL_PROXY_ONBOARD_WORKER_COUNT]; \
            int w; \
            memset(&dev_runtime_host, 0, sizeof(dev_runtime_host)); \
            ACL_CHECK(aclrtMemcpy(&dev_runtime_host, sizeof(dev_runtime_host), (dev_runtime_dev_ptr), \
                                  sizeof(EslRuntime), ACL_MEMCPY_DEVICE_TO_HOST), \
                      "D2H runtime for swimlane core types"); \
            for (w = 0; w < ESL_PROXY_ONBOARD_WORKER_COUNT; ++w) { \
                core_types[w] = dev_runtime_host.workers[w].core_type; \
            } \
            ESL_SWIMLANE_HOST_SET_CORE_TYPES(core_types, ESL_PROXY_ONBOARD_WORKER_COUNT); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_HOST_ONBOARD_PARSE_ENV(level_var) ((void)(level_var))
#define ESL_SWIMLANE_HOST_ONBOARD_INIT_OR(level, device_id, rc_ptr, on_fail) \
    ((void)(level), (void)(device_id), (void)(rc_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(k_args_ptr, level) ((void)(k_args_ptr), (void)(level))
#define ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(level, dev_runtime_dev_ptr) \
    ((void)(level), (void)(dev_runtime_dev_ptr))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_HOST_ONBOARD_H */
