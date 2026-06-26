/*
 * platform_sim.c — host CPU sim backend for platform.h
 */
#include "platform.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "fake_aicore_host.h"
#include "worker_map.h"

void platform_init_from_env(void)
{
    host_fake_aicore_configure_from_env();
}

int platform_fake_kernel_enabled(void)
{
    return host_fake_aicore_kernel_enabled();
}

void platform_workers_start(int worker_count)
{
    host_fake_aicore_start(worker_count);
}

void platform_workers_stop(void)
{
    host_fake_aicore_stop();
}

int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

int platform_issue_block(uint16_t task_id, int exe_type, int core, int slot, uint64_t mask,
                         uint32_t duration_ns, uint32_t jitter_mask, uint16_t *phys_out)
{
    if (host_fake_aicore_kernel_enabled()) {
        const int phys = esl_pick_phys_worker(core, exe_type);
        if (phys_out != NULL) {
            *phys_out = (uint16_t)phys;
        }
        if (phys < 0) {
            return -1;
        }
        return host_fake_aicore_submit(phys, task_id, exe_type, core, slot, mask, duration_ns,
                                       jitter_mask);
    }
    return host_fake_aicore_finish_sync(task_id, exe_type, core, slot, mask);
}

int platform_pop_completion(PlatformCompletion *out)
{
    HostFakeFin fin;

    if (out == NULL) {
        return -1;
    }
    if (host_fake_fin_pop(&fin) != 0) {
        return -1;
    }
    out->task_id = fin.task_id;
    out->exe_type = fin.exe_type;
    out->core = fin.core;
    out->slot = fin.slot;
    out->mask = fin.mask;
    return 0;
}

uint64_t get_time_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void platform_main_log_vwrite(int line, const char *fmt, va_list args)
{
    fprintf(stdout, "[main:%d] ", line);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
}

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
