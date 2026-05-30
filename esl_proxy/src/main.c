#include <pthread.h>
#include <stdint.h>

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "manager.h"
#include "qwen3_decode.h"

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t cutter_threads[CUTTER_THREAD_CNT];
    pthread_t manager_thread;

    pthread_create(&manager_thread, NULL, manager_worker, &g_mem_pool);

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker,
                       (void *)(intptr_t)i);
    }

    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_create(&cutter_threads[i], NULL, cutter_worker,
                       (void *)(intptr_t)i);
    }

    aicpu_orchestration_entry(0);
    return 0;
}
