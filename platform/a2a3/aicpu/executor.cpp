/*
 * esl_proxy onboard AICPU executor — 3 logical threads:
 *   0 cutter, 1 dispatch, 2 orchestrator
 *
 * No AICore kernel / handshake: simulated register blocks on AICPU.
 */

#include "aicore_bridge.h"
#include "executor.h"
#include "onboard_config.h"
#include "onboard_shm_sync.h"
#include "onboard_sync.h"

#include <atomic>
#include <cstdint>

extern "C" {
void cutter_loop_run(void);
void dispatch_loop_run(int tid);
void aicpu_orchestration_entry(uint64_t orch_args);
void esl_signal_orch_done(void);

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge);
void esl_platform_shutdown(AicoreBridge *bridge);
}

static std::atomic<int> g_thread_idx{0};
static std::atomic<int> g_finished_count{0};
static std::atomic<int> g_workers_in_loop{0};
static std::atomic<bool> g_init_done{false};
static std::atomic<bool> g_init_failed{false};
static AicoreBridge g_bridge;

static int init_once(EslRuntime *runtime)
{
    static std::atomic_flag once = ATOMIC_FLAG_INIT;
    if (once.test_and_set(std::memory_order_acquire)) {
        while (!g_init_done.load(std::memory_order_acquire) &&
               !g_init_failed.load(std::memory_order_acquire)) {
        }
        return g_init_failed.load(std::memory_order_acquire) ? -1 : 0;
    }

    if (esl_platform_init(runtime, &g_bridge) != 0) {
        g_init_failed.store(true, std::memory_order_release);
        return -1;
    }

    g_init_done.store(true, std::memory_order_release);
    return 0;
}

extern "C" void esl_onboard_worker_enter(void)
{
    g_workers_in_loop.fetch_add(1, std::memory_order_release);
}

extern "C" void esl_onboard_wait_workers(int required)
{
    while (g_workers_in_loop.load(std::memory_order_acquire) < required) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
}

extern "C" int32_t esl_aicpu_execute(EslRuntime *runtime)
{
    if (runtime == nullptr) {
        return -1;
    }

    if (init_once(runtime) != 0) {
        return -1;
    }

    esl_onboard_invalidate_runtime(runtime);

    int idx = g_thread_idx.fetch_add(1, std::memory_order_acq_rel);
#if ESL_PROXY_ONBOARD_SINGLE_AICPU
    if (idx == 0) {
        aicpu_orchestration_entry(0);
        esl_signal_orch_done();
        dispatch_loop_run(0);
        cutter_loop_run();
        esl_platform_shutdown(&g_bridge);
        return 0;
    }
    return 0;
#else
    if (idx >= ESL_PROXY_AICPU_THREAD_NUM) {
        return 0;
    }

    if (idx == 0) {
        esl_onboard_worker_enter();
        cutter_loop_run();
    } else if (idx == 1) {
        esl_onboard_worker_enter();
        dispatch_loop_run(0);
    } else if (idx == 2) {
        aicpu_orchestration_entry(0);
        esl_signal_orch_done();
    }

    int prev = g_finished_count.fetch_add(1, std::memory_order_acq_rel);
    if (prev + 1 == ESL_PROXY_AICPU_THREAD_NUM) {
        esl_platform_shutdown(&g_bridge);
    }

    return 0;
#endif
}
