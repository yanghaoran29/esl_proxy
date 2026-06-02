// Shared qwen3_decode static sub-graph building blocks.
//
// Both cases/qwen3_decode_static.h (manual dep_static + macro_group inter-group)
// and cases/qwen3_decode_tensormap_static.h (tensormap inter-group) build the
// exact same intra-group task graph. Everything that is identical between the two
// lives here and both cases include this header:
//   - the id layout (QW3_*) and the three task_group_tpl_t templates
//     (type/mode/duration/layout only — no dependency wiring),
//   - the intra-group dependency blobs (g1_dep/g2_dep/g3_dep),
//   - the rewire blobs (g1_rewire/g2_rewire/g3_rewire) + their ref enums,
//   - the qw3_g{1,2,3}_emit() helpers: claim + template apply + rewire +
//     intra-group dep install.
// Only the *inter-group* wiring differs and stays in each case file: static.h
// uses macro_group_bind/macro_succeed_build, tensormap_static.h registers each
// group's boundary outputs into the tensormap and looks up its cross-group
// inputs.
//
// Fixed shape: user_batch=90, tile_rows=16, batch_padded=96, num_tiles=6.
// Micro task ids (522 total):
//   prefill  [1..30]   6 tiles × 5 tasks (rmsnorm/q/k/v_proj/qk_norm)
//   attn     [31..480] 90 batches × 5 tasks (rope/qk_mm/softmax/sv_mm/online)
//   mlp      [481..522] 6 tiles × 7 tasks (out_proj/post_rms/gate/up/silu/down/down_res)
// Macro ids (102 total, static case only): G1 tile [1..6], G2 batch [7..96], G3 tile [97..102].
//
// Intra-group edges (consistent pred/succ, entry task intra-pred = 0):
//   G1 5/tile, G2 6/batch, G3 8/tile  ->  30 + 540 + 48 = 618 total.

#ifndef QW3_BUILD_STATIC_SUBGRAPH_H
#define QW3_BUILD_STATIC_SUBGRAPH_H

#include <stddef.h>
#include <stdint.h>

#include "orch_build.h"

/* Orchestration id layout (user_batch=90, batch_padded=96, num_tiles=6). */
enum {
    QW3_USER_BATCH    = 90,
    QW3_BATCH_PADDED  = 96,
    QW3_TILE_ROWS     = 16,
    QW3_G1_MACRO_BASE = 1,
    QW3_G1_TASK_BASE  = 1,
    QW3_G1_TASK_CNT   = 5,
    QW3_G2_MACRO_BASE = 7,
    QW3_G2_TASK_BASE  = 31,
    QW3_G2_TASK_CNT   = 5,
    QW3_G3_MACRO_BASE = 97,
    QW3_G3_TASK_BASE  = 481,
    QW3_G3_TASK_CNT   = 7,
};

/* ---- G1 prefill tile template (5 tasks per tile) ---- */
static task_group_tpl_t g_tpl_g1 = {
    .n_slots = QW3_G1_TASK_CNT,
    .slots = {
        /* [0] rmsnorm (AIV single) */
        [0] = {
            .type = TASK_TYPE_VECTOR,
            .tensor_cnt = 3,
            .scalar_cnt = 2,
            .duration = 23950,
        },
        /* [1] q_proj (AIC SPMD, block_num=20) */
        [1] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 20,
            .tensor_cnt = 3,
            .scalar_cnt = 1,
            .duration = 26060,
        },
        /* [2] k_proj (AIC SPMD, block_num=8) */
        [2] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 8,
            .tensor_cnt = 3,
            .scalar_cnt = 1,
            .duration = 18170,
        },
        /* [3] v_proj (AIC SPMD, block_num=8) */
        [3] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 8,
            .tensor_cnt = 3,
            .scalar_cnt = 1,
            .duration = 17890,
        },
        /* [4] qk_norm (AIV single) */
        [4] = {
            .type = TASK_TYPE_VECTOR,
            .tensor_cnt = 6,
            .scalar_cnt = 2,
            .duration = 13190,
        },
    },
};

