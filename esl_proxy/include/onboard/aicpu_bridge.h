/*
 * AICPU platform glue: AICore bridge, register access, shm sync.
 */
#ifndef ESL_PROXY_AICPU_BRIDGE_H
#define ESL_PROXY_AICPU_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "conf.h"
#include "esl_runtime.h"
#include "onboard_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AicoreBridge {
    EslRuntime *runtime;
    uint64_t fake_kernel_addr;
    int initialized;
} AicoreBridge;

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr);
void aicore_bridge_shutdown(AicoreBridge *bridge);
int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid);
int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id,
                                int core, int slot, int exe_type, uint32_t block_idx);

static inline int esl_phys_worker(int core, int exe_type)
{
#ifdef ESL_PROXY_ONBOARD
    /* Default mapping (lane 0). Dispatch path stores actual phys in block_idx. */
    if (exe_type == 0) {
        return core;
    }
    return ESL_PROXY_ONBOARD_BLOCK_DIM + core * PLATFORM_AIV_CORES_PER_BLOCKDIM;
#else
    return core * EXE_TYPE_CNT + exe_type;
#endif
}

int esl_handshake_all_cores(EslRuntime *runtime);
void esl_shutdown_all_cores(EslRuntime *runtime);
uint64_t esl_handshake_reg_addr(int core_idx);

void esl_dispatch_payload_prepare(EslRuntime *runtime, int core, uint32_t reg_task_id,
                                  const EslOnboardDispatchInput *input);

void set_platform_regs(uint64_t regs);
uint64_t get_platform_regs(void);

volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg);
uint64_t read_reg(uint64_t reg_base_addr, RegId reg);
void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value);

void platform_init_aicore_regs(uint64_t reg_addr);
void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

void esl_onboard_invalidate_runtime(void *runtime);
void esl_onboard_flush_shared_after_orch(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_BRIDGE_H */
