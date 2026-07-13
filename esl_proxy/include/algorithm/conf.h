#ifndef CONF_H
#define CONF_H

#include "worker_map.h"

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
#define NODE_BUFF_SIZE 65536

// TODO: ERROR
#define CON_NODE_CNT 256

#define AIC_OSTD 2
#define AIC_CNT ESL_PROXY_WORKER_BLOCK_DIM
#define EXE_TYPE_CNT 2

#define CUTTER_BATCH_SIZE 512
#define ADD_BATCH_SIZE 240
#define LOCAL_BUFFER_SIZE 512
#define DISPATCH_COMPLETE_BATCH 512

/* Number of scheduler lanes. Each lane = one cutter thread paired 1:1 with one
 * dispatch thread and a disjoint, strided subset of AICore cores
 * (lane i owns cores { c : c % ESL_LANE_CNT == i }). Set this to scale. */
#ifndef ESL_LANE_CNT
#define ESL_LANE_CNT 1
#endif

/* Orchestrator-first (先行执行 Orchestrator). 1: the orchestrator runs to
 * completion before any cutter/dispatch work — onboard it is folded into the
 * last dispatch lane's phase-1; in the sim the cutter/dispatch lanes idle-wait
 * on g_orch_is_done. Safe only for workloads <= RING_SIZE (nothing advances
 * g_min_uncomplete_task during orch, so new_task backpressure caps live tasks
 * at RING_SIZE). 0: the orchestrator overlaps as a standalone thread and
 * cutter/dispatch drain concurrently. Independent of ESL_LANE_CNT — a single
 * cutter/dispatch lane can still run orchestrator-first. This #ifndef is a
 * fallback; the Makefile -DESL_ORCH_FIRST is authoritative. Default preserves
 * prior behavior (folded orch-first at >=2 lanes, overlapped at 1 lane). */
#ifndef ESL_ORCH_FIRST
#define ESL_ORCH_FIRST (ESL_LANE_CNT >= 2)
#endif

#define CUTTER_THREAD_CNT ESL_LANE_CNT
#define DISPATCH_THREAD_CNT ESL_LANE_CNT
#define EXECUTOR_THREAD_CNT 1

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(CUTTER_THREAD_CNT == DISPATCH_THREAD_CNT,
               "cutter and dispatch threads must be paired 1:1");
#endif

/* core -> owning lane (strided). Matches executor.c completion routing. */
#define CORE_LANE(core) ((core) % DISPATCH_THREAD_CNT)

/* Dispatch mode. 0 (default) = basic dispatch; 1 = double-buffer dispatch.
 * Selected via `make DISPATCH=double_buffer` (Makefile adds -D=1). Basic mode
 * filters cores held by in-flight MIX clusters (dispatch_mix_core_busy) and
 * runs a second-pass MIX prefetch (dispatch_mix_prefetch); double-buffer mode
 * skips both so dispatch_prefetch can actively use the 2nd slot. */
#ifndef ESL_DISPATCH_DOUBLE_BUFFER
#define ESL_DISPATCH_DOUBLE_BUFFER 0
#endif

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#ifndef WORKER_LOG
#define WORKER_LOG 1
#endif

/* 1: compile in main thread logs; output to screen only */
#ifndef MAIN_LOG
#define MAIN_LOG 1
#endif

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

/* 1: enable aicpu_orchestration_entry execution time logging in nanoseconds */
#define ORCHESTRATION_TIME 1

/* 1: compile post-orchestration DAG dump; runtime via DEP_DUMP=1 env */
#ifndef DEP_DUMP
#define DEP_DUMP 0
#endif

/* 1: skip tensormap lookup/insert and succeed(); all tasks submit with no edges */
#ifndef NO_DEPS
#define NO_DEPS 0
#endif

#endif /* CONF_H */
