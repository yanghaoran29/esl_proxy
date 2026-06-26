/*
 * platform.h — unified HAL interface for algorithm code.
 * Host sim links platform/sim sources; onboard links platform/onboard sources.
 */
#ifndef ESL_PROXY_PLATFORM_H
#define ESL_PROXY_PLATFORM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t task_id;
    uint8_t exe_type;
    uint8_t core;
    uint8_t slot;
    uint64_t mask;
} PlatformCompletion;

typedef struct queue queue_t;

#define PLATFORM_HOST_WORKER_COUNT 72
#define PLATFORM_WORKER_BLOCK_DIM 24

#define ESL_AICPU_ROLE_CUTTER 0
#define ESL_AICPU_ROLE_DISPATCH 1
#ifndef ESL_AICPU_ROLE_ORCH
#define ESL_AICPU_ROLE_ORCH 2
#endif

#define ESL_TRACE_THREAD_SLOTS 4
#define ESL_TRACE_MAX_THREADS 3
#define ESL_TRACE_BASE_SLOT 8
#define ESL_TRACE_GLOBAL_SLOT (ESL_TRACE_BASE_SLOT + ESL_TRACE_MAX_THREADS * ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_TRACE_SLOTS (ESL_TRACE_GLOBAL_SLOT + ESL_TRACE_THREAD_SLOTS)
#define ESL_DEVICE_WALL_SLOTS 24
#define ESL_TRACE_THREAD_ANY (-1)

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

const char *esl_onboard_trace_stage_name(int stage);

void platform_init_from_env(void);
int platform_fake_kernel_enabled(void);
void platform_workers_start(int worker_count);
void platform_workers_stop(void);

int platform_pick_phys_worker(int core, int exe_type);

int platform_issue_block(uint16_t task_id, int exe_type, int core, int slot, uint64_t mask,
                         uint32_t duration_ns, uint32_t jitter_mask, uint16_t *phys_out);

int platform_pop_completion(PlatformCompletion *out);

uint64_t get_time_ns(void);
void platform_main_log_vwrite(int line, const char *fmt, va_list args);

void platform_publish_task_slot(uint16_t task_id);
void platform_publish_predecessor_cnt(uint16_t task_id);
void platform_publish_atomic_u64(uint64_t *field);
void platform_publish_u16(uint16_t *field);
void platform_consume_task_slot(uint16_t task_id);
void platform_consume_min_uncomplete(void);

/* Cache primitives used directly by algorithm-layer sched snapshot sync.
 * sim backend: no-op + compiler barrier; onboard backend: dc civac / dc cvac. */
void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

void platform_queue_lock_prepare(queue_t *queue);
void platform_queue_unlock_publish(queue_t *queue);

void platform_predecessor_ring_init(uint16_t **head_out);
void platform_state_buf_init(void **buf_out, size_t *size_out);
void platform_orch_done_notify(void);

void platform_scheduler_idle(void);

/* Onboard tracing / loop-stage hooks. Backend-provided: the onboard backend writes
 * the device trace buffer (onboard_trace_device.c); the sim backend supplies no-op
 * definitions (sim/onboard_trace_sim.c). No #ifdef in this shared header. */
void platform_cutter_loop_enter(void);
void platform_cutter_loop_iter(uint32_t loop_iter, uint16_t commit_task_id, uint32_t task_id);
void platform_cutter_drain_begin(uint16_t prev_commit, uint32_t task_id);
void platform_cutter_stall_trace(uint16_t cur_commit, uint32_t task_id);

/* Flush final scheduler stats to the device_wall buffer (onboard: esl_write_stats;
 * sim: no-op). Called once at dispatch loop exit. */
void platform_dispatch_loop_exit(int tid, uint64_t elapsed_ns);

void esl_onboard_trace_set_base(volatile uint64_t *base);
void esl_onboard_trace(int thread, int stage, uint64_t aux_a, uint64_t aux_b, uint64_t aux_c);

int platform_bringup(void);
void platform_teardown(void);
void platform_sched_stats_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_H */
