/*
 * Host-side platform register stubs for CPU sim (no MMIO).
 */
#include "platform_regs.h"

#include "log.h"
#include "platform_config.h"

#include <stddef.h>
#include <stdint.h>

#define SIM_REG_SLOTS 128
#define SIM_MAX_COMPLETED_PER_REG 256

typedef struct {
    uint64_t reg_base;
    uint8_t exited;
    uint32_t completed[SIM_MAX_COMPLETED_PER_REG];
    int completed_cnt;
} SimWorkerReg;

static uint64_t g_platform_regs;
static SimWorkerReg g_sim_worker_regs[SIM_REG_SLOTS];

static SimWorkerReg *sim_reg_lookup(uint64_t reg_base_addr, int create)
{
    for (int i = 0; i < SIM_REG_SLOTS; i++) {
        if (g_sim_worker_regs[i].reg_base == reg_base_addr) {
            return &g_sim_worker_regs[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < SIM_REG_SLOTS; i++) {
        if (g_sim_worker_regs[i].reg_base == 0) {
            g_sim_worker_regs[i].reg_base = reg_base_addr;
            return &g_sim_worker_regs[i];
        }
    }
    return NULL;
}

void set_platform_regs(uint64_t regs)
{
    g_platform_regs = regs;
}

uint64_t get_platform_regs(void)
{
    return g_platform_regs;
}

void cache_invalidate_range(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}

void cache_flush_range(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}

uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    SimWorkerReg *wr;

    if (reg != REG_ID_COND) {
        return 0;
    }
    wr = sim_reg_lookup(reg_base_addr, 0);
    if (wr == NULL || wr->exited) {
        return (uint64_t)AICORE_EXITED_VALUE;
    }
    return MAKE_ACK_VALUE(0);
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    SimWorkerReg *wr;

    if (reg != REG_ID_DATA_MAIN_BASE) {
        return;
    }
    wr = sim_reg_lookup(reg_base_addr, 1);
    if (wr == NULL) {
        return;
    }
    if (value == (uint64_t)AICORE_EXIT_SIGNAL) {
        wr->exited = 1;
        wr->completed_cnt = 0;
        return;
    }
    if (wr->completed_cnt < SIM_MAX_COMPLETED_PER_REG) {
        wr->completed[wr->completed_cnt++] = (uint32_t)value;
    } else {
        MAIN_LOGF("[sim] reg 0x%llx completed queue full (task=%u)",
                  (unsigned long long)reg_base_addr, (unsigned)(uint32_t)value);
    }
    wr->exited = 0;
}

int platform_reg_task_finished(uint64_t reg_base_addr, uint32_t reg_task)
{
    SimWorkerReg *wr;

    wr = sim_reg_lookup(reg_base_addr, 0);
    if (wr == NULL || wr->exited) {
        return 0;
    }
    for (int i = 0; i < wr->completed_cnt; i++) {
        if (wr->completed[i] == reg_task) {
            return 1;
        }
    }
    return 0;
}

void platform_reg_task_ack(uint64_t reg_base_addr, uint32_t reg_task)
{
    SimWorkerReg *wr;

    wr = sim_reg_lookup(reg_base_addr, 0);
    if (wr == NULL) {
        return;
    }
    for (int i = 0; i < wr->completed_cnt; i++) {
        if (wr->completed[i] == reg_task) {
            wr->completed[i] = wr->completed[wr->completed_cnt - 1];
            wr->completed_cnt--;
            return;
        }
    }
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    (void)reg_addr;
}
