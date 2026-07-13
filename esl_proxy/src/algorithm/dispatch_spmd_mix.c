/*
 * dispatch_spmd_mix.c - SPMD multi-block + MIX cluster dispatch state.
 */
#define _GNU_SOURCE

#include "dispatch_spmd_mix.h"

#include "executor.h"
#include "handshake.h"
#include "platform.h"
#include "platform_regs.h"
#include "queue.h"
#include "ring_buf.h"
#include "runtime.h"
#include "swimlane_aicpu.h"
#include "worker_map.h"

#include <stdatomic.h>
#include <stdint.h>

extern struct task_desc g_basic_buf[RING_SIZE];
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];
extern EslRuntime *g_runtime;

/* Per-task SPMD block cursors, ATOMIC so both dispatch lanes can claim disjoint
 * block ranges of the same wide task concurrently (a task now lives in the shared
 * ready pool, not pinned to one lane). Mirrors simpler's atomic next_block_idx. */
static _Atomic uint16_t g_next_block[RING_SIZE];
static _Atomic uint16_t g_finished_blocks[RING_SIZE];
static uint32_t g_slot_block_idx[EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_mix_active[AIC_CNT][AIC_OSTD];
static uint16_t g_mix_task[AIC_CNT][AIC_OSTD];

static uint64_t dispatch_mix_phys_reg_addr(int worker_id)
{
    uint64_t reg_addr = esl_handshake_reg_addr(worker_id);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return 0;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
}

static int dispatch_mix_cluster_subtasks_acked(int core, int slot)
{
    const int other = 1 - slot;
    const int phys_list[3] = {dispatch_mix_aic_phys(core), dispatch_mix_aiv0_phys(core),
                              dispatch_mix_aiv1_phys(core)};
    const int exe_list[3] = {TASK_TYPE_CUBE, TASK_TYPE_VECTOR, TASK_TYPE_VECTOR};
    const int slot_list[3] = {slot, slot, other};
    int p;

    for (p = 0; p < 3; p++) {
        const uint32_t reg_task = (uint32_t)g_executors[exe_list[p]][core].base[slot_list[p]];
        const uint64_t reg_addr = dispatch_mix_phys_reg_addr(phys_list[p]);

        if (reg_task == 0U || reg_addr == 0 ||
            !platform_reg_task_acked(reg_addr, reg_task)) {
            return 0;
        }
    }
    return 1;
}

static int dispatch_mix_cluster_pending(ctrl_t *ctrl, int core, int *out_busy_slot, int *out_free_slot)
{
    const uint64_t mask = (uint64_t)1 << core;

    for (int busy = 0; busy < AIC_OSTD; busy++) {
        const int free = 1 - busy;

        if (!g_mix_active[core][busy]) {
            continue;
        }
        if (g_executors[TASK_TYPE_CUBE][core].tasks[free] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[free] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[busy] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_CUBE][free] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][free] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][busy] & mask)) {
            continue;
        }
        if (out_busy_slot != NULL) {
            *out_busy_slot = busy;
        }
        if (out_free_slot != NULL) {
            *out_free_slot = free;
        }
        return 1;
    }
    return 0;
}

int dispatch_mix_core_busy(int core)
{
    for (int s = 0; s < AIC_OSTD; s++) {
        if (g_mix_active[core][s] != 0) {
            return 1;
        }
    }
    return 0;
}

/* Prepare one MIX cluster's up-to-3 subtask payloads (AIC+AIV0+AIV1), appending
 * handles+phys into pubs[]/phys_arr[] from *np. NO wmb/publish — dispatch_mix_flush
 * batches them so many clusters share one barrier (simpler flush_publish idiom).
 * Returns 0 (prepared, or fast-completed with no handles), -1 (fail; this
 * cluster's partial handles discarded, caller rolls back). */
