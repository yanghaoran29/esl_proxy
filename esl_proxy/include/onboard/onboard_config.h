/*
 * Unified onboard configuration: bring-up knobs, platform constants, time, HAL, payload layout.
 */
#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#define ESL_PROXY_ONBOARD_CONFIG_H

#include <stdint.h>

#include "onboard_tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- esl_proxy bring-up --- */
#define ESL_PROXY_AICPU_THREAD_NUM 3
/* block_dim=24 → 24 AIC + 48 AIV = 72 workers (1+2 per blockdim) */
#define ESL_PROXY_ONBOARD_BLOCK_DIM 24
#define ESL_PROXY_ONBOARD_AIC_COUNT ESL_PROXY_ONBOARD_BLOCK_DIM
#define ESL_PROXY_ONBOARD_AIV_COUNT (ESL_PROXY_ONBOARD_BLOCK_DIM * PLATFORM_AIV_CORES_PER_BLOCKDIM)
#define ESL_PROXY_ONBOARD_WORKER_COUNT (ESL_PROXY_ONBOARD_AIC_COUNT + ESL_PROXY_ONBOARD_AIV_COUNT)
#define ESL_PROXY_FAKE_AICORE_COUNT ESL_PROXY_ONBOARD_WORKER_COUNT

/* --- platform architectural (AICPU / AICore / Host) --- */
#define PLATFORM_MAX_BLOCKDIM 24
#define PLATFORM_CORES_PER_BLOCKDIM 3
#define PLATFORM_AIC_CORES_PER_BLOCKDIM 1
#define PLATFORM_AIV_CORES_PER_BLOCKDIM 2
#define PLATFORM_MAX_AICPU_THREADS 4
#define PLATFORM_MAX_AICPU_THREADS_JUST_FOR_LAUNCH 6
#define PLATFORM_OP_EXECUTE_TIMEOUT_US 3000000ULL
#define PLATFORM_STREAM_SYNC_TIMEOUT_MS 4000

#define PLATFORM_MAX_AIC_PER_THREAD (PLATFORM_MAX_BLOCKDIM * PLATFORM_AIC_CORES_PER_BLOCKDIM)
#define PLATFORM_MAX_AIV_PER_THREAD (PLATFORM_MAX_BLOCKDIM * PLATFORM_AIV_CORES_PER_BLOCKDIM)
#define PLATFORM_MAX_CORES_PER_THREAD (PLATFORM_MAX_AIC_PER_THREAD + PLATFORM_MAX_AIV_PER_THREAD)
#define PLATFORM_MAX_CORES (PLATFORM_MAX_BLOCKDIM * PLATFORM_CORES_PER_BLOCKDIM)

#define PLATFORM_PROF_SYS_CNT_FREQ 50000000ULL
#define ESL_ONBOARD_SYS_CNT_FREQ PLATFORM_PROF_SYS_CNT_FREQ

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

/* --- host HAL register query --- */
#define PLATFORM_AICORE_MAP_BUFF_LEN 2U

/* --- fake_kernel GM payload (dual-buffer per core, task_id & 1) --- */
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
    uint16_t duration;
    uint16_t _pad;
    uint32_t index;
    uint32_t count;
    uint64_t kernel;
} EslOnboardTaskDesc;

typedef struct EslOnboardDispatchInput {
    EslOnboardTaskDesc task;
    uint64_t tensor_addrs[ESL_ONBOARD_MAX_TENSOR_ARGS];
    int64_t scalars[ESL_ONBOARD_MAX_SCALAR_ARGS];
} EslOnboardDispatchInput;

typedef struct EslFakeDispatchPayload {
    EslOnboardTaskDesc task;
    int64_t duration_ticks;
    int64_t task_id_mask;
    uint64_t args[ESL_ONBOARD_MAX_KERNEL_ARGS];
    EslOnboardTensor tensors[ESL_ONBOARD_MAX_TENSOR_ARGS];
} __attribute__((aligned(64))) EslFakeDispatchPayload;

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_CONFIG_H */
