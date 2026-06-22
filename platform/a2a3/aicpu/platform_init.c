/*
 * C-only onboard init — keeps stdatomic headers out of executor.cpp (C++).
 */
#include "aicore_bridge.h"
#include "cutter.h"
#include "dispatch.h"
#include "esl_runtime.h"
#include "fake_aicore_regs.h"
#include "ring_buf.h"

void init_predecessors(void);

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge)
{
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();

    if (esl_fake_aicore_regs_init() != 0) {
        return -1;
    }
    if (runtime != NULL) {
        runtime->worker_count = (int)esl_fake_aicore_core_count();
    }

    uint64_t fake_addr = 0;
    if (runtime != NULL) {
        fake_addr = runtime->func_id_to_addr_[0];
    }
    if (aicore_bridge_init(bridge, runtime, fake_addr) != 0) {
        return -1;
    }
    dispatch_set_aicore_bridge(bridge);
    return 0;
}

void esl_platform_shutdown(AicoreBridge *bridge)
{
    aicore_bridge_shutdown(bridge);
    esl_fake_aicore_regs_shutdown();
}