int dispatch_mix_prepare_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx,
                                 EslPublishHandle pubs[], int phys_arr[], int *np)
{
    const int other = 1 - slot;
    const int phys_aic = dispatch_mix_aic_phys(core);
    const int phys_aiv0 = dispatch_mix_aiv0_phys(core);
    const int phys_aiv1 = dispatch_mix_aiv1_phys(core);

    (void)ctrl;
    g_executors[TASK_TYPE_CUBE][core].base[slot] = 0;
    g_executors[TASK_TYPE_VECTOR][core].base[slot] = 0;
    g_executors[TASK_TYPE_VECTOR][core].base[other] = 0;
    if (g_runtime != NULL && phys_aic >= g_runtime->worker_count) {
        const uint64_t mask = (uint64_t)1 << core;

        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_CUBE][slot] |= mask;
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][slot] |= mask;
        g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][other] |= mask;
        return 0; /* fast-complete: no handles */
    }
    {
        const int phys_list[3] = {phys_aic, phys_aiv0, phys_aiv1};
        const int exe_list[3] = {TASK_TYPE_CUBE, TASK_TYPE_VECTOR, TASK_TYPE_VECTOR};
        const int slot_list[3] = {slot, slot, other};
        const int start_np = *np;
        int p;

        for (p = 0; p < 3; p++) {
            const uint64_t reg_addr = dispatch_mix_phys_reg_addr(phys_list[p]);
            EslPublishHandle pub;

            if (reg_addr == 0) {
                *np = start_np;
                return -1;
            }
            pub = esl_prepare_subtask_to_core(g_runtime, phys_list[p], task_id, block_idx);
            if (pub.reg_task_id == 0U) {
                *np = start_np;
                return -1;
            }
            pub.reg_addr = reg_addr;
            g_executors[exe_list[p]][core].base[slot_list[p]] = pub.reg_task_id;
            pubs[*np] = pub;
            phys_arr[*np] = phys_list[p];
            (*np)++;
        }
        return 0;
    }
}

/* Batch-publish accumulated MIX handles: one wmb() then a doorbell per handle. */
void dispatch_mix_flush(EslPublishHandle pubs[], const int phys_arr[], int np)
{
    int i;

    if (np <= 0) {
        return;
    }
    wmb();
    for (i = 0; i < np; i++) {
        ESL_SWIMLANE_AICPU_ON_DISPATCH(phys_arr[i], ESL_AICPU_ROLE_DISPATCH);
        esl_publish_subtask_to_core(pubs[i]);
    }
}

/* Single-cluster publish (dispatch_mix_prefetch 2-outstanding filler). */
int dispatch_mix_publish_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id,
                                 uint32_t block_idx)
{
    EslPublishHandle pubs[3];
    int phys_arr[3];
    int np = 0;

    if (dispatch_mix_prepare_cluster(ctrl, core, slot, task_id, block_idx, pubs, phys_arr, &np) != 0) {
        return -1;
    }
    dispatch_mix_flush(pubs, phys_arr, np);
    return 0;
}

int dispatch_mix_prefetch(ctrl_t *ctrl)
{
    int sent = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        int slot;
        int busy_slot = -1;
        uint16_t one;
        uint16_t cnt1 = 1;
        uint32_t block_idx;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        if (dispatch_mix_cluster_idle(ctrl, core, &slot)) {
            (void)0;
        } else if (dispatch_mix_cluster_pending(ctrl, core, &busy_slot, &slot)) {
            if (!dispatch_mix_cluster_subtasks_acked(core, busy_slot)) {
                continue;
            }
        } else {
            continue;
        }
        if (!batch_dequeue(&g_shared_ready[TASK_TYPE_MIX], &one, &cnt1) || cnt1 < 1) {
            continue;
        }
        if (!dispatch_spmd_claim_block(one, &block_idx)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            continue;
        }
        dispatch_mix_occupy_cluster(ctrl, core, slot, one, block_idx);
        if (dispatch_mix_publish_cluster(ctrl, core, slot, one, block_idx) != 0) {
            dispatch_mix_release_cluster(ctrl, core, slot);
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
            continue;
        }
        if (dispatch_spmd_has_remaining(one)) {
            batch_enqueue(&g_shared_ready[TASK_TYPE_MIX], &one, 1);
        }
        sent++;
    }
    return sent;
}

static int dispatch_mix_cluster_all_done(ctrl_t *ctrl, int core, int slot)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;

    return (ctrl->msg_bitmap[TASK_TYPE_CUBE][slot] & mask) &&
           (ctrl->msg_bitmap[TASK_TYPE_VECTOR][slot] & mask) &&
           (ctrl->msg_bitmap[TASK_TYPE_VECTOR][other] & mask);
}

