/*
 * onboard_trace_sim.c — sim backend no-op definitions of the onboard tracing /
 * loop-stage hooks declared in platform.h. The sim has no device trace buffer, so
 * every hook is a no-op. (Onboard's real implementations live in
 * src/platform/onboard/onboard_trace_device.c.)
 */
#include "platform.h"

void esl_onboard_trace_set_base(volatile uint64_t *base)
{
    (void)base;
}

void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c)
{
    (void)thread;
    (void)stage;
    (void)aux_a;
    (void)aux_b;
    (void)aux_c;
}

void platform_cutter_loop_enter(void)
{
}

void platform_cutter_loop_iter(uint32_t loop_iter, uint16_t commit_task_id, uint32_t task_id)
{
    (void)loop_iter;
    (void)commit_task_id;
    (void)task_id;
}

void platform_cutter_drain_begin(uint16_t prev_commit, uint32_t task_id)
{
    (void)prev_commit;
    (void)task_id;
}

void platform_cutter_stall_trace(uint16_t cur_commit, uint32_t task_id)
{
    (void)cur_commit;
    (void)task_id;
}

void platform_dispatch_loop_exit(int tid, uint64_t elapsed_ns)
{
    (void)tid;
    (void)elapsed_ns;
}
