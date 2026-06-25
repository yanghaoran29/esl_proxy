/*
 * L2 swimlane onboard hooks for AICPU (.c) — composite macros only.
 * Call sites include this header; ESL_PROXY_ENABLE_L2_SWIMLANE stays here.
 */
#ifndef ESL_PROXY_ESL_SWIMLANE_AICPU_ONBOARD_H
#define ESL_PROXY_ESL_SWIMLANE_AICPU_ONBOARD_H

#include "aicpu_bridge.h"
#include "conf.h"
#include "l2_swimlane/esl_swimlane_aicpu_c.h"
#include "onboard_config.h"

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#define ESL_SWIMLANE_AICPU_DISPATCH_TS_STORAGE \
    static uint64_t g_hw_dispatch_ts[EXE_TYPE_CNT][AIC_CNT][AIC_OSTD]

#define ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(bridge) \
    do { \
        AicoreBridge *_esl_sw_bridge = (bridge); \
        if (_esl_sw_bridge != NULL && _esl_sw_bridge->runtime != NULL) { \
            int _esl_sw_n = _esl_sw_bridge->runtime->worker_count; \
            int _esl_sw_cores[RUNTIME_MAX_WORKER]; \
            int _esl_sw_i; \
            if (_esl_sw_n > RUNTIME_MAX_WORKER) { \
                _esl_sw_n = RUNTIME_MAX_WORKER; \
            } \
            for (_esl_sw_i = 0; _esl_sw_i < _esl_sw_n; ++_esl_sw_i) { \
                _esl_sw_cores[_esl_sw_i] = _esl_sw_i; \
            } \
            ESL_SWIMLANE_AICPU_FLUSH(ESL_AICPU_ROLE_DISPATCH, _esl_sw_cores, _esl_sw_n); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_AICPU_DISPATCH_TS_STORAGE
#define ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(bridge) ((void)(bridge))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_ESL_SWIMLANE_AICPU_ONBOARD_H */
