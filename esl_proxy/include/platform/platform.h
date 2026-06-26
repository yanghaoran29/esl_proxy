/*
 * platform.h — unified HAL interface for algorithm code.
 * Host sim links platform/sim sources; onboard links platform/onboard sources.
 */
#ifndef ESL_PROXY_PLATFORM_H
#define ESL_PROXY_PLATFORM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_HOST_WORKER_COUNT 72
#define PLATFORM_WORKER_BLOCK_DIM 24

#define ESL_AICPU_ROLE_CUTTER 0
#define ESL_AICPU_ROLE_DISPATCH 1
#ifndef ESL_AICPU_ROLE_ORCH
#define ESL_AICPU_ROLE_ORCH 2
#endif

#define ESL_TRACE_THREAD_SLOTS 4
#define ESL_TRACE_MAX_THREADS 3
#define ESL_TRACE_BASE_SLOT 8
#define ESL_TRACE_GLOBAL_SLOT (ESL_TRACE_BASE_SLOT + ESL_TRACE_MAX_THREADS * ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_TRACE_SLOTS (ESL_TRACE_GLOBAL_SLOT + ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_SLOTS 24
#define ESL_TRACE_THREAD_ANY (-1)

enum {
    ESL_TRACE_EXEC_ENTER = 1,
    ESL_TRACE_INIT_ONCE_WAIT,
    ESL_TRACE_INIT_ONCE_LEADER,
    ESL_TRACE_INIT_PLATFORM,
    ESL_TRACE_INIT_HANDSHAKE,
    ESL_TRACE_INIT_DONE,
    ESL_TRACE_WORKER_BARRIER,
    ESL_TRACE_CUTTER_START,
    ESL_TRACE_CUTTER_PRE_CALL,
    ESL_TRACE_CUTTER_LOOP_ENTER,
    ESL_TRACE_CUTTER_LOOP,
    ESL_TRACE_CUTTER_DRAIN,
    ESL_TRACE_CUTTER_DONE,
    ESL_TRACE_DISPATCH_START,
    ESL_TRACE_DISPATCH_PRE_CALL,
    ESL_TRACE_DISPATCH_LOOP_ENTER,
    ESL_TRACE_DISPATCH_PHASE1,
    ESL_TRACE_DISPATCH_PHASE2,
    ESL_TRACE_DISPATCH_STALL,
    ESL_TRACE_DISPATCH_DONE,
    ESL_TRACE_ORCH_START,
    ESL_TRACE_ORCH_PRE_CALL,
    ESL_TRACE_ORCH_IN_ENTRY,
    ESL_TRACE_ORCH_DONE,
    ESL_TRACE_SIGNAL_ORCH_DONE,
    ESL_TRACE_FINISHED_BARRIER,
    ESL_TRACE_SHUTDOWN,
    ESL_TRACE_EXEC_RETURN,
    ESL_TRACE_SPARE_WAIT,
    ESL_TRACE_SPARE_EXIT,
};

const char *esl_onboard_trace_stage_name(int stage);

int platform_pick_phys_worker(int core, int exe_type);

void platform_main_log_vwrite(int line, const char *fmt, va_list args);

/* Cache primitives used directly by algorithm-layer sched snapshot sync.
 * sim backend: no-op + compiler barrier; onboard backend: dc civac / dc cvac. */
void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

/* Dispatch loop exit: flush final scheduler stats to device_wall (onboard); sim: no-op. */
void platform_dispatch_loop_exit(int tid, uint64_t elapsed_ns);

/* Sim: pre-fill handshake ack fields + fake reg table (no real AICore).
 * Onboard: no-op — real AICore sets fields in aicore_executor. */
void platform_handshake_aicore_bootstrap(EslRuntime *runtime);

int esl_platform_init(EslRuntime *runtime);
void esl_platform_shutdown(EslRuntime *runtime);

void esl_onboard_trace_set_base(volatile uint64_t *base);
void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_H */
