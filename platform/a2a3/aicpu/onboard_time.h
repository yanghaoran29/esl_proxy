#ifndef ESL_ONBOARD_TIME_H
#define ESL_ONBOARD_TIME_H

#include <stdint.h>

/* Matches a2a3 PLATFORM_PROF_SYS_CNT_FREQ (50 MHz). */
#define ESL_ONBOARD_SYS_CNT_FREQ 50000000ULL

static inline uint64_t esl_onboard_sys_cnt(void)
{
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}

static inline uint64_t get_time_ns(void)
{
    return esl_onboard_sys_cnt() * 1000000000ULL / ESL_ONBOARD_SYS_CNT_FREQ;
}

#endif /* ESL_ONBOARD_TIME_H */