int dispatch_mix_defer_slot_clear(int exe_type, int core, int slot)
{
    if (exe_type == TASK_TYPE_CUBE) {
        return g_mix_active[core][slot] != 0;
    }
    for (int s = 0; s < AIC_OSTD; s++) {
        if (g_mix_active[core][s] != 0 && (slot == s || slot == (1 - s))) {
            return 1;
        }
    }
    return 0;
}

static int dispatch_mix_partial_pending(ctrl_t *ctrl, int exe_type, int core, int slot)
{
    if (exe_type == TASK_TYPE_CUBE) {
        if (!g_mix_active[core][slot]) {
            return 0;
        }
        return !dispatch_mix_cluster_all_done(ctrl, core, slot);
    }
    for (int s = 0; s < AIC_OSTD; s++) {
        if (!g_mix_active[core][s]) {
            continue;
        }
        if (slot == s || slot == (1 - s)) {
            return !dispatch_mix_cluster_all_done(ctrl, core, s);
        }
    }
    return 0;
}

void dispatch_merge_msg_to_free(ctrl_t *ctrl)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            uint64_t bitmap = ctrl->msg_bitmap[i][j];
            uint64_t safe = 0;

            while (bitmap != 0) {
                const int core = (int)__builtin_ctzll(bitmap);
                const uint64_t mask = (uint64_t)1 << core;

                if (!dispatch_mix_partial_pending(ctrl, i, core, j)) {
                    safe |= mask;
                }
                bitmap &= ~mask;
            }
            ctrl->free_bitmap[i][j] |= safe;
        }
    }
    for (int j = 0; j < AIC_OSTD; j++) {
        ctrl->free_bitmap[TASK_TYPE_MIX][j] =
            ctrl->free_bitmap[TASK_TYPE_CUBE][j] & ctrl->free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

__attribute__((weak)) void dispatch_spmd_on_ready(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;

    /* Reset happens-before the task is published to g_shared_ready (cutter's
     * batch_enqueue release); the claiming lane observes it via the dequeue
     * acquire. Fires once per ready transition; re-enqueues do not call this. */
    atomic_store_explicit(&g_next_block[slot], 0, memory_order_relaxed);
    atomic_store_explicit(&g_finished_blocks[slot], 0, memory_order_relaxed);
}

int dispatch_task_is_spmd(uint16_t task_id)
{
    return g_basic_buf[task_id & RING_MASK].count > 1U;
}

/* simpler dispatch_shape range-claim (scheduler_dispatch.cpp:645-660): CAS-claim
 * min(avail, remaining) blocks atomically so both lanes can claim disjoint ranges
 * of the same wide task concurrently. Advances the shared cursor by the claimed
 * count and returns that count + the first block index. count<=1 tasks claim
 * exactly one block once (avail-agnostic). */
int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    if (avail <= 0) {
        return 0;
    }
    uint16_t cur = atomic_load_explicit(&g_next_block[slot], memory_order_acquire);
    uint32_t start;
    uint32_t n;

    for (;;) {
        start = cur;
        if (total <= 1U) {
            if (start > 0U) {
                return 0;
            }
            n = 1U;
        } else {
            if (start >= total) {
                return 0;
            }
            const uint32_t remaining = total - start;
            n = ((uint32_t)avail < remaining) ? (uint32_t)avail : remaining;
        }
        if (atomic_compare_exchange_weak_explicit(&g_next_block[slot], &cur,
                                                  (uint16_t)(start + n), memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break; /* claimed [start, start+n); cur reloaded on failure, retry */
        }
    }
    if (start_block != NULL) {
        *start_block = start;
    }
    return (int)n;
}

/* Roll the claim cursor back after a partial-dispatch failure so blocks
 * claimed-but-not-published are re-claimed next round (else they never FIN).
 * CONDITIONAL CAS: only rewind if no peer lane advanced the cursor past our claim
 * (expected == start+n). If a peer already claimed beyond us, a blind store would
 * clobber its claim -> double-dispatch; leave the cursor and let the caller carry
 * the stranded blocks. In a correctly-configured run publish never fails, so this
 * is defensive; at 1 lane `expected` always holds -> equivalent to the old store. */
int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block)
{
    uint16_t expected = (uint16_t)claimed_end;

    return atomic_compare_exchange_strong_explicit(&g_next_block[task_id & RING_MASK],
                                                   &expected, (uint16_t)next_block,
                                                   memory_order_acq_rel, memory_order_relaxed)
               ? 1
               : 0;
}

