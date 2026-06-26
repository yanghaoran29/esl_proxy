/* aicore_bridge.c — sim AICore bridge (fake_aicore FIN / issue path). */
#include "aicore_bridge.h"

#include "conf.h"
#include "dispatch.h"
#include "executor.h"
#include "fake_aicore_host.h"
#include "platform.h"
#include "ring_buf.h"

#include <stdint.h>

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern struct task_desc g_basic_buf[RING_SIZE];

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr)
{
    if (bridge == NULL || runtime == NULL) {
        return -1;
    }
    bridge->runtime = runtime;
    bridge->fake_kernel_addr = fake_kernel_addr;
    bridge->initialized = 1;
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
    HostFakeFin fin;

    (void)bridge;
    (void)dispatch_tid;

    while (host_fake_fin_pop(&fin) == 0) {
        g_ctrl_t[0].msg_bitmap[(int)fin.exe_type][(int)fin.slot] |= fin.mask;
    }
    return 0;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core,
                                int slot, int exe_type, uint32_t block_idx)
{
    uint32_t raw_duration;
    uint32_t jitter_mask;
    uint64_t mask;
    uint16_t phys = 0;

    (void)bridge;
    (void)dispatch_tid;
    (void)block_idx;

    raw_duration = g_basic_buf[task_id & RING_MASK].duration;
    jitter_mask = g_basic_buf[task_id & RING_MASK].jitter_mask;
    mask = (uint64_t)0x1 << (unsigned)core;

    if (platform_issue_block(task_id, exe_type, core, slot, mask, raw_duration, jitter_mask,
                             &phys) != 0) {
        return -1;
    }
    if (platform_fake_kernel_enabled()) {
        g_executors[exe_type][core].block_idx[slot] = phys;
    }
    return 0;
}
