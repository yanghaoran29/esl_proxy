/*
 * platform_config.h — platform configuration shared by BOTH the onboard and sim
 * backends: dimensions, profiling flags, register-layout constants, RegId, task-id
 * encoding, dispatch payload layout, and small inline helpers.
 *
 * Single source of truth. Onboard-only knobs (CANN host timeouts) live in
 * include/platform/onboard/onboard_config.h, which #includes this header.
 */
#ifndef ESL_PROXY_PLATFORM_CONFIG_H
#define ESL_PROXY_PLATFORM_CONFIG_H

#include <stdint.h>

#include "worker_map.h"

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#define PLATFORM_MAX_BLOCKDIM ESL_PROXY_WORKER_BLOCK_DIM
#define PLATFORM_CORES_PER_BLOCKDIM 3
#define PLATFORM_AIC_CORES_PER_BLOCKDIM 1
#define PLATFORM_AIV_CORES_PER_BLOCKDIM ESL_PROXY_AIV_LANES_PER_BLOCK
#define PLATFORM_MAX_AICPU_THREADS 4
#define PLATFORM_MAX_AICPU_THREADS_JUST_FOR_LAUNCH 6

#define PLATFORM_MAX_AIC_PER_THREAD (PLATFORM_MAX_BLOCKDIM * PLATFORM_AIC_CORES_PER_BLOCKDIM)
#define PLATFORM_MAX_AIV_PER_THREAD (PLATFORM_MAX_BLOCKDIM * PLATFORM_AIV_CORES_PER_BLOCKDIM)
#define PLATFORM_MAX_CORES_PER_THREAD (PLATFORM_MAX_AIC_PER_THREAD + PLATFORM_MAX_AIV_PER_THREAD)
#define PLATFORM_MAX_CORES (PLATFORM_MAX_BLOCKDIM * PLATFORM_CORES_PER_BLOCKDIM)

#define PLATFORM_PROF_BUFFER_SIZE 1000
#define PLATFORM_PROF_SLOT_COUNT 4
#define PLATFORM_PROF_BUFFERS_PER_CORE 8
#define PLATFORM_AICORE_BUFFERS_PER_CORE 16
#define PLATFORM_PROF_BUFFERS_PER_THREAD 16
#define PLATFORM_PROF_READYQUEUE_SIZE \
    (PLATFORM_MAX_CORES * PLATFORM_PROF_BUFFERS_PER_CORE + \
     2 * PLATFORM_MAX_AICPU_THREADS * PLATFORM_PROF_BUFFERS_PER_THREAD + \
     PLATFORM_MAX_CORES * PLATFORM_AICORE_BUFFERS_PER_CORE)
#define PLATFORM_PROF_TIMEOUT_SECONDS 30
#define PLATFORM_PROF_SYS_CNT_FREQ 50000000ULL
#define ESL_ONBOARD_SYS_CNT_FREQ PLATFORM_PROF_SYS_CNT_FREQ

#define PROFILING_FLAG_NONE 0u
#define PROFILING_FLAG_DUMP_TENSOR (1u << 0)
#define PROFILING_FLAG_L2_SWIMLANE (1u << 1)
#define PROFILING_FLAG_PMU (1u << 2)
#define PROFILING_FLAG_DEP_GEN (1u << 3)
#define PROFILING_FLAG_SCOPE_STATS (1u << 4)
#define GET_PROFILING_FLAG(flags, bit) ((((uint32_t)(flags)) & ((uint32_t)(bit))) != 0u)
#define SET_PROFILING_FLAG(flags, bit) ((flags) |= (uint32_t)(bit))
#define CLEAR_PROFILING_FLAG(flags, bit) ((flags) &= ~((uint32_t)(bit)))

#ifndef ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
#include "tensor.h"
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

