/*
 * dump_edges_main.c — dump the micro-level dependency edge set a case builds.
 *
 * Replaces main.c: runs the real orchestration build for ORCH_CASE (using the
 * real ring_buf / orch_build / macro_group / dep install), then prints every
 * resolved (consumer producer) micro edge to stdout. The static case carries its
 * inter-group edges at the macro level, so those are expanded to micro edges via
 * the entry/exit micro bindings. Pipe through `sort -u` to get the edge SET.
 *
 * Build (per case), e.g.:
 *   cc -std=c11 -O2 -I include -I cases -DORCH_CASE='"qwen3_decode.h"' \
 *      tests/dump_edges_main.c src/shm.c src/orch_build.c -o /tmp/dump_manual -lpthread -latomic
 */

#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

#include "conf.h"
#include "macro_group.h"
#include "mem_pool.h"
#include "orch_build.h"
#include "ring_buf.h"

#ifndef ORCH_CASE
#define ORCH_CASE "qwen3_decode.h"
#endif
#include ORCH_CASE

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

static void dump_micro_edges(void)
{
    for (uint16_t p = 1; p <= g_task_id; p++) {
        task_state st = atomic_load_explicit(&g_state_buf[p & RING_MASK],
                                             memory_order_relaxed);
        uint16_t cnt = (uint16_t)st.successor_cnt;
        struct succ_list *ptr = &g_successor_buf[p & RING_MASK];
        int idx = 0;
        for (uint16_t j = 0; j < cnt; j++) {
            while (idx >= SUCC_NODE_CNT) { idx -= SUCC_NODE_CNT; ptr = ptr->next; }
            printf("%u %u\n", (unsigned)ptr->successor[idx], (unsigned)p);
            idx++;
        }
    }
}

static void dump_macro_expanded_edges(void)
{
    /* exit micro of each macro: the micro whose completion fires the macro. */
    static uint16_t exit_of[MACRO_RING_SIZE];
    for (uint16_t m = 1; m <= g_task_id; m++) {
        uint16_t mac = g_micro_exit_to_macro[m & RING_MASK];
        if (mac != 0) exit_of[mac & MACRO_RING_MASK] = m;
    }
    for (uint16_t mp = 0; mp < MACRO_RING_SIZE; mp++) {
        task_state st = g_macro_state_buf[mp];
        uint16_t cnt = (uint16_t)st.successor_cnt;
        struct succ_list *ptr = &g_macro_successor_buf[mp];
        int idx = 0;
        for (uint16_t j = 0; j < cnt; j++) {
            while (idx >= SUCC_NODE_CNT) { idx -= SUCC_NODE_CNT; ptr = ptr->next; }
            uint16_t mc = ptr->successor[idx];
            uint16_t entry = g_macro_entry_micro[mc & MACRO_RING_MASK];
            uint16_t exitm = exit_of[mp & MACRO_RING_MASK];
            if (entry != 0 && exitm != 0)
                printf("%u %u\n", (unsigned)entry, (unsigned)exitm);
            idx++;
        }
    }
}

int main(void)
{
    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    macro_ring_init();
#ifdef USE_TENSORMAP
    /* the tensormap cases call tm_deps_init() themselves inside the entry */
#endif
    orch_build_begin();
    aicpu_orchestration_entry(0);
#if defined(USE_TENSORMAP) && defined(TM_ASYNC)
    tm_async_finish(); /* drain the TensorMap thread before reading edges */
#endif
    orch_build_end();

    dump_micro_edges();
    dump_macro_expanded_edges();
    fprintf(stderr, "[dump] tasks=%u\n", (unsigned)g_task_id);
    return 0;
}
