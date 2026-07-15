/*
 * aicore_wrapper.cpp — sim AICore worker thread entry (TLS reg + algorithm executor).
 */
#include "aicore_executor.h"

#include "platform_config.h"
#include "runtime.h"
#include "aicore.h"
#include "sim_core_regs.h"

#include <cstdint>

extern "C" void aicore_execute_wrapper(EslRuntime *runtime, int block_idx, CoreType core_type,
                                       uint32_t physical_core_id, uint64_t regs_table)
{
    int hal_idx;
    uint64_t *table;
    SimCoreReg *reg;

    (void)physical_core_id;
    hal_idx = esl_worker_to_hal_reg_index(block_idx);
    if (hal_idx < 0 || regs_table == 0) {
        return;
    }
    table = reinterpret_cast<uint64_t *>(regs_table);
    reg = sim_core_reg_at(table[hal_idx]);
    sim_aicore_tls_set(reg, (uint32_t)hal_idx);

    aicore_execute(runtime, block_idx, core_type);
}
