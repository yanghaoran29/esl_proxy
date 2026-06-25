/*
 * AICPU onboard debug trace — writes last-known stage to device_wall GM slots
 * so host can read them even when aclrtSynchronizeStream returns 507018.
 *
 * device_wall layout (uint64 slots):
 *   [0-7]   existing stats (task_cnt, completed, diag, ...)
 *   [8-11]  thread 0 (cutter)
 *   [12-15] thread 1 (dispatch)
 *   [16-19] thread 2 (orch)
 *   [20-23] global / thread_id=-1 (init, exec_enter, spare)
 */
#ifndef ESL_PROXY_ONBOARD_TRACE_H
#define ESL_PROXY_ONBOARD_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESL_TRACE_THREAD_SLOTS 4
#define ESL_TRACE_MAX_THREADS 3
#define ESL_TRACE_BASE_SLOT 8
#define ESL_TRACE_GLOBAL_SLOT (ESL_TRACE_BASE_SLOT + ESL_TRACE_MAX_THREADS * ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_TRACE_SLOTS (ESL_TRACE_GLOBAL_SLOT + ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_SLOTS 24

/* Thread ids in trace (high 32 bits of slot 0 per region) */
#define ESL_TRACE_THREAD_ANY (-1)

/* Stage ids (low 32 bits) — keep in sync with esl_onboard_trace_stage_name(). */
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

static inline const char *esl_onboard_trace_stage_name(int stage)
{
    switch (stage) {
    case ESL_TRACE_EXEC_ENTER: return "exec_enter";
    case ESL_TRACE_INIT_ONCE_WAIT: return "init_once_wait";
    case ESL_TRACE_INIT_ONCE_LEADER: return "init_once_leader";
    case ESL_TRACE_INIT_PLATFORM: return "init_platform";
    case ESL_TRACE_INIT_HANDSHAKE: return "init_handshake";
    case ESL_TRACE_INIT_DONE: return "init_done";
    case ESL_TRACE_WORKER_BARRIER: return "worker_barrier";
    case ESL_TRACE_CUTTER_START: return "cutter_start";
    case ESL_TRACE_CUTTER_PRE_CALL: return "cutter_pre_call";
    case ESL_TRACE_CUTTER_LOOP_ENTER: return "cutter_loop_enter";
    case ESL_TRACE_CUTTER_LOOP: return "cutter_loop";
    case ESL_TRACE_CUTTER_DRAIN: return "cutter_drain";
    case ESL_TRACE_CUTTER_DONE: return "cutter_done";
    case ESL_TRACE_DISPATCH_START: return "dispatch_start";
    case ESL_TRACE_DISPATCH_PRE_CALL: return "dispatch_pre_call";
    case ESL_TRACE_DISPATCH_LOOP_ENTER: return "dispatch_loop_enter";
    case ESL_TRACE_DISPATCH_PHASE1: return "dispatch_phase1";
    case ESL_TRACE_DISPATCH_PHASE2: return "dispatch_phase2";
    case ESL_TRACE_DISPATCH_STALL: return "dispatch_stall";
    case ESL_TRACE_DISPATCH_DONE: return "dispatch_done";
    case ESL_TRACE_ORCH_START: return "orch_start";
    case ESL_TRACE_ORCH_PRE_CALL: return "orch_pre_call";
    case ESL_TRACE_ORCH_IN_ENTRY: return "orch_in_entry";
    case ESL_TRACE_ORCH_DONE: return "orch_done";
    case ESL_TRACE_SIGNAL_ORCH_DONE: return "signal_orch_done";
    case ESL_TRACE_FINISHED_BARRIER: return "finished_barrier";
    case ESL_TRACE_SHUTDOWN: return "shutdown";
    case ESL_TRACE_EXEC_RETURN: return "exec_return";
    case ESL_TRACE_SPARE_WAIT: return "spare_wait";
    case ESL_TRACE_SPARE_EXIT: return "spare_exit";
    default: return "unknown";
    }
}

#ifdef ESL_PROXY_ONBOARD

extern volatile uint64_t *g_trace_base;

static inline void esl_onboard_trace_set_base(volatile uint64_t *base)
{
    g_trace_base = base;
}

void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c);

#else

static inline void esl_onboard_trace_set_base(volatile uint64_t *base)
{
    (void)base;
}

static inline void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c)
{
    (void)thread;
    (void)stage;
    (void)aux_a;
    (void)aux_b;
    (void)aux_c;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_TRACE_H */
