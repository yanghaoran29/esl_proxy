/*
 * Host-side platform register stubs for CPU sim (no MMIO).
 */
#include "platform_regs.h"

#include <stddef.h>
#include <stdint.h>

static uint64_t g_platform_regs;

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

/* Sim has no MMIO. read_reg reports the AICore as already exited so the handshake
 * shutdown spin terminates immediately; write_reg / init are no-ops. */
uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    (void)reg_base_addr;
    (void)reg;
    return (uint64_t)AICORE_EXITED_VALUE;
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    (void)reg_base_addr;
    (void)reg;
    (void)value;
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    (void)reg_addr;
}
