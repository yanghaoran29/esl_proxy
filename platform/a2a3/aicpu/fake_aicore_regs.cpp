/*
 * In-memory simulated AICore register blocks for esl_proxy onboard.
 * No real AICore kernel; dispatch writes DATA_MAIN_BASE then ACK+FIN on COND.
 */
#include "fake_aicore_regs.h"

#include "aicpu/platform_regs.h"
#include "common/platform_config.h"
#include "onboard_config.h"

#include <cstdint>
#include <cstring>

static uint8_t g_reg_blocks[ESL_PROXY_FAKE_AICORE_COUNT * SIM_REG_BLOCK_SIZE]
    __attribute__((aligned(64)));
static uint64_t g_regs_table[ESL_PROXY_FAKE_AICORE_COUNT];
static int g_initialized = 0;

int esl_fake_aicore_regs_init(void)
{
    if (g_initialized) {
        return 0;
    }

    std::memset(g_reg_blocks, 0, sizeof(g_reg_blocks));
    for (int i = 0; i < ESL_PROXY_FAKE_AICORE_COUNT; i++) {
        g_regs_table[i] = reinterpret_cast<uint64_t>(g_reg_blocks + i * SIM_REG_BLOCK_SIZE);
        platform_init_aicore_regs(g_regs_table[i]);
    }

    set_platform_regs(reinterpret_cast<uint64_t>(g_regs_table));
    g_initialized = 1;
    return 0;
}

void esl_fake_aicore_regs_shutdown(void)
{
    if (!g_initialized) {
        return;
    }

    for (int i = 0; i < ESL_PROXY_FAKE_AICORE_COUNT; i++) {
        write_reg(g_regs_table[i], RegId::DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
        write_reg(g_regs_table[i], RegId::COND, AICORE_IDLE_VALUE);
        write_reg(g_regs_table[i], RegId::FAST_PATH_ENABLE, REG_SPR_FAST_PATH_CLOSE);
    }
    g_initialized = 0;
}

uint32_t esl_fake_aicore_core_count(void)
{
    return static_cast<uint32_t>(ESL_PROXY_FAKE_AICORE_COUNT);
}

uint64_t esl_fake_aicore_reg_addr(int core)
{
    if (core < 0 || core >= ESL_PROXY_FAKE_AICORE_COUNT) {
        return 0;
    }
    return g_regs_table[core];
}

int esl_fake_aicore_dispatch(int core, uint32_t task_id)
{
    if (!g_initialized || core < 0 || core >= ESL_PROXY_FAKE_AICORE_COUNT) {
        return -1;
    }

    const uint64_t reg_addr = g_regs_table[core];
    write_reg(reg_addr, RegId::DATA_MAIN_BASE, task_id);
    write_reg(reg_addr, RegId::COND, MAKE_ACK_VALUE(task_id));
    write_reg(reg_addr, RegId::COND, MAKE_FIN_VALUE(task_id));
    return 0;
}
