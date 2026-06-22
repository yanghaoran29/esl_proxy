#ifndef ESL_PROXY_AICPU_LOADER_H
#define ESL_PROXY_AICPU_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "esl_kernel_args.h"
#include "runtime/rt.h"
#include "runtime/runtime/rts/rts_kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESL_AICPU_INIT_NAME "simpler_aicpu_init"
#define ESL_AICPU_EXEC_NAME "simpler_aicpu_exec"

typedef struct EslAicpuLoader EslAicpuLoader;

EslAicpuLoader *esl_aicpu_loader_create(void);
void esl_aicpu_loader_destroy(EslAicpuLoader *loader);

int esl_aicpu_loader_bootstrap(
    EslAicpuLoader *loader, const void *dispatcher_so, size_t dispatcher_len, const void *inner_so, size_t inner_len,
    rtStream_t stream, int device_id);

int esl_aicpu_loader_init(EslAicpuLoader *loader);

int esl_aicpu_loader_launch(
    EslAicpuLoader *loader, rtStream_t stream, struct EslKernelArgs *k_args, int aicpu_num, const char *symbol_name);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_LOADER_H */
