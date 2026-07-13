/*
 * dispatch_spmd_mix.h - SPMD multi-block + MIX cluster dispatch helpers.
 *
 * Mirrors simpler claim/re-push (next_block_idx) and MIX cluster publish
 * (AIC + AIV0 + AIV1 per block) for esl_proxy's single dispatch thread.
 */
#ifndef DISPATCH_SPMD_MIX_H
#define DISPATCH_SPMD_MIX_H

#include <stdint.h>

#include "dispatch.h"
#include "runtime.h"
#include "task.h"

void dispatch_spmd_on_ready(uint16_t task_id);

int dispatch_spmd_claim_block(uint16_t task_id, uint32_t *block_idx);

/* Range claim: reserve min(avail, remaining blocks) blocks in one pop; returns
 * the claimed count (0 if exhausted) and writes the first block index. */
int dispatch_spmd_claim_range(uint16_t task_id, int avail, uint32_t *start_block);

/* Rewind the claim cursor from `claimed_end` back to `next_block` after a
 * partial-dispatch failure. Conditional CAS: returns 1 if rewound, 0 if a peer
 * lane already advanced the cursor past claimed_end (blocks then stranded — only
 * reachable on a permanent publish misconfig, which hangs regardless). */
int dispatch_spmd_rewind(uint16_t task_id, uint32_t claimed_end, uint32_t next_block);

int dispatch_spmd_has_remaining(uint16_t task_id);

/* One logical block finished (all MIX subtasks done when applicable). */
int dispatch_spmd_note_block_done(uint16_t task_id);

int dispatch_task_is_spmd(uint16_t task_id);

int dispatch_mix_cluster_idle(ctrl_t *ctrl, int core, int *out_slot);

int dispatch_mix_aic_phys(int core);
int dispatch_mix_aiv0_phys(int core);
int dispatch_mix_aiv1_phys(int core);

void dispatch_mix_occupy_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx);

void dispatch_mix_release_cluster(ctrl_t *ctrl, int core, int slot);

/* Merge msg_bitmap into free_bitmap; hold partial MIX clusters until harvest. */
void dispatch_merge_msg_to_free(ctrl_t *ctrl);

/* 1 if slot clear should wait for full MIX cluster harvest. */
int dispatch_mix_defer_slot_clear(int exe_type, int core, int slot);

/* Returns task ids whose full SPMD/MIX block set finished. */
int dispatch_push_completed_slots(ctrl_t *ctrl, uint16_t out_tasks[], int max_out);

/* Publish one MIX block to AIC+AIV0+AIV1; 0 ok, -1 rollback needed. */
int dispatch_mix_publish_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx);

/* Prepare one MIX cluster (append <=3 handles+phys from *np, no wmb/publish);
 * dispatch_mix_flush batch-publishes them (prepare-all / 1-wmb / publish-all). */
int dispatch_mix_prepare_cluster(ctrl_t *ctrl, int core, int slot, uint16_t task_id, uint32_t block_idx,
                                 EslPublishHandle pubs[], int phys_arr[], int *np);
void dispatch_mix_flush(EslPublishHandle pubs[], const int phys_arr[], int np);

/* Second-pass MIX dispatch: dequeue fail uses continue so idle cores still get work. */
int dispatch_mix_prefetch(ctrl_t *ctrl);

/* 1 if logical core holds an in-flight MIX cluster (any slot). */
int dispatch_mix_core_busy(int core);

#endif /* DISPATCH_SPMD_MIX_H */
