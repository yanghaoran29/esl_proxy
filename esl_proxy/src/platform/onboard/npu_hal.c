/* npu_hal.c — onboard MMIO register table and cache operations. */
#define _GNU_SOURCE

#include "platform_regs.h"
#include "tools.h" /* reg_offset() for get_reg_ptr */

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

static volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg)
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
