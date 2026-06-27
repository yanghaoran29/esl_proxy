/* dispatch_payload.c — PTO2 dispatch payload prepare/publish (algorithm layer). */
#define _GNU_SOURCE

#include "dispatch_payload.h"

#include "platform_config.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern struct task_desc g_basic_buf[RING_SIZE];

#define ESL_FAKE_KERNEL_FUNC_ID_AIC 0U
#define ESL_FAKE_KERNEL_FUNC_ID_AIV 1U

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];

static uint32_t dispatch_next_reg_task_id(int phys)
{
    uint32_t seq;
    uint32_t reg_id;

    if (phys < 0 || phys >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    seq = ++g_core_dispatch_seq[phys];
    reg_id = seq & (uint32_t)TASK_ID_MASK;
    if (reg_id >= (uint32_t)AICORE_EXIT_SIGNAL) {
        g_core_dispatch_seq[phys] = seq + ((uint32_t)AICORE_EXIT_SIGNAL - reg_id);
        reg_id = (uint32_t)(g_core_dispatch_seq[phys] & (uint32_t)TASK_ID_MASK);
    }
    return reg_id;
}

static void build_payload(EslDispatchPayload *out, const struct task_desc *desc, uint32_t block_idx,
                          uint64_t fake_kernel_addr)
{
    if (out == NULL || desc == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->function_bin_addr = fake_kernel_addr;
    out->args[0] = (uint64_t)desc->duration;
    out->args[1] = (uint64_t)desc->jitter_mask;
    out->local_context.block_idx = (int32_t)block_idx;
    out->local_context.block_num = (int32_t)desc->count;
    out->local_context.async_ctx.task_token = ESL_PTO2_TASK_ID_INVALID;
    out->args[ESL_PTO2_LOCAL_CONTEXT_INDEX] = (uint64_t)(uintptr_t)&out->local_context;
    out->args[ESL_PTO2_GLOBAL_CONTEXT_INDEX] = (uint64_t)(uintptr_t)&out->global_context;
    out->not_ready = 0U;
}

void esl_init_global_context(EslRuntime *runtime)
{
    int i;
    int slot;

    if (runtime == NULL) {
        return;
    }

    for (i = 0; i < runtime->worker_count && i < RUNTIME_MAX_WORKER; ++i) {
        uint64_t base = runtime->workers[i].task;
        if (base == 0) {
            continue;
        }
        for (slot = 0; slot < 2; ++slot) {
            EslDispatchPayload *p =
                (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
            if (runtime->workers[i].core_type != 0) {
                int aiv_idx = i - ESL_PROXY_ONBOARD_BLOCK_DIM;
                p->global_context.sub_block_id = (aiv_idx >= 0) ? (aiv_idx % ESL_PROXY_AIV_LANES_PER_BLOCK) : 0;
            } else {
                p->global_context.sub_block_id = 0;
            }
        }
    }
}

EslPublishHandle esl_prepare_subtask_to_core(EslRuntime *runtime, int core, uint16_t task_id, uint32_t block_idx)
{
    EslPublishHandle handle = {0, 0};
    EslDispatchPayload *p;
    uint64_t base;
    uint32_t reg_task_id;
    int slot;
    const struct task_desc *desc;
    uint64_t fake_kernel_addr;

    if (runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER) {
        return handle;
    }

    base = runtime->workers[core].task;
    if (base == 0) {
        return handle;
    }

    reg_task_id = dispatch_next_reg_task_id(core);
    slot = (int)(reg_task_id & 1u);
    p = (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
    desc = &g_basic_buf[task_id & RING_MASK];
    fake_kernel_addr = runtime->func_id_to_addr_[runtime->workers[core].core_type == 0
                                                      ? ESL_FAKE_KERNEL_FUNC_ID_AIC
                                                      : ESL_FAKE_KERNEL_FUNC_ID_AIV];
    build_payload(p, desc, block_idx, fake_kernel_addr);
    handle.reg_task_id = reg_task_id;
    return handle;
}

void esl_publish_subtask_to_core(EslPublishHandle handle)
{
    if (handle.reg_addr == 0U || handle.reg_task_id == 0U) {
        return;
    }
    write_reg(handle.reg_addr, REG_ID_DATA_MAIN_BASE, handle.reg_task_id);
}
