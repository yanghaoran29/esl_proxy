/* aicpu_platform.c — onboard AICPU platform primitives.
 *
 * Pure NPU bring-up + AICore handshake: register access, cache ops, HW
 * dispatch primitives. Onboard-only — this file must
 * not include any non-onboard (functional) header.
 */
#define _GNU_SOURCE

#include "onboard_config.h"
#include "tools.h"
#include "onboard_log.h"
#include "aicpu_bridge.h"
#include "kernel_args.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __aarch64__
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("dmb ishst" ::: "memory")
#else
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

#define HANDSHAKE_SPIN_MAX 50000000ULL
#define DEINIT_ACK_SPIN_MAX 5000000ULL

static uint64_t g_platform_regs;
static EslRuntime *g_runtime;
static uint64_t g_core_reg_addrs[RUNTIME_MAX_WORKER];

/* ========================================================================== */
/* Platform registers & cache (aicpu_bridge.h)                                */
/* ========================================================================== */

void set_platform_regs(uint64_t regs)
{
    g_platform_regs = regs;
}

uint64_t get_platform_regs(void)
{
    return g_platform_regs;
}

volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg)
{
    return (volatile uint32_t *)(uintptr_t)(reg_base_addr + reg_offset(reg));
}

uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    return (uint64_t)*get_reg_ptr(reg_base_addr, reg);
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    *get_reg_ptr(reg_base_addr, reg) = (uint32_t)value;
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    write_reg(reg_addr, REG_ID_FAST_PATH_ENABLE, REG_SPR_FAST_PATH_OPEN);
    write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
}

void cache_invalidate_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

void cache_flush_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

/* ========================================================================== */
/* HW dispatch & AICore handshake                                             */
/* ========================================================================== */

int esl_hw_poll_fin(uint64_t reg_addr, uint32_t reg_task_id)
{
    uint64_t cond;

    if (reg_addr == 0) {
        return 0;
    }
    cond = read_reg(reg_addr, REG_ID_COND);
#ifdef __aarch64__
    __asm__ __volatile__("dmb ishld" ::: "memory");
#endif
    return (EXTRACT_TASK_STATE(cond) == TASK_FIN_STATE && EXTRACT_TASK_ID(cond) == (int)reg_task_id) ? 1 : 0;
}

void esl_hw_dispatch_reg(uint64_t reg_addr, uint32_t reg_task_id)
{
    if (reg_addr != 0) {
        write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, reg_task_id);
    }
}

void esl_dispatch_payload_init(EslRuntime *runtime)
{
    g_runtime = runtime;
}

void esl_dispatch_payload_prepare(int core, uint32_t reg_task_id, const EslOnboardDispatchInput *input)
{
    EslFakeDispatchPayload *p;
    uint64_t base;
    int slot;
    uint16_t tc;
    uint16_t sc;
    int i;

    if (g_runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER || input == NULL) {
        return;
    }
    base = g_runtime->workers[core].task;
    if (base == 0) {
        return;
    }
    /* Must match AICore: exec_payload = payload_base + (reg_task_id & 1). */
    slot = (int)(reg_task_id & 1u);
    p = (EslFakeDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslFakeDispatchPayload));

    memset(p, 0, sizeof(*p));
    p->task = input->task;
    /* duration_ticks / jitter_mask are both in ns; AICore converts to SYS_CNT. */
    p->duration_ticks = (int64_t)input->task.duration;
    p->jitter_mask = (int64_t)input->task.jitter_mask;

    tc = input->task.tensor_cnt;
    sc = input->task.scalar_cnt;
    if (tc > ESL_ONBOARD_MAX_TENSOR_ARGS) {
        tc = ESL_ONBOARD_MAX_TENSOR_ARGS;
    }
    if (sc > ESL_ONBOARD_MAX_SCALAR_ARGS) {
        sc = ESL_ONBOARD_MAX_SCALAR_ARGS;
    }

    for (i = 0; i < (int)tc; ++i) {
        p->tensors[i].buffer_addr = input->tensor_addrs[i];
        p->args[i] = (uint64_t)(uintptr_t)&p->tensors[i];
    }
    for (i = 0; i < (int)sc; ++i) {
        p->args[tc + (uint16_t)i] = (uint64_t)input->scalars[i];
    }

    cache_flush_range(p, sizeof(EslFakeDispatchPayload));
}

