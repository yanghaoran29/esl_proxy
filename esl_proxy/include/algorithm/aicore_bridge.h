/*
 * AICore bridge protocol: handshake, dispatch payload, completion poll (algorithm layer).
 *
 * Backend-neutral orchestration surface, mirroring simpler's runtime/scheduler:
 * the per-backend definitions live in src/platform/{onboard,sim}/aicore_bridge.c,
 * which implement these neutral declarations on top of the platform HAL. The
 * onboard cache (dcci) hooks are NOT here — they are platform-specific and live
 * in include/platform/onboard/onboard_cache_hooks.h (one-way layering).
 */
#ifndef ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H
#define ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "conf.h"
#include "runtime.h"
#include "platform_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AicoreBridge {
    EslRuntime *runtime;
    uint64_t fake_kernel_addr;
    int initialized;
} AicoreBridge;

void esl_runtime_setup_host(EslRuntime *rt, EslFakeDispatchPayload *payload, int worker_count);

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr);
void aicore_bridge_shutdown(AicoreBridge *bridge);
int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid);
int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id,
                                int core, int slot, int exe_type, uint32_t block_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H */
