/*
 * fake_aicore_host.h — Host CPU fake AICore workers (one pthread per phys worker).
 */
#ifndef ESL_PROXY_FAKE_AICORE_HOST_H
#define ESL_PROXY_FAKE_AICORE_HOST_H

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
} HostFakeFin;

void host_fake_aicore_configure_from_env(void);
int host_fake_aicore_kernel_enabled(void);

void host_fake_aicore_start(int worker_count);
void host_fake_aicore_stop(void);

/* Synchronous FIN (no worker threads): dispatch thread only. */
int host_fake_aicore_finish_sync(uint16_t task_id, int exe_type, int core, int slot, uint64_t mask);

/* Queue one subtask on phys_worker; returns 0 on success. Requires workers started. */
int host_fake_aicore_submit(int phys_worker, uint16_t task_id, int exe_type, int core, int slot,
                            uint64_t mask, uint32_t duration_ns, uint32_t jitter_mask);

/* Dispatch thread drains FINs (MPSC queue). Returns 0 if popped, -1 if empty. */
int host_fake_fin_pop(HostFakeFin *out);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_FAKE_AICORE_HOST_H */
