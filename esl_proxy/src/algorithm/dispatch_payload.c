/* dispatch_payload.c — dispatch payload marshalling (algorithm layer, backend-neutral). */
#define _GNU_SOURCE

#include "handshake.h"

#include "platform_config.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "task.h"
#include "tensor.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern struct task_desc g_basic_buf[RING_SIZE];

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
                                  uint16_t task_id, uint32_t block_idx)
{
    EslFakeDispatchPayload *p;
    uint64_t base;
    int slot;
    const struct task_desc *desc;

    if (runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER) {
        return;
    }
    base = runtime->workers[core].task;
    if (base == 0) {
        return;
    }
    slot = (int)(reg_task_id & 1u);
    p = (EslFakeDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslFakeDispatchPayload));
    desc = &g_basic_buf[task_id & RING_MASK];

    esl_build_dispatch_payload(p, desc, block_idx);
    cache_flush_range(p, sizeof(EslFakeDispatchPayload));
}
