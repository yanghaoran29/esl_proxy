/*
 * L2 swimlane host API + onboard runner hooks (host_onboard.c / esl_swimlane_host.cpp).
 */
#ifndef ESL_PROXY_SWIMLANE_HOST_H
#define ESL_PROXY_SWIMLANE_HOST_H

#include <stdint.h>

#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#define ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
#include "onboard_config.h"
#undef ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
#endif

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

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) SET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_HOST_SET_LEVEL(level) esl_swimlane_host_set_level(level)
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    esl_swimlane_host_init((workers), (aicpu_threads), (device_id), (prefix))
#define ESL_SWIMLANE_HOST_START() esl_swimlane_host_start()
#define ESL_SWIMLANE_HOST_STOP_EXPORT() esl_swimlane_host_stop_and_export()
#define ESL_SWIMLANE_HOST_FINALIZE() esl_swimlane_host_finalize()
#define ESL_SWIMLANE_HOST_DATA_BASE() esl_swimlane_host_data_base()
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() esl_swimlane_host_rotation_table()
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) esl_swimlane_host_set_core_types((types), (count))

#if !defined(ESL_SWIMLANE_HOST_CPP)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esl_runtime.h"
#include <acl/acl_rt.h>

static inline int swimlane_host_parse_level(const char *env_val)
{
    int level = 0;

    if (env_val != NULL && env_val[0] != '\0') {
        level = atoi(env_val);
        if (level < 0) {
            level = 0;
        } else if (level > 1) {
            level = 1;
        }
    }
    return level;
}

#define ESL_SWIMLANE_HOST_ONBOARD_PARSE_ENV(level_var) \
    do { \
        (level_var) = swimlane_host_parse_level(getenv("ESL_PROXY_L2_SWIMLANE_LEVEL")); \
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

#endif /* !ESL_SWIMLANE_HOST_CPP */

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) ((void)(flags))
#define ESL_SWIMLANE_HOST_SET_LEVEL(level) ((void)(level))
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    ((void)(workers), (void)(aicpu_threads), (void)(device_id), (void)(prefix), 0)
#define ESL_SWIMLANE_HOST_START() ((void)0)
#define ESL_SWIMLANE_HOST_STOP_EXPORT() ((void)0)
#define ESL_SWIMLANE_HOST_FINALIZE() ((void)0)
#define ESL_SWIMLANE_HOST_DATA_BASE() (0ULL)
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() (0ULL)
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) ((void)(types), (void)(count))

#define ESL_SWIMLANE_HOST_ONBOARD_PARSE_ENV(level_var) ((void)(level_var))
#define ESL_SWIMLANE_HOST_ONBOARD_INIT_OR(level, device_id, rc_ptr, on_fail) \
    ((void)(level), (void)(device_id), (void)(rc_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(k_args_ptr, level) ((void)(k_args_ptr), (void)(level))
#define ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(level, dev_runtime_dev_ptr) \
    ((void)(level), (void)(dev_runtime_dev_ptr))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_HOST_H */
