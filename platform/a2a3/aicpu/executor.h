#ifndef ESL_PROXY_AICPU_EXECUTOR_H
#define ESL_PROXY_AICPU_EXECUTOR_H

#include <stdint.h>

#include "esl_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t esl_aicpu_execute(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_EXECUTOR_H */
