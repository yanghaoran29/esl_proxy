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
