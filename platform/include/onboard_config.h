#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#define ESL_PROXY_ONBOARD_CONFIG_H

/* AICPU logical threads: cutter(0), dispatch(1), orchestrator(2) */
#define ESL_PROXY_AICPU_THREAD_NUM 3

/* 0: cutter / dispatch / orch on three CANN AICPU threads */
#define ESL_PROXY_ONBOARD_SINGLE_AICPU 0

/* Simulated AICore count (register blocks allocated on AICPU) */
#define ESL_PROXY_FAKE_AICORE_COUNT 24

/* Host block_dim / runtime.worker_count for AICPU-only bring-up */
#define ESL_PROXY_ONBOARD_BLOCK_DIM ESL_PROXY_FAKE_AICORE_COUNT

#endif /* ESL_PROXY_ONBOARD_CONFIG_H */