/* ---- G2 attention batch template (5 tasks per batch) ---- */
static task_group_tpl_t g_tpl_g2 = {
    .n_slots = QW3_G2_TASK_CNT,
    .slots = {
        /* [0] rope_kv_cache (AIV single) */
        [0] = {
            .type = TASK_TYPE_VECTOR,
            .tensor_cnt = 10,
            .scalar_cnt = 3,
            .duration = 9480,
        },
        /* [1] qk_matmul (AIC SPMD, block_num=4) */
        [1] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 4,
            .tensor_cnt = 4,
            .scalar_cnt = 3,
            .duration = 29350,
        },
        /* [2] softmax (AIV SPMD, block_num=4) */
        [2] = {
            .type = TASK_TYPE_VECTOR,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 4,
            .tensor_cnt = 4,
            .scalar_cnt = 2,
            .duration = 19400,
        },
        /* [3] sv_matmul (AIC SPMD, block_num=4) */
        [3] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 4,
            .tensor_cnt = 4,
            .scalar_cnt = 2,
            .duration = 31650,
        },
        /* [4] online_softmax (AIV SPMD, block_num=4) */
        [4] = {
            .type = TASK_TYPE_VECTOR,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 4,
            .tensor_cnt = 4,
            .scalar_cnt = 1,
            .duration = 20820,
        },
    },
};

/* ---- G3 MLP tile template (7 tasks per tile) ---- */
static task_group_tpl_t g_tpl_g3 = {
    .n_slots = QW3_G3_TASK_CNT,
    .slots = {
        /* [0] out_proj_residual (MIX SPMD, block_num=40) */
        [0] = {
            .type = TASK_TYPE_MIX,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 40,
            .tensor_cnt = 5,
            .scalar_cnt = 2,
            .duration = 40750,
        },
        /* [1] post_rmsnorm (AIV single) */
        [1] = {
            .type = TASK_TYPE_VECTOR,
            .tensor_cnt = 3,
            .duration = 24390,
        },
        /* [2] gate_proj (AIC SPMD, block_num=34) */
        [2] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 34,
            .tensor_cnt = 3,
            .duration = (uint16_t)95700,
        },
        /* [3] up_proj (AIC SPMD, block_num=34) */
        [3] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 34,
            .tensor_cnt = 3,
            .duration = (uint16_t)97140,
        },
        /* [4] silu (AIV SPMD, block_num=34) */
        [4] = {
            .type = TASK_TYPE_VECTOR,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 34,
            .tensor_cnt = 3,
            .duration = 2820,
        },
        /* [5] down_proj (AIC SPMD, block_num=40) */
        [5] = {
            .type = TASK_TYPE_CUBE,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 40,
            .tensor_cnt = 3,
            .duration = (uint16_t)72220,
        },
        /* [6] down_proj_residual (AIV SPMD, block_num=40) */
        [6] = {
            .type = TASK_TYPE_VECTOR,
            .mode = ORG_MODE_SPMD_SYNC,
            .count = 40,
            .tensor_cnt = 3,
            .scalar_cnt = 2,
            .duration = 2590,
        },
    },
};

/* ===========================================================================
 * Intra-group dependency blobs.
 *
 * Consistent pred/succ: every edge appears once as a predecessor count on the
 * consumer and once in the producer's successor list. The group entry task
 * carries intra-pred 0; its cross-group predecessor count is supplied later by
 * each case (macro gate in static.h, tensormap lookups in tensormap_static.h).
 * ========================================================================== */

/* ---- G1 prefill tile: 5 intra edges ----
 * qk_norm reads only q_proj/k_proj (not v_proj), so it depends on q/k_proj only. */
static const dep_slot_tpl_t g1_dep[QW3_G1_TASK_CNT] = {
    {0, 3, {1, 2, 3, 0}},       /* [0] rmsnorm  -> q_proj, k_proj, v_proj */
    {1, 1, {4, 0, 0, 0}},       /* [1] q_proj   dep rmsnorm; -> qk_norm */
    {1, 1, {4, 0, 0, 0}},       /* [2] k_proj   dep rmsnorm; -> qk_norm */
    {1, 0, {0, 0, 0, 0}},       /* [3] v_proj   dep rmsnorm */
    {2, 0, {0, 0, 0, 0}},       /* [4] qk_norm  dep q_proj, k_proj */
};

