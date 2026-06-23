/*
 * Unified onboard configuration: bring-up knobs, platform constants, time, HAL, payload layout.
 */
#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#define ESL_PROXY_ONBOARD_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- esl_proxy bring-up --- */
#define ESL_PROXY_AICPU_THREAD_NUM 3
#define ESL_PROXY_FAKE_AICORE_COUNT 2
#define ESL_PROXY_ONBOARD_BLOCK_DIM ESL_PROXY_FAKE_AICORE_COUNT

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

static inline double cycles_to_us(uint64_t cycles)
{
    return ((double)cycles / (double)PLATFORM_PROF_SYS_CNT_FREQ) * 1000000.0;
}

#define REG_SPR_DATA_MAIN_BASE_OFFSET 0xA0U
#define REG_SPR_COND_OFFSET 0x4C8U
#define REG_SPR_FAST_PATH_ENABLE_OFFSET 0x18U
#define REG_SPR_FAST_PATH_OPEN 0xEU
#define REG_SPR_FAST_PATH_CLOSE 0xFU
#define AICORE_EXIT_SIGNAL 0x7FFFFFF0U
#define AICORE_COREID_MASK 0x0FFFU

typedef enum {
    REG_ID_DATA_MAIN_BASE = 0,
    REG_ID_COND = 1,
    REG_ID_FAST_PATH_ENABLE = 2,
} RegId;

static inline uint32_t reg_offset(RegId reg)
{
    switch (reg) {
    case REG_ID_DATA_MAIN_BASE:
        return REG_SPR_DATA_MAIN_BASE_OFFSET;
    case REG_ID_COND:
        return REG_SPR_COND_OFFSET;
    case REG_ID_FAST_PATH_ENABLE:
        return REG_SPR_FAST_PATH_ENABLE_OFFSET;
    default:
        return 0U;
    }
}

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

/* --- host-side sys counter (core log / sim) --- */
static inline uint64_t esl_onboard_sys_cnt(void)
{
    uint64_t ticks;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}

static inline uint64_t get_time_ns(void)
{
    return esl_onboard_sys_cnt() * 1000000000ULL / PLATFORM_PROF_SYS_CNT_FREQ;
}

/* --- fake_kernel GM payload --- */
typedef struct EslFakeTaskArgs {
    int64_t duration;
    int64_t mask;
} EslFakeTaskArgs;

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_CONFIG_H */
