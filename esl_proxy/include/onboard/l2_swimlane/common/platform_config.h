/*
 * Platform profiling constants for L2 swimlane (ported from simpler a2a3).
 * Architectural limits come from onboard_config.h — do not re-alias PLATFORM_* here.
 */
#ifndef ESL_PROXY_L2_SWIMLANE_PLATFORM_CONFIG_H
#define ESL_PROXY_L2_SWIMLANE_PLATFORM_CONFIG_H

#include <stdint.h>

#include "onboard_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_PROF_BUFFER_SIZE 1000
#define PLATFORM_PROF_SLOT_COUNT 4
#define PLATFORM_PROF_BUFFERS_PER_CORE 8
/* Per-core AICore rotation pool depth (1 active + recycled). Larger pool gives
 * the host collector more in-flight buffers before free_queue exhaustion drops
 * records at rotation / flush (see swimlane_converter unmatched warning). */
#define PLATFORM_AICORE_BUFFERS_PER_CORE 16
#define PLATFORM_PROF_BUFFERS_PER_THREAD 16
#define PLATFORM_PROF_READYQUEUE_SIZE \
    (PLATFORM_MAX_CORES * PLATFORM_PROF_BUFFERS_PER_CORE + \
     2 * PLATFORM_MAX_AICPU_THREADS * PLATFORM_PROF_BUFFERS_PER_THREAD + \
     PLATFORM_MAX_CORES * PLATFORM_AICORE_BUFFERS_PER_CORE)
#define PLATFORM_PROF_TIMEOUT_SECONDS 30

#define PROFILING_FLAG_NONE 0u
#define PROFILING_FLAG_DUMP_TENSOR (1u << 0)
#define PROFILING_FLAG_L2_SWIMLANE (1u << 1)
#define PROFILING_FLAG_PMU (1u << 2)
#define PROFILING_FLAG_DEP_GEN (1u << 3)
#define PROFILING_FLAG_SCOPE_STATS (1u << 4)
#define GET_PROFILING_FLAG(flags, bit) ((((uint32_t)(flags)) & ((uint32_t)(bit))) != 0u)
#define SET_PROFILING_FLAG(flags, bit) ((flags) |= (uint32_t)(bit))
#define CLEAR_PROFILING_FLAG(flags, bit) ((flags) &= ~((uint32_t)(bit)))

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
constexpr int PLATFORM_PROF_BUFFER_SIZE_CXX = PLATFORM_PROF_BUFFER_SIZE;
constexpr int PLATFORM_PROF_SLOT_COUNT_CXX = PLATFORM_PROF_SLOT_COUNT;
constexpr int PLATFORM_PROF_BUFFERS_PER_CORE_CXX = PLATFORM_PROF_BUFFERS_PER_CORE;
constexpr int PLATFORM_AICORE_BUFFERS_PER_CORE_CXX = PLATFORM_AICORE_BUFFERS_PER_CORE;
constexpr int PLATFORM_PROF_BUFFERS_PER_THREAD_CXX = PLATFORM_PROF_BUFFERS_PER_THREAD;
constexpr uint32_t PLATFORM_PROF_READYQUEUE_SIZE_CXX = PLATFORM_PROF_READYQUEUE_SIZE;
constexpr int PLATFORM_PROF_TIMEOUT_SECONDS_CXX = PLATFORM_PROF_TIMEOUT_SECONDS;
#endif

#endif /* ESL_PROXY_L2_SWIMLANE_PLATFORM_CONFIG_H */