static int wait_handshake_field(volatile uint32_t *field, uint32_t expect)
{
    uint64_t spins;

    for (spins = 0; spins < HANDSHAKE_SPIN_MAX; ++spins) {
        cache_invalidate_range((const void *)field, sizeof(*field));
        if (*field == expect) {
            return 1;
        }
    }
    return 0;
}

int esl_handshake_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_handshake;
    int i;

    if (runtime == NULL) {
        return -1;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", n);
        return -1;
    }
    regs_table = get_platform_regs();
    if (regs_table == 0) {
        LOG_ERROR("Platform regs table not set");
        return -1;
    }
    regs = (uint64_t *)(uintptr_t)regs_table;
    n_handshake = n;
    LOG_INFO_V0("esl_proxy handshake for %d workers", n_handshake);
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];

        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_ready = 1;
        cache_flush_range((const void *)wk, sizeof(EslHandshake));
    }
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];
        uint32_t phys;
        uint64_t reg_addr;

        if (!wait_handshake_field(&wk->aicore_regs_ready, 1)) {
            LOG_ERROR("Core %d aicore_regs_ready timeout", i);
            return -1;
        }
        cache_invalidate_range((const void *)&wk->physical_core_id, sizeof(wk->physical_core_id));
        phys = wk->physical_core_id;
        if (phys >= ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
            LOG_ERROR(
                "Worker %d invalid physical_core_id=%u (HAL table size=%u)", i, phys,
                (unsigned)ESL_PROXY_PLATFORM_HAL_REG_SLOTS);
            return -1;
        }
        reg_addr = regs[phys];
        if (reg_addr == 0) {
            LOG_ERROR("Worker %d physical_core_id=%u has zero register address", i, phys);
            return -1;
        }
        g_core_reg_addrs[i] = reg_addr;
        platform_init_aicore_regs(reg_addr);
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_regs_ready = 1;
        cache_flush_range((const void *)&wk->aicpu_regs_ready, sizeof(wk->aicpu_regs_ready));
        if (!wait_handshake_field((volatile uint32_t *)&wk->aicore_done, (uint32_t)(i + 1))) {
            LOG_ERROR("Core %d aicore_done timeout", i);
            return -1;
        }
    }
    return 0;
}

uint64_t esl_handshake_reg_addr(int core_idx)
{
    if (core_idx < 0 || core_idx >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    return g_core_reg_addrs[core_idx];
}

void esl_shutdown_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_shutdown;
    int i;

    if (runtime == NULL) {
        return;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        return;
    }
    n_shutdown = n;
    LOG_INFO_V0("esl_proxy shutting down %d AICore workers", n_shutdown);
    regs_table = get_platform_regs();
    regs = regs_table ? (uint64_t *)(uintptr_t)regs_table : NULL;
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];

        if (reg_addr == 0 && regs != NULL) {
            int hal_idx = esl_worker_to_hal_reg_index(i);

            if (hal_idx >= 0) {
                reg_addr = regs[hal_idx];
            }
        }
        if (reg_addr != 0) {
            write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICORE_EXIT_SIGNAL);
        }
    }
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];
        uint64_t spins;

        if (reg_addr == 0 && regs != NULL) {
            int hal_idx = esl_worker_to_hal_reg_index(i);

            if (hal_idx >= 0) {
                reg_addr = regs[hal_idx];
            }
        }
        if (reg_addr == 0) {
            continue;
        }
        spins = 0;
        while (read_reg(reg_addr, REG_ID_COND) != AICORE_EXITED_VALUE) {
            if (++spins > DEINIT_ACK_SPIN_MAX) {
                LOG_ERROR("Core %d deinit ack timeout (best-effort, continuing)", i);
                break;
            }
        }
    }
}
