/*
 * runtime_setup.c — host-side EslRuntime initialization (sim mirrors onboard layout).
 */
#include "aicore_bridge.h"

#include "conf.h"
#include "platform.h"

#include <stddef.h>
#include <string.h>

void esl_runtime_setup_host(EslRuntime *rt, EslFakeDispatchPayload *payload, int worker_count)
{
    int i;

    if (rt == NULL || payload == NULL || worker_count <= 0 ||
        worker_count > RUNTIME_MAX_WORKER) {
        return;
    }

    memset(rt, 0, sizeof(*rt));
    rt->worker_count = worker_count;
    rt->aicpu_thread_num = CUTTER_THREAD_CNT + DISPATCH_THREAD_CNT + 1;

    for (i = 0; i < worker_count; ++i) {
        rt->workers[i].task =
            (uint64_t)(uintptr_t)(payload + (size_t)i * 2U);
        rt->workers[i].core_type = (i < AIC_CNT) ? 0 : 1;
    }
}
