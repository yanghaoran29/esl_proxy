/*
 * swimlane.h - Swimlane profiling for esl_proxy (binary on/off, full collection)
 *
 * Per-lane (orchestrator / manager / dispatch / cutter / executor) timeline
 * collection, exported as raw JSON and converted to a Perfetto Chrome-Trace by
 * tools/swimlane_converter.py. Ported in spirit from simpler's L2 swimlane.
 *
 * THE FEATURE IS ENTIRELY MACRO-GATED. Every instrumentation site is a SWIM_*
 * macro that expands to real code ONLY when ESL_SWIMLANE is defined; otherwise
 * it is ((void)0). A default build emits no swimlane code, data, or symbols and
 * pays zero overhead. There are no runtime levels: compiled in => full collection.
 *
 * Threading model: each lane has a single writer thread (the thread that owns
 * that role / executor slot). swim_dump() runs after the run is quiesced, so
 * reads need no synchronization.
 *
 * Timestamps: aarch64 reads CNTVCT_EL0 (frequency from CNTFRQ_EL0); other arches
 * fall back to clock_gettime(CLOCK_MONOTONIC) in nanoseconds.
 *
 * Naming follows Constitution XI: short `swim_` module prefix, no dag_ prefix.
 */

#ifndef ESL_PROXY_SWIMLANE_H
#define ESL_PROXY_SWIMLANE_H

#include <stdint.h>

/* ---- public enums/constants (defined unconditionally so instrumentation
 *      referencing them compiles in both on and off builds) ---------------- */

typedef enum {
    LANE_ORCH     = 0,
    LANE_MANAGER  = 1,
    LANE_DISPATCH = 2,
    LANE_CUTTER   = 3,
    LANE_EXEC     = 4,
} lane_kind_t;

typedef enum {
    PH_DISPATCH    = 0,
    PH_COMPLETE    = 1,
    PH_ORCH_SUBMIT = 2,
} phase_id_t;

/* swim_stamp() selector: which timestamp field the one-shot marker fills. */
enum { SWIM_DISPATCH = 0, SWIM_FINISH = 1 };

/* Record discriminator (so the dumper can label rows unambiguously). */
enum { SWIM_RT_TASK = 0, SWIM_RT_STAMP = 1, SWIM_RT_PHASE = 2 };

/* Per-lane ring capacity (records). Override with -DSWIM_LANE_CAP=N. */
#ifndef SWIM_LANE_CAP
#define SWIM_LANE_CAP 4096u
#endif

/* Max lane id + 1. orch=0, manager=1, dispatch=2+d, cutter=4+c, exec=8+slot
 * where slot in [0, THREAD_CNT*EXE_TYPE_CNT*AIC_CNT) = [0,240) -> up to 247. */
#ifndef SWIM_MAX_LANES
#define SWIM_MAX_LANES 256u
#endif

#ifdef ESL_SWIMLANE

#include <stddef.h>

#if !defined(__aarch64__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#endif

/* ---- record (64-byte aligned, mirrors simpler's L2PerfRecord shape) ------ */

typedef struct {
    uint64_t start;     /* start_time  (CNTVCT ticks / ns)            */
    uint64_t end;       /* end_time                                   */
    uint64_t dispatch;  /* dispatch_time (filled by swim_stamp)       */
    uint64_t finish;    /* finish_time   (filled by swim_stamp)       */
    uint64_t task_id;   /* zero-extended uint16 task id (0 for phases)*/
    uint32_t func_id;   /* kernel/function identifier                 */
    uint16_t lane;
    uint8_t  kind;      /* lane_kind_t                                */
    uint8_t  phase;     /* phase_id_t (only meaningful for phase rows)*/
    uint8_t  rtype;     /* SWIM_RT_TASK / SWIM_RT_STAMP / SWIM_RT_PHASE */
} swim_record_t __attribute__((aligned(64)));

/* ---- inline timer (pure C; __asm__ form passes -pedantic) ---------------- */

static inline uint64_t swim_now(void) {
#if defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t swim_freq(void) {
#if defined(__aarch64__)
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
#else
    return 1000000000ull; /* clock_gettime fallback is in nanoseconds */
#endif
}

/* ---- API (defined in src/swimlane.c) ------------------------------------- */

void swim_init(void);
void swim_lane_register(uint16_t lane, uint8_t kind, const char *name);
void swim_task_begin(uint16_t lane, uint64_t task_id, uint32_t func_id);
void swim_task_end(uint16_t lane, uint64_t task_id);
/* one-shot full bar with explicit start/end (for cross-call synthesized spans
 * where the begin/end scratch model does not fit, e.g. executor slots) */
void swim_task_record(uint16_t lane, uint64_t task_id, uint32_t func_id,
                      uint64_t start, uint64_t end);
void swim_stamp(uint16_t lane, uint64_t task_id, int which);
void swim_phase_begin(uint16_t lane, uint8_t phase);
void swim_phase_end(uint16_t lane);
void swim_dump(const char *path);
void swim_shutdown(void);

/* ---- gated macros (the only thing instrumentation sites should use) ------ */

#define SWIM_INIT()                       swim_init()
#define SWIM_LANE(lane, kind, name)       swim_lane_register((uint16_t)(lane), (uint8_t)(kind), (name))
#define SWIM_TASK_BEGIN(lane, tid, fid)   swim_task_begin((uint16_t)(lane), (uint64_t)(tid), (uint32_t)(fid))
#define SWIM_TASK_END(lane, tid)          swim_task_end((uint16_t)(lane), (uint64_t)(tid))
#define SWIM_TASK_RECORD(lane, tid, fid, s, e) \
    swim_task_record((uint16_t)(lane), (uint64_t)(tid), (uint32_t)(fid), (uint64_t)(s), (uint64_t)(e))
#define SWIM_STAMP(lane, tid, which)      swim_stamp((uint16_t)(lane), (uint64_t)(tid), (which))
#define SWIM_PHASE_BEGIN(lane, phase)     swim_phase_begin((uint16_t)(lane), (uint8_t)(phase))
#define SWIM_PHASE_END(lane)              swim_phase_end((uint16_t)(lane))
#define SWIM_DUMP(path)                   swim_dump(path)
#define SWIM_SHUTDOWN()                   swim_shutdown()
/* read the raw CNTVCT timer at an instrumentation site (for synthesized spans) */
#define SWIM_NOW()                        swim_now()

#else /* !ESL_SWIMLANE — every site collapses to a no-op, zero footprint */

#define SWIM_INIT()                       ((void)0)
#define SWIM_LANE(lane, kind, name)       ((void)0)
#define SWIM_TASK_BEGIN(lane, tid, fid)   ((void)0)
#define SWIM_TASK_END(lane, tid)          ((void)0)
#define SWIM_TASK_RECORD(lane, tid, fid, s, e) ((void)0)
#define SWIM_STAMP(lane, tid, which)      ((void)0)
#define SWIM_PHASE_BEGIN(lane, phase)     ((void)0)
#define SWIM_PHASE_END(lane)              ((void)0)
#define SWIM_DUMP(path)                   ((void)0)
#define SWIM_SHUTDOWN()                   ((void)0)
#define SWIM_NOW()                        ((uint64_t)0)

#endif /* ESL_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_H */
