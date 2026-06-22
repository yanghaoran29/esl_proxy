#ifndef ESL_PROXY_PLATFORM_INIT_H
#define ESL_PROXY_PLATFORM_INIT_H

#include "aicore_bridge.h"
#include "esl_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge);
void esl_platform_shutdown(AicoreBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_INIT_H */
