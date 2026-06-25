/*
 * worker_map.h — logical dispatch core → physical worker (24 AIC + 48 AIV = 72).
 */
#ifndef ESL_PROXY_WORKER_MAP_H
#define ESL_PROXY_WORKER_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESL_PROXY_WORKER_BLOCK_DIM 24
#define ESL_PROXY_AIV_LANES_PER_BLOCK 2
#define ESL_PROXY_HOST_WORKER_COUNT 72

/* exe_type: TASK_TYPE_CUBE (0) or TASK_TYPE_VECTOR (1). core: 0..block_dim-1. */
int esl_pick_phys_worker(int core, int exe_type);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_WORKER_MAP_H */
