#ifndef ESL_ONBOARD_SYNC_H
#define ESL_ONBOARD_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Cutter/dispatch call at loop entry; orch waits before creating tasks. */
void esl_onboard_worker_enter(void);
void esl_onboard_wait_workers(int required);
void esl_onboard_flush_handshake_flags(void);
void esl_onboard_invalidate_handshake_flags(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_ONBOARD_SYNC_H */