/* ---- G2 attention batch: 6 intra edges ----
 * online_softmax reads all_oi (sv_matmul) and all_mi/all_li (softmax), so it
 * depends on both sv_matmul and softmax. */
static const dep_slot_tpl_t g2_dep[QW3_G2_TASK_CNT] = {
    {0, 2, {1, 3, 0, 0}},       /* [0] rope_kv_cache -> qk_matmul, sv_matmul */
    {1, 1, {2, 0, 0, 0}},       /* [1] qk_matmul     dep rope; -> softmax */
    {1, 2, {3, 4, 0, 0}},       /* [2] softmax       dep qk_matmul; -> sv_matmul, online_softmax */
    {2, 1, {4, 0, 0, 0}},       /* [3] sv_matmul     dep rope, softmax; -> online_softmax */
    {2, 0, {0, 0, 0, 0}},       /* [4] online_softmax dep sv_matmul, softmax */
};

/* ---- G3 MLP tile: 8 intra edges ---- */
static const dep_slot_tpl_t g3_dep[QW3_G3_TASK_CNT] = {
    {0, 2, {1, 6, 0, 0}},       /* [0] out_proj_residual -> post_rmsnorm, down_proj_residual */
    {1, 2, {2, 3, 0, 0}},       /* [1] post_rmsnorm      dep out_proj; -> gate_proj, up_proj */
    {1, 1, {4, 0, 0, 0}},       /* [2] gate_proj         dep post_rmsnorm; -> silu */
    {1, 1, {4, 0, 0, 0}},       /* [3] up_proj           dep post_rmsnorm; -> silu */
    {2, 1, {5, 0, 0, 0}},       /* [4] silu              dep gate, up; -> down_proj */
    {1, 1, {6, 0, 0, 0}},       /* [5] down_proj         dep silu; -> down_proj_residual */
    {2, 0, {0, 0, 0, 0}},       /* [6] down_proj_residual dep down_proj, out_proj */
};

/* ===========================================================================
 * Rewire blobs: static tensor/scalar wiring per apply instance. The ref indices
 * (G1T_.. tensors, G1S_.. scalars, and likewise for G2/G3) index into the
 * tensors[]/scalars[] arrays the caller builds for each group instance.
 * ========================================================================== */

/* ---- G1 rewire blob ---- */
enum {
    G1T_EXT_HIDDEN = 0,
    G1T_EXT_INPUT_RMS,
    G1T_EXT_WQ,
    G1T_EXT_WK,
    G1T_EXT_WV,
    G1T_EXT_Q_NW,
    G1T_EXT_K_NW,
    G1T_Q_PROJ,
    G1T_K_PROJ,
    G1T_V_PROJ,
    G1T_Q_PROJ_N,
    G1T_K_PROJ_N,
    G1T_NORMED,
    G1S_B0,
    G1S_CUR_VALID,
    G1S_ZERO,
};

static const rewire_op_t g1_rewire[] = {
    /* [0] rmsnorm */
    {0, RW_IN,    G1T_EXT_HIDDEN}, {0, RW_OUT,   G1T_NORMED},
    {0, RW_IN,    G1T_EXT_INPUT_RMS}, {0, RW_SCALAR, G1S_B0}, {0, RW_SCALAR, G1S_CUR_VALID},
    /* [1] q_proj */
    {1, RW_IN,    G1T_NORMED}, {1, RW_IN, G1T_EXT_WQ}, {1, RW_OUT, G1T_Q_PROJ}, {1, RW_SCALAR, G1S_B0},
    /* [2] k_proj */
    {2, RW_IN,    G1T_NORMED}, {2, RW_IN, G1T_EXT_WK}, {2, RW_OUT, G1T_K_PROJ}, {2, RW_SCALAR, G1S_B0},
    /* [3] v_proj */
    {3, RW_IN,    G1T_NORMED}, {3, RW_IN, G1T_EXT_WV}, {3, RW_OUT, G1T_V_PROJ}, {3, RW_SCALAR, G1S_B0},
    /* [4] qk_norm */
    {4, RW_OUT,   G1T_K_PROJ_N}, {4, RW_OUT, G1T_Q_PROJ_N}, {4, RW_IN, G1T_Q_PROJ},
    {4, RW_IN,    G1T_EXT_Q_NW}, {4, RW_IN, G1T_K_PROJ}, {4, RW_IN, G1T_EXT_K_NW},
    {4, RW_SCALAR, G1S_ZERO}, {4, RW_SCALAR, G1S_B0},
};

