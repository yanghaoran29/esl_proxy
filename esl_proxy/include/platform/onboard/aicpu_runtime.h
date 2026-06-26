/*
 * Onboard AICPU functional layer API (orchestration + platform init hooks).
 */
#ifndef ESL_PROXY_AICPU_RUNTIME_H
#define ESL_PROXY_AICPU_RUNTIME_H

#include <stdint.h>

/* Forward declarations only — keep this platform header from including the
 * algorithm-layer runtime.h / aicore_bridge.h (one-way layering). Implementation
 * .c files include the full algorithm headers. */
typedef struct EslRuntime EslRuntime;
typedef struct AicoreBridge AicoreBridge;

#ifdef __cplusplus
extern "C" {
#endif

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge);
void esl_platform_shutdown(AicoreBridge *bridge);
int32_t esl_aicpu_execute(EslRuntime *runtime);
void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
                     uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_RUNTIME_H */
