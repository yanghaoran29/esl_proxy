/*
 * onboard_trace_sim.c — sim backend no-op definitions of the onboard tracing /
 * loop-stage hooks declared in platform.h. The sim has no device trace buffer, so
 * every hook is a no-op. (Onboard's real implementations live in
 * src/platform/onboard/onboard_trace.c.)
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
