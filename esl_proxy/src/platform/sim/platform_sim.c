/*
 * platform_sim.c — host CPU sim backend for platform.h
 */
#include "platform.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "worker_map.h"

int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

uint64_t get_time_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void platform_main_log_vwrite(int line, const char *fmt, va_list args)
{
    fprintf(stdout, "[main:%d] ", line);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
}
