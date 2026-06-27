#include "platform.h"

#include "memory_barrier.h"
#include "platform_regs.h"

#if defined(ESL_PROXY_ONBOARD)
#include "onboard_log.h"

volatile uint64_t *g_trace_base;

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
