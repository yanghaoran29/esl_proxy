/*
 * platform.h — unified HAL interface for algorithm code.
 * Host sim links platform/sim/platform_sim.c; onboard links platform/onboard HAL.
 */
#ifndef ESL_PROXY_PLATFORM_H
#define ESL_PROXY_PLATFORM_H

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

#define PLATFORM_HOST_WORKER_COUNT 72
#define PLATFORM_WORKER_BLOCK_DIM 24

void platform_init_from_env(void);
int platform_fake_kernel_enabled(void);
void platform_workers_start(int worker_count);
void platform_workers_stop(void);

int platform_pick_phys_worker(int core, int exe_type);

int platform_dispatch_block(uint16_t task_id, int exe_type, int core, int slot, uint64_t mask,
                            uint32_t duration_ns, uint32_t jitter_mask, uint16_t *phys_out);

int platform_pop_completion(PlatformCompletion *out);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_H */
