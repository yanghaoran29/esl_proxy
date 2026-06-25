/*
 * fake_kernel.h — shared busy-wait stub (Host ns clock or onboard SYS_CNT).
 */
#ifndef ESL_PROXY_FAKE_KERNEL_H
#define ESL_PROXY_FAKE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*esl_fake_now_fn)(void);

#if defined(__DAV_VEC__) || defined(__DAV_CUBE__)
#ifndef __aicore__
#define __aicore__ [aicore]
#endif
#define ESL_FAKE_KERNEL_FN __aicore__ __attribute__((always_inline)) static inline
#else
#define ESL_FAKE_KERNEL_FN static inline
#endif

/* Busy-wait for duration_ns ± jitter derived from start & jitter_mask (§4.2). */
ESL_FAKE_KERNEL_FN void esl_fake_kernel_busy_wait_ns(uint64_t duration_ns, uint64_t jitter_mask,
                                                     esl_fake_now_fn now_ns)
{
    uint64_t start;
    int64_t wait_ns;
    uint64_t end;

    if (duration_ns == 0U || now_ns == NULL) {
        return;
    }
    start = now_ns();
    wait_ns = (int64_t)duration_ns;
    if (jitter_mask != 0U) {
        const int64_t jitter_ns =
            (int64_t)(start & jitter_mask) - (int64_t)((jitter_mask + 1U) / 2U);
        wait_ns += jitter_ns;
    }
    if (wait_ns < 1) {
        wait_ns = (int64_t)duration_ns;
    }
    end = start + (uint64_t)wait_ns;
    while (now_ns() < end) {
        /* spin */
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_FAKE_KERNEL_H */
