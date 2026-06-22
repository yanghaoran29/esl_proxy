#ifndef ESL_ONBOARD_SHM_SYNC_H
#define ESL_ONBOARD_SHM_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Host DMA / cross-AICPU-core visibility for esl_proxy globals in .bss */
void esl_onboard_invalidate_runtime(void *runtime);
void esl_onboard_flush_shared_after_orch(void);
void esl_onboard_invalidate_shared_before_worker(void);
void esl_onboard_flush_after_cutter(void);
void esl_onboard_flush_after_dispatch(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_ONBOARD_SHM_SYNC_H */
