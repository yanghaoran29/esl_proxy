/*
 * C-safe L2 swimlane hooks for host_onboard.c.
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_HOST_C_H
#define ESL_PROXY_ESL_SWIMLANE_HOST_C_H

#include <stdint.h>

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#include "esl_swimlane_host.h"

#define PROFILING_FLAG_L2_SWIMLANE (1u << 1)
#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) ((flags) |= PROFILING_FLAG_L2_SWIMLANE)

#define ESL_SWIMLANE_HOST_SET_LEVEL(level) esl_swimlane_host_set_level(level)
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    esl_swimlane_host_init((workers), (aicpu_threads), (device_id), (prefix))
#define ESL_SWIMLANE_HOST_START() esl_swimlane_host_start()
#define ESL_SWIMLANE_HOST_STOP_EXPORT() esl_swimlane_host_stop_and_export()
#define ESL_SWIMLANE_HOST_FINALIZE() esl_swimlane_host_finalize()
#define ESL_SWIMLANE_HOST_DATA_BASE() esl_swimlane_host_data_base()
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() esl_swimlane_host_rotation_table()
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) esl_swimlane_host_set_core_types((types), (count))

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

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_HOST_C_H */