/* ---- G2 rewire blob ---- */
enum {
    G2T_ALL_Q = 0,
    G2T_EXT_K_CACHE,
    G2T_EXT_V_CACHE,
    G2T_K_PROJ_N,
    G2T_EXT_ROPE_COS,
    G2T_EXT_ROPE_SIN,
    G2T_V_PROJ,
    G2T_Q_PROJ_N,
    G2T_EXT_BLOCK_TABLE,
    G2T_ALL_RAW,
    G2T_ALL_EXP,
    G2T_ALL_MI,
    G2T_ALL_LI,
    G2T_ALL_OI,
    G2T_ATTN_OUT,
    G2S_SLOT_BLOCK,
    G2S_SLOT_OFFSET,
    G2S_B,
    G2S_CTX_BLOCKS,
    G2S_CTX_LEN,
    G2S_BLOCK_TABLE_BASE,
};

static const rewire_op_t g2_rewire[] = {
    /* [0] rope_kv_cache */
    {0, RW_OUT,   G2T_ALL_Q}, {0, RW_OUT, G2T_EXT_K_CACHE}, {0, RW_OUT, G2T_EXT_V_CACHE},
    {0, RW_IN,    G2T_K_PROJ_N}, {0, RW_IN, G2T_EXT_ROPE_COS}, {0, RW_IN, G2T_EXT_ROPE_SIN},
    {0, RW_IN,    G2T_EXT_ROPE_COS}, {0, RW_IN, G2T_EXT_ROPE_SIN}, {0, RW_IN, G2T_V_PROJ},
    {0, RW_IN,    G2T_Q_PROJ_N},
    {0, RW_SCALAR, G2S_SLOT_BLOCK}, {0, RW_SCALAR, G2S_SLOT_OFFSET}, {0, RW_SCALAR, G2S_B},
    /* [1] qk_matmul */
    {1, RW_IN,    G2T_ALL_Q}, {1, RW_OUT, G2T_ALL_RAW}, {1, RW_IN, G2T_EXT_BLOCK_TABLE},
    {1, RW_IN,    G2T_EXT_K_CACHE},
    {1, RW_SCALAR, G2S_B}, {1, RW_SCALAR, G2S_CTX_BLOCKS}, {1, RW_SCALAR, G2S_BLOCK_TABLE_BASE},
    /* [2] softmax */
    {2, RW_OUT,   G2T_ALL_LI}, {2, RW_OUT, G2T_ALL_MI}, {2, RW_OUT, G2T_ALL_EXP},
    {2, RW_IN,    G2T_ALL_RAW}, {2, RW_SCALAR, G2S_CTX_BLOCKS}, {2, RW_SCALAR, G2S_CTX_LEN},
    /* [3] sv_matmul */
    {3, RW_OUT,   G2T_ALL_OI}, {3, RW_IN, G2T_EXT_BLOCK_TABLE}, {3, RW_IN, G2T_ALL_EXP},
    {3, RW_IN,    G2T_EXT_V_CACHE}, {3, RW_SCALAR, G2S_CTX_BLOCKS}, {3, RW_SCALAR, G2S_BLOCK_TABLE_BASE},
    /* [4] online_softmax */
    {4, RW_IN,    G2T_ALL_OI}, {4, RW_IN, G2T_ALL_MI}, {4, RW_IN, G2T_ALL_LI},
    {4, RW_INOUT, G2T_ATTN_OUT}, {4, RW_SCALAR, G2S_CTX_BLOCKS},
};

