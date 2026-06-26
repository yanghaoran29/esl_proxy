/*
 * platform_sync_onboard.c — onboard cache flush/invalidate for shared scheduler state.
 */
#include "platform.h"

#include "conf.h"
#include "onboard_config.h"
#include "worker_map.h"

#include <stdarg.h>

int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

void platform_main_log_vwrite(int line, const char *fmt, va_list args)
{
    (void)line;
    (void)fmt;
    (void)args;
}
