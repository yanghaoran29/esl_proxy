/*
 * esl_proxy AICore executor — no-op when AICore kernel is not launched.
 */
#include "aicore/aicore.h"
#include "runtime.h"

__aicore__ __attribute__((weak)) void aicore_execute(__gm__ Runtime *runtime, int block_idx, CoreType core_type)
{
    (void)runtime;
    (void)block_idx;
    (void)core_type;
}
