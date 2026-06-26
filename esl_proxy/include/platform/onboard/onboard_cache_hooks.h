/*
 * Onboard cache (dcci) hooks (platform layer).
 *
 * Backend-specific cache primitives — analogous to simpler's
 * cache_invalidate_range / cache_flush_range in the platform HAL. Onboard-only
 * (no sim definition). Included by onboard .c files that flush/invalidate the
 * AICPU↔AICore shared state. Must NOT include any algorithm header.
 */
#ifndef ESL_PROXY_PLATFORM_ONBOARD_CACHE_HOOKS_H
#define ESL_PROXY_PLATFORM_ONBOARD_CACHE_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

void esl_onboard_invalidate_runtime(void *runtime);
void esl_onboard_flush_shared_after_orch(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_ONBOARD_CACHE_HOOKS_H */
