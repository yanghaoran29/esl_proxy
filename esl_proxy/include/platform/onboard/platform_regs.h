/*
 * Platform register access and cache operations (onboard HAL).
 */
#ifndef ESL_PROXY_PLATFORM_REGS_H
#define ESL_PROXY_PLATFORM_REGS_H

#include <stddef.h>
#include <stdint.h>

#include "onboard_config.h"
#include "tools.h"

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_regs(uint64_t regs);
uint64_t get_platform_regs(void);

static inline volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg)
{
    return (volatile uint32_t *)(uintptr_t)(reg_base_addr + reg_offset(reg));
}

static inline uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    return (uint64_t)*get_reg_ptr(reg_base_addr, reg);
}

static inline void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    *get_reg_ptr(reg_base_addr, reg) = (uint32_t)value;
}

static inline void platform_init_aicore_regs(uint64_t reg_addr)
{
    write_reg(reg_addr, REG_ID_FAST_PATH_ENABLE, REG_SPR_FAST_PATH_OPEN);
    write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
}

void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_REGS_H */
