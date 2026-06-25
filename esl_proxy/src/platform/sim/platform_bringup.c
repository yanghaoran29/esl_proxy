/*
 * platform_bringup.c — sim host bringup (runtime + bridge + dispatch bind).
 */
#include "platform.h"

#include "conf.h"
#include "dispatch.h"
#include "onboard_config.h"

int platform_bringup(void)
{
    static EslRuntime g_sim_runtime;
    static EslFakeDispatchPayload g_sim_payload[PLATFORM_HOST_WORKER_COUNT * 2U];
    static AicoreBridge g_sim_bridge;

    esl_runtime_setup_host(&g_sim_runtime, g_sim_payload, PLATFORM_HOST_WORKER_COUNT);
    if (aicore_bridge_init(&g_sim_bridge, &g_sim_runtime, 0) != 0) {
        return -1;
    }
    dispatch_bind(&g_sim_bridge);
    return 0;
}

void platform_teardown(void)
{
    /* sim bridge has no persistent teardown beyond process exit */
}

void platform_sched_stats_flush(void)
{
}