/* One-block wrapper (prefetch / MIX single-cluster call sites). */
int dispatch_spmd_claim_block(uint16_t task_id, uint32_t *block_idx)
{
    uint32_t s = 0;
    int n = dispatch_spmd_claim_range(task_id, 1, &s);

    if (n > 0 && block_idx != NULL) {
        *block_idx = s;
    }
    return n > 0 ? 1 : 0;
}

int dispatch_spmd_has_remaining(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    if (total <= 1U) {
        return 0;
    }
    return atomic_load_explicit(&g_next_block[slot], memory_order_acquire) < total;
}

int dispatch_spmd_note_block_done(uint16_t task_id)
{
    const uint16_t slot = task_id & RING_MASK;
    const uint32_t total = g_basic_buf[slot].count;

    /* Each block completes on exactly one core owned by exactly one lane, so the
     * increments sum to `total`; the caller whose increment reaches total (== not
     * >=) observes completion exactly once — no double/lost complete across lanes. */
    uint16_t prev = atomic_fetch_add_explicit(&g_finished_blocks[slot], 1, memory_order_acq_rel);

    return (uint32_t)(prev + 1U) == total;
}

int dispatch_mix_aic_phys(int core)
{
    return core;
}

int dispatch_mix_aiv0_phys(int core)
{
    return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK;
}

int dispatch_mix_aiv1_phys(int core)
{
    return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK + 1;
}

int dispatch_mix_cluster_idle(ctrl_t *ctrl, int core, int *out_slot)
{
    const uint64_t mask = (uint64_t)1 << core;

    for (int s = 0; s < AIC_OSTD; s++) {
        const int other = 1 - s;

        if (g_executors[TASK_TYPE_CUBE][core].tasks[s] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[s] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (g_executors[TASK_TYPE_VECTOR][core].tasks[other] != EXEC_SLOT_EMPTY) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_CUBE][s] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][s] & mask)) {
            continue;
        }
        if (!(ctrl->free_bitmap[TASK_TYPE_VECTOR][other] & mask)) {
            continue;
        }
        if (out_slot != NULL) {
            *out_slot = s;
        }
        return 1;
    }
    return 0;
}

void dispatch_mix_occupy_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;
    const uint32_t dur = g_basic_buf[task_id & RING_MASK].duration;

    ctrl->free_bitmap[TASK_TYPE_CUBE][slot] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][slot] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][other] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_MIX][slot] &= ~mask;

    g_executors[TASK_TYPE_CUBE][core].tasks[slot] = task_id;
    g_executors[TASK_TYPE_CUBE][core].duration[slot] = dur;
    g_executors[TASK_TYPE_CUBE][core].block_idx[slot] = (uint16_t)dispatch_mix_aic_phys(core);
    g_slot_block_idx[TASK_TYPE_CUBE][core][slot] = block_idx;

    g_executors[TASK_TYPE_VECTOR][core].tasks[slot] = task_id;
    g_executors[TASK_TYPE_VECTOR][core].duration[slot] = dur;
    g_executors[TASK_TYPE_VECTOR][core].block_idx[slot] = (uint16_t)dispatch_mix_aiv0_phys(core);
    g_slot_block_idx[TASK_TYPE_VECTOR][core][slot] = block_idx;

    g_executors[TASK_TYPE_VECTOR][core].tasks[other] = task_id;
    g_executors[TASK_TYPE_VECTOR][core].duration[other] = dur;
    g_executors[TASK_TYPE_VECTOR][core].block_idx[other] = (uint16_t)dispatch_mix_aiv1_phys(core);
    g_slot_block_idx[TASK_TYPE_VECTOR][core][other] = block_idx;

    if (slot == 1) {
        ctrl->task_id_map2[TASK_TYPE_CUBE][core] = task_id;
        ctrl->task_id_map2[TASK_TYPE_VECTOR][core] = task_id;
    } else {
        ctrl->task_id_map1[TASK_TYPE_CUBE][core] = task_id;
        ctrl->task_id_map1[TASK_TYPE_VECTOR][core] = task_id;
    }

    g_mix_active[core][slot] = 1;
    g_mix_task[core][slot] = task_id;
}

