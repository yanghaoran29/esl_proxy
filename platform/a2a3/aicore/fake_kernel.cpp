#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

#include <stdint.h>

extern "C" __global__ __aicore__ void fake_kernel(__gm__ int64_t *args)
{
    if (args == nullptr) {
        return;
    }
    uint64_t duration = (uint64_t)args[0];
    uint64_t mask = (uint64_t)args[1];
    duration += mask & 0x1Fu;

    uint64_t start;
    asm volatile("MOV %0, SYS_CNT\n" : "+l"(start));

    for (;;) {
        uint64_t now;
        asm volatile("MOV %0, SYS_CNT\n" : "+l"(now));
        if (now - start >= duration) {
            break;
        }
    }
}
