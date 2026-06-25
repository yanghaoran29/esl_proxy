/*
 * worker_map.c — Host + onboard worker index mapping.
 */
#include "worker_map.h"

#include <stdatomic.h>

static _Atomic uint32_t g_aiv_lane_pick[ESL_PROXY_WORKER_BLOCK_DIM];

int esl_pick_phys_worker(int core, int exe_type)
{
    if (core < 0 || core >= ESL_PROXY_WORKER_BLOCK_DIM) {
        return -1;
    }
    if (exe_type == 0) {
        return core;
    }
    {
        const uint32_t lane =
            atomic_fetch_add_explicit(&g_aiv_lane_pick[core], 1U, memory_order_relaxed) %
            (uint32_t)ESL_PROXY_AIV_LANES_PER_BLOCK;
        return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK + (int)lane;
    }
}