#ifdef __cplusplus
extern "C" {
#endif

/* --- esl_proxy bring-up --- */
#define ESL_PROXY_AICPU_THREAD_NUM 3
#define ESL_AICPU_ROLE_CUTTER 0
#define ESL_AICPU_ROLE_DISPATCH 1
#define ESL_AICPU_ROLE_ORCH (ESL_PROXY_AICPU_THREAD_NUM - 1)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(ESL_PROXY_AICPU_THREAD_NUM == 3, "three-thread cutter/dispatch/orch model");
#endif
#define ESL_PROXY_ONBOARD_BLOCK_DIM ESL_PROXY_WORKER_BLOCK_DIM
#define ESL_PROXY_ONBOARD_AIC_COUNT ESL_PROXY_ONBOARD_BLOCK_DIM
#define ESL_PROXY_ONBOARD_AIV_COUNT (ESL_PROXY_ONBOARD_BLOCK_DIM * PLATFORM_AIV_CORES_PER_BLOCKDIM)
#define ESL_PROXY_ONBOARD_WORKER_COUNT (ESL_PROXY_ONBOARD_AIC_COUNT + ESL_PROXY_ONBOARD_AIV_COUNT)
#define ESL_PROXY_FAKE_AICORE_COUNT ESL_PROXY_ONBOARD_WORKER_COUNT

#define REG_SPR_DATA_MAIN_BASE_OFFSET 0xA0U
#define REG_SPR_COND_OFFSET 0x4C8U
#define REG_SPR_FAST_PATH_ENABLE_OFFSET 0x18U
#define REG_SPR_FAST_PATH_OPEN 0xEU
#define REG_SPR_FAST_PATH_CLOSE 0xFU
#define AICORE_EXIT_SIGNAL 0x7FFFFFF0U
#define AICORE_COREID_MASK 0x0FFFU

#define SIM_REG_BLOCK_SIZE 0x500U
#define PLATFORM_SUB_CORES_PER_AICORE PLATFORM_CORES_PER_BLOCKDIM
#define DAV_2201_PLATFORM_MAX_PHYSICAL_CORES 25U
#define ESL_PROXY_PLATFORM_HAL_REG_SLOTS \
    (DAV_2201_PLATFORM_MAX_PHYSICAL_CORES * PLATFORM_SUB_CORES_PER_AICORE)

#define TASK_ID_MASK 0x7FFFFFFFU
#define TASK_STATE_MASK 0x80000000U
#define TASK_ACK_STATE 0U
#define TASK_FIN_STATE 1U

#define EXTRACT_TASK_ID(regval) ((int)((regval) & TASK_ID_MASK))
#define EXTRACT_TASK_STATE(regval) ((int)(((regval) & TASK_STATE_MASK) >> 31))
#define MAKE_ACK_VALUE(task_id) ((uint64_t)((task_id) & TASK_ID_MASK))
#define MAKE_FIN_VALUE(task_id) ((uint64_t)(((task_id) & TASK_ID_MASK) | TASK_STATE_MASK))

#define AICORE_IDLE_TASK_ID 0x7FFFFFFFU
#define AICORE_EXIT_TASK_ID 0x7FFFFFFEU
#define AICPU_IDLE_TASK_ID 0x7FFFFFFDU
#define AICORE_IDLE_VALUE MAKE_FIN_VALUE(AICORE_IDLE_TASK_ID)
#define AICORE_EXITED_VALUE MAKE_FIN_VALUE(AICORE_EXIT_TASK_ID)

#define AICPU_TASK_INVALID (-1)

#define PLATFORM_AICORE_MAP_BUFF_LEN 2U

#define ESL_ONBOARD_MAX_TENSOR_ARGS 16
#define ESL_ONBOARD_MAX_SCALAR_ARGS 32
#define ESL_ONBOARD_MAX_KERNEL_ARGS (ESL_ONBOARD_MAX_TENSOR_ARGS + ESL_ONBOARD_MAX_SCALAR_ARGS)

typedef enum {
    REG_ID_DATA_MAIN_BASE = 0,
    REG_ID_COND = 1,
    REG_ID_FAST_PATH_ENABLE = 2,
} RegId;

typedef struct EslOnboardTaskDesc {
    uint16_t id;
    uint16_t type;
    uint16_t mode;
    uint16_t tensor_cnt;
    uint16_t scalar_cnt;
    uint32_t duration;
    uint32_t jitter_mask;
    uint32_t index;
    uint32_t count;
    uint64_t kernel;
} EslOnboardTaskDesc;

#ifndef ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD
typedef struct EslFakeDispatchPayload {
    EslOnboardTaskDesc task;
    int64_t duration_ticks;
    int64_t jitter_mask;
    uint64_t args[ESL_ONBOARD_MAX_KERNEL_ARGS];
    Tensor tensors[ESL_ONBOARD_MAX_TENSOR_ARGS];
} __attribute__((aligned(64))) EslFakeDispatchPayload;
#endif /* !ESL_PROXY_ONBOARD_CONFIG_NO_PAYLOAD */

static inline uint64_t esl_duration_ns_to_sys_cnt(uint64_t duration_ns)
{
    return duration_ns * ESL_ONBOARD_SYS_CNT_FREQ / 1000000000ULL;
}

static inline int esl_worker_to_hal_reg_index(int worker_id)
{
    if (worker_id < 0 || worker_id >= ESL_PROXY_ONBOARD_WORKER_COUNT) {
        return -1;
    }
    if (worker_id < ESL_PROXY_ONBOARD_AIC_COUNT) {
        return worker_id;
    }
    return worker_id + 1;
}

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_CONFIG_H */
