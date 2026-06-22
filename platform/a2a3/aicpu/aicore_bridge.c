/*
 * AICore bridge — onboard dispatch/completion for esl_proxy.
 * Uses in-memory simulated register blocks; tasks complete instantly.
 */

#include "aicore_bridge.h"
#include "fake_aicore_regs.h"

#include <stdint.h>

#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "ring_buf.h"

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr)
{
    if (bridge == NULL || runtime == NULL) {
        return -1;
    }
    bridge->runtime = runtime;
    bridge->fake_kernel_addr = fake_kernel_addr;
    bridge->initialized = 1;
    (void)fake_kernel_addr;
    return 0;
}

void aicore_bridge_shutdown(AicoreBridge *bridge)
{
    if (bridge != NULL && bridge->initialized) {
        bridge->initialized = 0;
    }
}

int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid)
{
    (void)bridge;
    (void)dispatch_tid;
    return 0;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id,
                                int core, int slot, int exe_type)
{
    (void)bridge;
    (void)dispatch_tid;

    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;

    esl_fake_aicore_dispatch(core, (uint32_t)task_id);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= ((uint64_t)1 << core);
    return 0;
}
