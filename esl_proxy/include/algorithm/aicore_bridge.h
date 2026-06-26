/*
 * AICore bridge protocol: dispatch payload, completion poll (algorithm layer).
 *
 * Backend-neutral orchestration surface; implementation in src/algorithm/aicore_bridge.c
 * uses platform HAL (read_reg / write_reg / cache_*).
 */
#ifndef ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H
#define ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "conf.h"
#include "runtime.h"
#include "platform_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void esl_runtime_setup_host(EslRuntime *rt, EslFakeDispatchPayload *payload, int worker_count);

void aicore_bridge_shutdown(EslRuntime *runtime);
int aicore_bridge_poll_completions(EslRuntime *runtime, int dispatch_tid);
int aicore_bridge_dispatch_task(EslRuntime *runtime, int dispatch_tid, uint16_t task_id,
                                int core, int slot, int exe_type, uint32_t block_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_AICORE_BRIDGE_H */
