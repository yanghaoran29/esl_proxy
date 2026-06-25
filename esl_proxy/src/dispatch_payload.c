/*
 * dispatch_payload.c — dispatch-time payload assembly (PTO2 build_payload analogue).
 */
#include "dispatch_payload.h"

#include "ring_buf.h"

#include <string.h>

void task_payload_materialize(uint16_t task_id)
{
    const uint16_t slot = (uint16_t)(task_id & RING_MASK);
    struct task_payload *pay = &g_task_payload[slot];
    uint16_t i;

    for (i = 0; i < pay->tensor_cnt; ++i) {
        pay->tensors[i].owner_task_id = task_id;
    }
}

void esl_build_dispatch_payload(EslFakeDispatchPayload *out, const struct task_desc *desc,
                                const struct task_payload *pay, uint32_t block_idx)
{
    uint16_t tc;
    uint16_t sc;
    int i;

    if (out == NULL || desc == NULL || pay == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->task.id = desc->id;
    out->task.type = (uint16_t)desc->type;
    out->task.mode = (uint16_t)desc->mode;
    out->task.tensor_cnt = pay->tensor_cnt;
    out->task.scalar_cnt = pay->scalar_cnt;
    out->task.duration = desc->duration;
    out->task.jitter_mask = desc->jitter_mask;
    out->task.index = block_idx;
    out->task.count = desc->count;
    out->task.kernel = (uint64_t)(uintptr_t)desc->kernel;
    out->duration_ticks = (int64_t)desc->duration;
    out->jitter_mask = (int64_t)desc->jitter_mask;

    tc = pay->tensor_cnt;
    sc = pay->scalar_cnt;
    if (tc > ESL_ONBOARD_MAX_TENSOR_ARGS) {
        tc = ESL_ONBOARD_MAX_TENSOR_ARGS;
    }
    if (sc > ESL_ONBOARD_MAX_SCALAR_ARGS) {
        sc = ESL_ONBOARD_MAX_SCALAR_ARGS;
    }

    for (i = 0; i < (int)tc; ++i) {
        out->tensors[i] = pay->tensors[i];
        out->args[i] = (uint64_t)(uintptr_t)&out->tensors[i];
    }
    for (i = 0; i < (int)sc; ++i) {
        out->args[tc + (uint16_t)i] = (uint64_t)pay->scalars[i];
    }
}