/* ---- G3 rewire blob ---- */
enum {
    G3T_EXT_HIDDEN = 0,
    G3T_ATTN_OUT,
    G3T_EXT_WO,
    G3T_RESID1,
    G3T_GM_PIPE,
    G3T_POST_NORM,
    G3T_EXT_POST_RMS,
    G3T_EXT_W_GATE,
    G3T_GATE,
    G3T_EXT_W_UP,
    G3T_UP,
    G3T_MLP,
    G3T_EXT_W_DOWN,
    G3T_DOWN,
    G3T_EXT_OUT,
    G3S_B0,
    G3S_CUR_VALID,
};

static const rewire_op_t g3_rewire[] = {
    /* [0] out_proj_residual */
    {0, RW_IN,    G3T_EXT_HIDDEN}, {0, RW_IN, G3T_ATTN_OUT}, {0, RW_IN, G3T_EXT_WO},
    {0, RW_INOUT, G3T_RESID1}, {0, RW_OUT, G3T_GM_PIPE},
    {0, RW_SCALAR, G3S_B0}, {0, RW_SCALAR, G3S_CUR_VALID},
    /* [1] post_rmsnorm */
    {1, RW_IN,    G3T_RESID1}, {1, RW_OUT, G3T_POST_NORM}, {1, RW_IN, G3T_EXT_POST_RMS},
    /* [2] gate_proj */
    {2, RW_IN,    G3T_POST_NORM}, {2, RW_IN, G3T_EXT_W_GATE}, {2, RW_INOUT, G3T_GATE},
    /* [3] up_proj */
    {3, RW_IN,    G3T_POST_NORM}, {3, RW_IN, G3T_EXT_W_UP}, {3, RW_INOUT, G3T_UP},
    /* [4] silu */
    {4, RW_IN,    G3T_GATE}, {4, RW_IN, G3T_UP}, {4, RW_INOUT, G3T_MLP},
    /* [5] down_proj */
    {5, RW_IN,    G3T_MLP}, {5, RW_IN, G3T_EXT_W_DOWN}, {5, RW_INOUT, G3T_DOWN},
    /* [6] down_proj_residual */
    {6, RW_IN,    G3T_DOWN}, {6, RW_IN, G3T_RESID1}, {6, RW_OUT, G3T_EXT_OUT},
    {6, RW_SCALAR, G3S_CUR_VALID}, {6, RW_SCALAR, G3S_B0},
};

/* ===========================================================================
 * Intra-group emit: claim ids + apply template + rewire tensors/scalars +
 * install the intra-group dependency blob. Shared verbatim by both cases; the
 * inter-group wiring and submit/gate handling are added by each case afterward.
 * ========================================================================== */

static inline void qw3_g1_emit(uint16_t base, const uint64_t *tensors, const int64_t *scalars)
{
    task_claim_range(QW3_G1_TASK_CNT);
    task_group_tpl_apply(&g_tpl_g1, base);
    rewire_group_apply(base, g1_rewire, sizeof g1_rewire / sizeof g1_rewire[0], tensors, scalars);
    dep_group_install(base, g1_dep, QW3_G1_TASK_CNT);
}

static inline void qw3_g2_emit(uint16_t base, const uint64_t *tensors, const int64_t *scalars)
{
    task_claim_range(QW3_G2_TASK_CNT);
    task_group_tpl_apply(&g_tpl_g2, base);
    rewire_group_apply(base, g2_rewire, sizeof g2_rewire / sizeof g2_rewire[0], tensors, scalars);
    dep_group_install(base, g2_dep, QW3_G2_TASK_CNT);
}

static inline void qw3_g3_emit(uint16_t base, const uint64_t *tensors, const int64_t *scalars)
{
    task_claim_range(QW3_G3_TASK_CNT);
    task_group_tpl_apply(&g_tpl_g3, base);
    rewire_group_apply(base, g3_rewire, sizeof g3_rewire / sizeof g3_rewire[0], tensors, scalars);
    dep_group_install(base, g3_dep, QW3_G3_TASK_CNT);
}

#endif /* QW3_BUILD_STATIC_SUBGRAPH_H */
