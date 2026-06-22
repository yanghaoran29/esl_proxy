#ifndef ESL_PROXY_AICORE_BRIDGE_H
#define ESL_PROXY_AICORE_BRIDGE_H

#include <stdint.h>

#include "esl_runtime.h"

typedef struct AicoreBridge {
    EslRuntime *runtime;
    uint64_t fake_kernel_addr;
    int initialized;
} AicoreBridge;

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr);
void aicore_bridge_shutdown(AicoreBridge *bridge);
int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid);
int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id,
                                int core, int slot, int exe_type);

#endif /* ESL_PROXY_AICORE_BRIDGE_H */
