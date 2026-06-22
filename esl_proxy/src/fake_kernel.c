#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

// ns
inline __aicore__ uint64_t get_syscnt() {
    uint64_t syscnt;
    asm volatile("MOV %0, SYS_CNT\n" : "+l"(syscnt));
    return syscnt;
}

extern "C" __global__ __aicore__ void fake_kernel(uint64_t duration) {
    uint64_t start = get_syscnt();
    duration = duration + start & 0x1F; // 增加kernel执行时间的随机性
    while (get_syscnt() < duration) {
    }
}