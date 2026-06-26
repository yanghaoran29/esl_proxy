/*
 * Minimal Runtime / Handshake layout (platform layer).
 * Public field order matches simpler tensormap_and_ringbuffer Runtime
 * through func_id_to_addr_ for GM binary compatibility.
 */
#ifndef ESL_PROXY_RUNTIME_H
#define ESL_PROXY_RUNTIME_H

#include <stdint.h>

#ifndef RUNTIME_MAX_WORKER
#define RUNTIME_MAX_WORKER 72
#endif
#ifndef RUNTIME_MAX_FUNC_ID
#define RUNTIME_MAX_FUNC_ID 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EslHandshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile int32_t core_type;
    volatile uint32_t physical_core_id;
    volatile uint32_t aicpu_regs_ready;
    volatile uint32_t aicore_regs_ready;
} __attribute__((aligned(64))) EslHandshake;

typedef struct EslRuntime {
    EslHandshake workers[RUNTIME_MAX_WORKER];
    int worker_count;
    int aicpu_thread_num;
    int ready_queue_shards;
    uint64_t task_window_size;
    uint64_t heap_size;
    uint64_t dep_pool_size;
    uint64_t func_id_to_addr_[RUNTIME_MAX_FUNC_ID];
    uint8_t orch_to_sched;
} EslRuntime;

int32_t esl_aicpu_execute(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_RUNTIME_H */
