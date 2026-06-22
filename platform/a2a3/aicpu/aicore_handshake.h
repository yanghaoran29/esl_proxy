#ifndef ESL_AICORE_HANDSHAKE_H
#define ESL_AICORE_HANDSHAKE_H

#include "esl_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal AICPU↔AICore register handshake (M1). */
int esl_handshake_all_cores(EslRuntime *runtime);
void esl_shutdown_all_cores(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_AICORE_HANDSHAKE_H */
