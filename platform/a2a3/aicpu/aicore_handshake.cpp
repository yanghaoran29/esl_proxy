/*
 * Minimal AICPU↔AICore handshake for esl_proxy M1 onboard.
 * M1: publish aicpu_ready and record reg bases; AICore completes handshake
 * asynchronously while cutter/dispatch run. Shutdown sends exit via regs.
 */
#include "aicore_handshake.h"

#include "aicpu/platform_regs.h"
#include "aicpu/device_time.h"
#include "common/memory_barrier.h"
#include "common/unified_log.h"

#include <cstdint>

static uint64_t g_core_reg_addrs[RUNTIME_MAX_WORKER];

static constexpr uint64_t kHandshakeSpinTicks = 2000000000ULL; /* ~2s */

static bool wait_handshake_field(volatile uint32_t *field, uint32_t expect)
{
    uint64_t t0 = get_sys_cnt_aicpu();
    while (true) {
        cache_invalidate_range(const_cast<const void *>(static_cast<volatile void *>(field)), sizeof(*field));
        if (*field == expect) {
            return true;
        }
        if (get_sys_cnt_aicpu() - t0 > kHandshakeSpinTicks) {
            return false;
        }
    }
}

extern "C" int esl_handshake_all_cores(EslRuntime *runtime)
{
    if (runtime == nullptr) {
        return -1;
    }

    const int n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", n);
        return -1;
    }

    const uint64_t regs_table = get_platform_regs();
    if (regs_table == 0) {
        LOG_ERROR("Platform regs table not set");
        return -1;
    }

    auto *regs = reinterpret_cast<uint64_t *>(regs_table);
    const int n_handshake = n;

    LOG_INFO_V0("esl_proxy handshake for %d workers", n_handshake);

    for (int i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];
        wk->task = 0;
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_ready = 1;
        cache_flush_range(
            const_cast<const void *>(static_cast<volatile void *>(wk)), sizeof(EslHandshake));
    }

    for (int i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];

        if (!wait_handshake_field(&wk->aicore_regs_ready, 1)) {
            LOG_ERROR("Core %d aicore_regs_ready timeout", i);
            continue;
        }

        cache_invalidate_range(
            const_cast<const void *>(static_cast<volatile void *>(&wk->physical_core_id)),
            sizeof(wk->physical_core_id));
        const uint32_t phys = wk->physical_core_id;
        if (phys >= RUNTIME_MAX_WORKER) {
            LOG_ERROR("Core %d invalid physical_core_id=%u", i, phys);
            continue;
        }

        uint64_t reg_addr = regs[phys];
        g_core_reg_addrs[i] = reg_addr;
        platform_init_aicore_regs(reg_addr);
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_regs_ready = 1;
        cache_flush_range(
            const_cast<const void *>(static_cast<volatile void *>(&wk->aicpu_regs_ready)),
            sizeof(wk->aicpu_regs_ready));

        if (!wait_handshake_field(reinterpret_cast<volatile uint32_t *>(&wk->aicore_done),
                                  static_cast<uint32_t>(i + 1))) {
            LOG_ERROR("Core %d aicore_done timeout", i);
            continue;
        }
    }

    return 0;
}

extern "C" void esl_shutdown_all_cores(EslRuntime *runtime)
{
    if (runtime == nullptr) {
        return;
    }

    const int n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        return;
    }

    const int n_shutdown = n;
    LOG_INFO_V0("esl_proxy shutting down %d AICore workers", n_shutdown);

    const uint64_t regs_table = get_platform_regs();
    auto *regs = regs_table ? reinterpret_cast<uint64_t *>(regs_table) : nullptr;

    for (int i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];
        if (reg_addr == 0 && regs != nullptr) {
            reg_addr = regs[i];
        }
        if (reg_addr == 0) {
            continue;
        }
        if (platform_deinit_aicore_regs(reg_addr) != 0) {
            LOG_ERROR("Core %d deinit timed out", i);
        }
    }
}