void dispatch_mix_release_cluster(ctrl_t *ctrl, int core, int slot)
{
    const int other = 1 - slot;
    const uint64_t mask = (uint64_t)1 << core;

    g_mix_active[core][slot] = 0;
    g_mix_task[core][slot] = 0;

    g_executors[TASK_TYPE_CUBE][core].tasks[slot] = EXEC_SLOT_EMPTY;
    g_executors[TASK_TYPE_VECTOR][core].tasks[slot] = EXEC_SLOT_EMPTY;
    g_executors[TASK_TYPE_VECTOR][core].tasks[other] = EXEC_SLOT_EMPTY;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_CUBE][slot] &= ~mask;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][slot] &= ~mask;
    g_ctrl_t[CORE_LANE(core)].msg_bitmap[TASK_TYPE_VECTOR][other] &= ~mask;
    ctrl->free_bitmap[TASK_TYPE_CUBE][slot] |= mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][slot] |= mask;
    ctrl->free_bitmap[TASK_TYPE_VECTOR][other] |= mask;
    ctrl->free_bitmap[TASK_TYPE_MIX][slot] |= mask;
}

int dispatch_mix_harvest_completed(ctrl_t *ctrl, uint16_t done_tasks[], int max_out)
{
    int out = 0;

    for (int core = 0; core < AIC_CNT; core++) {
        const uint64_t mask = (uint64_t)1 << core;

        if (CORE_LANE(core) != ctrl->tid) {
            continue;
        }
        for (int s = 0; s < AIC_OSTD; s++) {
            uint16_t task_id;

            if (!g_mix_active[core][s]) {
                continue;
            }
            task_id = g_mix_task[core][s];
            if (!dispatch_mix_cluster_all_done(ctrl, core, s)) {
                continue;
            }
            dispatch_mix_release_cluster(ctrl, core, s);
            if (dispatch_spmd_note_block_done(task_id)) {
                if (out < max_out) {
                    done_tasks[out++] = task_id;
                }
            }
        }
    }
    return out;
}

int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out)
{
    int out = dispatch_mix_harvest_completed(ctrl, out_tasks, max_out);
    int i;

    for (i = 0; i < EXE_TYPE_CNT; i++) {
        uint64_t bitmap0 = ctrl->msg_bitmap[i][0];
        uint64_t bitmap1 = ctrl->msg_bitmap[i][1];
        uint64_t keep0 = ctrl->msg_bitmap[i][0];
        uint64_t keep1 = ctrl->msg_bitmap[i][1];

        while (bitmap0 != 0) {
            const int core = (int)__builtin_ctzll(bitmap0);
            const uint64_t mask = (uint64_t)1 << core;
            uint16_t tid_done = ctrl->task_id_map1[i][core];

            bitmap0 &= ~mask;
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX ||
                dispatch_mix_partial_pending(ctrl, i, core, 0)) {
                continue;
            }
            keep0 &= ~mask;
            if (dispatch_spmd_note_block_done(tid_done)) {
                if (out < max_out) {
                    out_tasks[out++] = tid_done;
                }
            }
        }
        while (bitmap1 != 0) {
            const int core = (int)__builtin_ctzll(bitmap1);
            const uint64_t mask = (uint64_t)1 << core;
            uint16_t tid_done = ctrl->task_id_map2[i][core];

            bitmap1 &= ~mask;
            if (g_basic_buf[tid_done & RING_MASK].type == TASK_TYPE_MIX ||
                dispatch_mix_partial_pending(ctrl, i, core, 1)) {
                continue;
            }
            keep1 &= ~mask;
            if (dispatch_spmd_note_block_done(tid_done)) {
                if (out < max_out) {
                    out_tasks[out++] = tid_done;
                }
            }
        }
        ctrl->msg_bitmap[i][0] = keep0;
        ctrl->msg_bitmap[i][1] = keep1;
    }
    return out;
}
