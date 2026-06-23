#ifndef CONF_H
#define CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
/* Predecessor-id ring: total dependency edges across the whole run. qwen3's
 * attention DAG has many edges; 8192 overflowed. */
#define NODE_BUFF_SIZE 65536

/* Max successors recorded per task. A shared-scope producer (e.g. an RMSNorm
 * tile feeding every head's projection) fans out to many consumers; 32
 * overflowed g_successor_buf[].node[] and corrupted the successor graph. */
#define CON_NODE_CNT 256

#define AIC_OSTD 2
#define AIC_CNT 60
#define EXE_TYPE_CNT 2

#define CUTTER_BATCH_SIZE 512
#define ADD_BATCH_SIZE 240
/* Per-pass cutter ready buffer. Sized to RING_SIZE so one deal_completed pass
 * can hold a high-fanout burst (qwen3 readies hundreds of successors when a
 * shared-scope predecessor completes); 512 overflowed and lost ~348 tasks. */
#define LOCAL_BUFFER_SIZE RING_SIZE
#define DISPATCH_COMPLETE_BATCH 512

#define CUTTER_THREAD_CNT 1
#define DISPATCH_THREAD_CNT 1
#define EXECUTOR_THREAD_CNT 1

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

/* 1: orchestration only (no cutter/dispatch threads); for DAG log generation */
#ifndef ORCH_ONLY
#define ORCH_ONLY 0
#endif

#endif /* CONF_H */
