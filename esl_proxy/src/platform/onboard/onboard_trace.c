#include "platform.h"

#include "memory_barrier.h"
#include "platform_regs.h"

#if defined(ESL_PROXY_ONBOARD)
#include "onboard_log.h"

volatile uint64_t *g_trace_base;
#endif

const char *esl_onboard_trace_stage_name(int stage)
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

#if defined(ESL_PROXY_ONBOARD)

void esl_onboard_trace_set_base(volatile uint64_t *base)
{
    g_trace_base = base;
}

void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c)
{
    const uint64_t tid = (thread < 0) ? 0xFFU : (uint64_t)(uint32_t)thread;
    int slot_idx;
    volatile uint64_t *slot;

    LOG_ERROR("[trace] t=%d stage=%s(%d) a=%llu b=%llu c=%llu", thread,
              esl_onboard_trace_stage_name(stage), stage, (unsigned long long)aux_a,
              (unsigned long long)aux_b, (unsigned long long)aux_c);

    if (g_trace_base == NULL) {
        return;
    }

    if (thread >= 0 && thread < ESL_TRACE_MAX_THREADS) {
        slot_idx = ESL_TRACE_BASE_SLOT + thread * ESL_TRACE_THREAD_SLOTS;
    } else {
        slot_idx = ESL_TRACE_GLOBAL_SLOT;
    }
    slot = &g_trace_base[slot_idx];
    slot[0] = (tid << 32) | ((uint64_t)(uint32_t)stage);
    slot[1] = aux_a;
    slot[2] = aux_b;
    slot[3] = aux_c;
    cache_flush_range((const void *)slot, ESL_TRACE_THREAD_SLOTS * sizeof(uint64_t));
}

#endif /* ESL_PROXY_ONBOARD */
