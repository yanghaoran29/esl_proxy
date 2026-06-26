/* dispatch_payload.c — dispatch payload marshalling (algorithm layer, backend-neutral). */
#define _GNU_SOURCE

#include "handshake.h"

#include "platform_config.h"
#include "platform_regs.h"
#include "task.h"
#include "tensor.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void esl_build_dispatch_payload(EslFakeDispatchPayload *out, const struct task_desc *desc, uint32_t block_idx)
{
    uint16_t tc;
    uint16_t sc;
    int i;

    if (out == NULL || desc == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->task.id = desc->id;
    out->task.type = (uint16_t)desc->type;
    out->task.mode = (uint16_t)desc->mode;
    out->task.tensor_cnt = desc->tensor_cnt;
    out->task.scalar_cnt = desc->scalar_cnt;
    out->task.duration = desc->duration;
    out->task.jitter_mask = desc->jitter_mask;
    out->task.index = block_idx;
    out->task.count = desc->count;
    out->task.kernel = (uint64_t)(uintptr_t)desc->kernel;
    out->duration_ticks = (int64_t)desc->duration;
    out->jitter_mask = (int64_t)desc->jitter_mask;

    tc = desc->tensor_cnt;
    sc = desc->scalar_cnt;
    if (tc > ESL_ONBOARD_MAX_TENSOR_ARGS) {
        tc = ESL_ONBOARD_MAX_TENSOR_ARGS;
    }
    if (sc > ESL_ONBOARD_MAX_SCALAR_ARGS) {
        sc = ESL_ONBOARD_MAX_SCALAR_ARGS;
    }

    for (i = 0; i < (int)tc; ++i) {
        out->tensors[i].buffer_addr = desc->data[i];
        out->args[i] = (uint64_t)(uintptr_t)&out->tensors[i];
    }
    for (i = 0; i < (int)sc; ++i) {
        out->args[tc + (uint16_t)i] = (uint64_t)desc->scalar[i];
    }
}

void esl_dispatch_payload_prepare(EslRuntime *runtime, int core, uint32_t reg_task_id,
                                  const EslOnboardDispatchInput *input)
{
    EslFakeDispatchPayload *p;
    uint64_t base;
    int slot;
    struct task_desc desc;

    if (runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER || input == NULL) {
        return;
    }
    base = runtime->workers[core].task;
    if (base == 0) {
        return;
    }
    slot = (int)(reg_task_id & 1u);
    p = (EslFakeDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslFakeDispatchPayload));

    memset(&desc, 0, sizeof(desc));
    desc.id = input->task.id;
    desc.type = (task_type_t)input->task.type;
    desc.mode = (org_mode_t)input->task.mode;
    desc.kernel = (void *)(uintptr_t)input->task.kernel;
    desc.index = input->task.index;
    desc.count = input->task.count;
    desc.duration = input->task.duration;
    desc.jitter_mask = input->task.jitter_mask;

    desc.tensor_cnt = input->task.tensor_cnt;
    desc.scalar_cnt = input->task.scalar_cnt;
    if (input->data != NULL) {
        uint16_t tc = desc.tensor_cnt;

        if (tc > TASK_MAX_TENSORS) {
            tc = TASK_MAX_TENSORS;
        }
        memcpy(desc.data, input->data, (size_t)tc * sizeof(uint64_t));
    }
    if (input->scalars != NULL && desc.scalar_cnt > 0) {
        uint16_t sc = desc.scalar_cnt;

        if (sc > TASK_MAX_SCALARS) {
            sc = TASK_MAX_SCALARS;
        }
        memcpy(desc.scalar, input->scalars, (size_t)sc * sizeof(int64_t));
    }

    esl_build_dispatch_payload(p, &desc, input->task.index);
    cache_flush_range(p, sizeof(EslFakeDispatchPayload));
}
