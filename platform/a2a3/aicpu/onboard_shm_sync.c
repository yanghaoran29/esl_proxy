/*
 * Cache maintenance for esl_proxy globals shared across AICPU cores.
 */
#include "onboard_shm_sync.h"

#include "conf.h"
#include "dispatch.h"
#include "esl_runtime.h"
#include "executor.h"
#include "ring_buf.h"
#include "task.h"

extern task_state *g_state_buf;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern atomic_bool g_is_done;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

static void cache_line_flush(const void *addr, size_t size)
{
    if (size == 0) {
        return;
    }
    const size_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end = ((uintptr_t)addr + size + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line) {
        __asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

static void cache_line_invalidate(const void *addr, size_t size)
{
    if (size == 0) {
        return;
    }
    const size_t line = 64;
    uintptr_t start = (uintptr_t)addr & ~(line - 1);
    uintptr_t end = ((uintptr_t)addr + size + line - 1) & ~(line - 1);
    for (uintptr_t p = start; p < end; p += line) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

void esl_onboard_invalidate_runtime(void *runtime)
{
    if (runtime != NULL) {
        cache_line_invalidate(runtime, sizeof(EslRuntime));
    }
}

void esl_onboard_flush_shared_after_orch(void)
{
    cache_line_flush(&g_task_id, sizeof(g_task_id));
    cache_line_flush(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_line_flush(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_line_flush(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_line_flush(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
}

void esl_onboard_invalidate_shared_before_worker(void)
{
    cache_line_invalidate(&g_task_id, sizeof(g_task_id));
    cache_line_invalidate(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_line_invalidate(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_line_invalidate(&g_is_done, sizeof(g_is_done));
    cache_line_invalidate(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_line_invalidate(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_line_invalidate(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
    cache_line_invalidate(g_ctrl_t, sizeof(g_ctrl_t));
}

void esl_onboard_flush_after_cutter(void)
{
    cache_line_flush(g_ctrl_t, sizeof(g_ctrl_t));
    cache_line_flush(&g_completed_cnt, sizeof(g_completed_cnt));
}

void esl_onboard_flush_after_dispatch(void)
{
    cache_line_flush(g_ctrl_t, sizeof(g_ctrl_t));
    cache_line_flush(&g_completed_cnt, sizeof(g_completed_cnt));
}
