// Orchestration: qwen3_decode (task-group template + static intra deps + macro inter-group).
//
// Fixed shape: user_batch=90, tile_rows=16, batch_padded=96, num_tiles=6.
// Micro task ids (522 total):
//   prefill  [1..30]   6 tiles × 5 tasks (rmsnorm/q/k/v_proj/qk_norm)
//   attn     [31..480] 90 batches × 5 tasks (rope/qk_mm/softmax/sv_mm/online)
//   mlp      [481..522] 6 tiles × 7 tasks (out_proj/post_rms/gate/up/silu/down/down_res)
// Macro ids (102 total): G1 tile [1..6], G2 batch [7..96], G3 tile [97..102].
//
// Design:
//   1. The id layout, the g_tpl_g1/g2/g3 templates, the intra-group dep blobs
//      (g1_dep/g2_dep/g3_dep), the rewire blobs, and the qw3_g{1,2,3}_emit()
//      helpers are all shared with cases/qwen3_decode_tensormap_static.h via
//      build_static_subgraph.h.
//   2. apply_*(): build tensors[]/scalars[], qw3_g*_emit() (claim + tpl_apply +
//      rewire + intra dep install), then macro_group_bind + macro gate + defer
//      submit. The entry task's cross-group predecessor is the macro gate (=1),
//      installed AFTER the dep blob so it overrides the intra-pred of 0.
//   3. Inter-group macro_succeed() before apply; orch_build_flush() after phases.

#include <stddef.h>
#include <stdint.h>

#include "build_static_subgraph.h"
#include "macro_group.h"
#include "mem_pool.h"
#include "orch_build.h"
#include "ring_buf.h"

static void apply_qw3_g1_group(uint16_t base, size_t tix,
                               Tensor ext_hidden_states, Tensor ext_input_rms_weight,
                               Tensor ext_wq, Tensor ext_wk, Tensor ext_wv,
                               Tensor ext_q_norm_weight, Tensor ext_k_norm_weight,
                               Tensor q_proj, Tensor k_proj, Tensor v_proj,
                               Tensor q_proj_norm, Tensor k_proj_norm,
                               Tensor normed_tile, int64_t b0, int64_t cur_valid)
{
    const uint64_t tensors[] = {
        [G1T_EXT_HIDDEN]    = ext_hidden_states,
        [G1T_EXT_INPUT_RMS] = ext_input_rms_weight,
        [G1T_EXT_WQ]        = ext_wq,
        [G1T_EXT_WK]        = ext_wk,
        [G1T_EXT_WV]        = ext_wv,
        [G1T_EXT_Q_NW]      = ext_q_norm_weight,
        [G1T_EXT_K_NW]      = ext_k_norm_weight,
        [G1T_Q_PROJ]        = q_proj,
        [G1T_K_PROJ]        = k_proj,
        [G1T_V_PROJ]        = v_proj,
        [G1T_Q_PROJ_N]      = q_proj_norm,
        [G1T_K_PROJ_N]      = k_proj_norm,
        [G1T_NORMED]        = normed_tile,
    };
    const int64_t scalars[] = {
        [G1S_B0]        = b0,
        [G1S_CUR_VALID] = cur_valid,
        [G1S_ZERO]      = 0,
    };

    qw3_g1_emit(base, tensors, scalars);

    macro_group_bind((uint16_t)(QW3_G1_MACRO_BASE + tix), (uint16_t)(base + 0), (uint16_t)(base + 4));
    {
        const uint16_t ss[3] = {
            (uint16_t)(base + 1),
            (uint16_t)(base + 2),
            (uint16_t)(base + 3),
        };
        orch_defer_batch_submit(3, ss);
    }
    orch_defer_submit((uint16_t)(base + 4));
    orch_defer_root((uint16_t)(base + 0));
}

static void apply_qw3_g2_group(uint16_t base, int64_t b,
                               Tensor all_q_padded, Tensor ext_k_cache, Tensor ext_v_cache,
                               Tensor k_proj_norm, Tensor ext_rope_cos, Tensor ext_rope_sin,
                               Tensor v_proj, Tensor q_proj_norm, Tensor ext_block_table,
                               Tensor all_raw_scores, Tensor all_exp_padded,
                               Tensor all_cur_mi, Tensor all_cur_li,
                               Tensor all_oi_tmp, Tensor attn_out,
                               int64_t ctx_blocks, int64_t ctx_len,
                               int64_t block_table_base, int64_t slot_block,
                               int64_t slot_offset)
{
    const uint64_t tensors[] = {
        [G2T_ALL_Q]           = all_q_padded,
        [G2T_EXT_K_CACHE]     = ext_k_cache,
        [G2T_EXT_V_CACHE]     = ext_v_cache,
        [G2T_K_PROJ_N]        = k_proj_norm,
        [G2T_EXT_ROPE_COS]    = ext_rope_cos,
        [G2T_EXT_ROPE_SIN]    = ext_rope_sin,
        [G2T_V_PROJ]          = v_proj,
        [G2T_Q_PROJ_N]        = q_proj_norm,
        [G2T_EXT_BLOCK_TABLE] = ext_block_table,
        [G2T_ALL_RAW]         = all_raw_scores,
        [G2T_ALL_EXP]         = all_exp_padded,
        [G2T_ALL_MI]          = all_cur_mi,
        [G2T_ALL_LI]          = all_cur_li,
        [G2T_ALL_OI]          = all_oi_tmp,
        [G2T_ATTN_OUT]        = attn_out,
    };
    const int64_t scalars[] = {
        [G2S_SLOT_BLOCK]      = slot_block,
        [G2S_SLOT_OFFSET]     = slot_offset,
        [G2S_B]               = b,
        [G2S_CTX_BLOCKS]      = ctx_blocks,
        [G2S_CTX_LEN]         = ctx_len,
        [G2S_BLOCK_TABLE_BASE] = block_table_base,
    };

    qw3_g2_emit(base, tensors, scalars);

    macro_group_bind((uint16_t)(QW3_G2_MACRO_BASE + b), (uint16_t)(base + 0), (uint16_t)(base + 4));
    /* Gate the entry micro AFTER the dep blob so it overrides the intra-pred 0. */
    macro_gate_micro_build((uint16_t)(base + 0), 1);
    /* rope reads v_proj of its tile — add the direct edge (matches the tensormap
     * cases). It is redundant with the macro G2<-G1 edge but keeps the explicit
     * dependency set consistent across all four schemes. */
    dep_install((uint16_t)(base + 0),
                (uint16_t)(QW3_G1_TASK_BASE + (b / QW3_TILE_ROWS) * QW3_G1_TASK_CNT + 3));
    {
        const uint16_t ss[4] = {
            (uint16_t)(base + 1),
            (uint16_t)(base + 2),
            (uint16_t)(base + 3),
            (uint16_t)(base + 4),
        };
        orch_defer_batch_submit(4, ss);
    }
}

static void apply_qw3_g3_group(uint16_t base, size_t tix, int64_t b0, int64_t cur_valid,
                               Tensor ext_hidden_states, Tensor attn_out, Tensor ext_wo,
                               Tensor ext_post_rms_weight, Tensor ext_w_gate, Tensor ext_w_up,
                               Tensor ext_w_down, Tensor ext_out,
                               Tensor resid1_tile, Tensor gm_pipe_buffer_0,
                               Tensor post_norm_tile, Tensor gate_tile, Tensor up_tile,
                               Tensor mlp_tile, Tensor down_tile)
{
    const uint64_t tensors[] = {
        [G3T_EXT_HIDDEN]    = ext_hidden_states,
        [G3T_ATTN_OUT]      = attn_out,
        [G3T_EXT_WO]        = ext_wo,
        [G3T_RESID1]        = resid1_tile,
        [G3T_GM_PIPE]       = gm_pipe_buffer_0,
        [G3T_POST_NORM]     = post_norm_tile,
        [G3T_EXT_POST_RMS]  = ext_post_rms_weight,
        [G3T_EXT_W_GATE]    = ext_w_gate,
        [G3T_GATE]          = gate_tile,
        [G3T_EXT_W_UP]      = ext_w_up,
        [G3T_UP]            = up_tile,
        [G3T_MLP]           = mlp_tile,
        [G3T_EXT_W_DOWN]    = ext_w_down,
        [G3T_DOWN]          = down_tile,
        [G3T_EXT_OUT]       = ext_out,
    };
    const int64_t scalars[] = {
        [G3S_B0]        = b0,
        [G3S_CUR_VALID] = cur_valid,
    };

    qw3_g3_emit(base, tensors, scalars);

    macro_group_bind((uint16_t)(QW3_G3_MACRO_BASE + tix), (uint16_t)(base + 0), (uint16_t)(base + 6));
    /* Gate the entry micro AFTER the dep blob so it overrides the intra-pred 0. */
    macro_gate_micro_build((uint16_t)(base + 0), 1);
    {
        const uint16_t ss[6] = {
            (uint16_t)(base + 1),
            (uint16_t)(base + 2),
            (uint16_t)(base + 3),
            (uint16_t)(base + 4),
            (uint16_t)(base + 5),
            (uint16_t)(base + 6),
        };
        orch_defer_batch_submit(6, ss);
    }
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    Tensor ext_hidden_states = orch_args + 0;
    Tensor ext_input_rms_weight = orch_args + 1;
    Tensor ext_wq = orch_args + 2;
    Tensor ext_wk = orch_args + 3;
    Tensor ext_wv = orch_args + 4;
    Tensor ext_q_norm_weight = orch_args + 5;
    Tensor ext_k_norm_weight = orch_args + 6;
    Tensor ext_seq_lens = orch_args + 7;
    Tensor ext_block_table = orch_args + 8;
    Tensor ext_slot_mapping = orch_args + 9;
    Tensor ext_rope_cos = orch_args + 10;
    Tensor ext_rope_sin = orch_args + 11;
    Tensor ext_k_cache = orch_args + 12;
    Tensor ext_v_cache = orch_args + 13;
    Tensor ext_wo = orch_args + 14;
    Tensor ext_post_rms_weight = orch_args + 15;
    Tensor ext_w_gate = orch_args + 16;
    Tensor ext_w_up = orch_args + 17;
    Tensor ext_w_down = orch_args + 18;
    Tensor ext_out = orch_args + 19;
    (void)ext_seq_lens;
    (void)ext_slot_mapping;

    const int64_t user_batch = QW3_USER_BATCH;
    const int64_t batch_padded = QW3_BATCH_PADDED;

    uint32_t all_q_padded_ci_shapes[2] = {11520, 128};
    Tensor all_q_padded = alloc_tensors(all_q_padded_ci_shapes, 2, BFLOAT16);

    uint32_t q_proj_ci_shapes[2] = {(uint32_t)batch_padded, 5120};
    Tensor q_proj = alloc_tensors(q_proj_ci_shapes, 2, FLOAT32);

    uint32_t k_proj_ci_shapes[2] = {(uint32_t)batch_padded, 1024};
    Tensor k_proj = alloc_tensors(k_proj_ci_shapes, 2, FLOAT32);

    uint32_t v_proj_ci_shapes[2] = {(uint32_t)batch_padded, 1024};
    Tensor v_proj = alloc_tensors(v_proj_ci_shapes, 2, FLOAT32);

    uint32_t q_proj_norm_ci_shapes[2] = {(uint32_t)batch_padded, 5120};
    Tensor q_proj_norm = alloc_tensors(q_proj_norm_ci_shapes, 2, FLOAT32);

    uint32_t k_proj_norm_ci_shapes[2] = {(uint32_t)batch_padded, 1024};
    Tensor k_proj_norm = alloc_tensors(k_proj_norm_ci_shapes, 2, FLOAT32);

    uint32_t attn_out_ci_shapes[2] = {(uint32_t)batch_padded, 5120};
    Tensor attn_out = alloc_tensors(attn_out_ci_shapes, 2, BFLOAT16);

    /* Phase 1: prefill — 6 tiles × 5 tasks, ids [QW3_G1_TASK_BASE ..] */
    for (int64_t b0 = 0; b0 < batch_padded; b0 += QW3_TILE_ROWS) {
        const size_t tix = (size_t)(b0 / QW3_TILE_ROWS);
        uint32_t normed_tile_ci_shapes[2] = {16, 5120};
        Tensor normed_tile = alloc_tensors(normed_tile_ci_shapes, 2, BFLOAT16);
        const int64_t cur_valid =
            (user_batch - b0 > QW3_TILE_ROWS) ? QW3_TILE_ROWS : (user_batch - b0);

        apply_qw3_g1_group((uint16_t)(QW3_G1_TASK_BASE + tix * QW3_G1_TASK_CNT), tix, ext_hidden_states,
                         ext_input_rms_weight, ext_wq, ext_wk, ext_wv, ext_q_norm_weight,
                         ext_k_norm_weight, q_proj, k_proj, v_proj, q_proj_norm, k_proj_norm,
                         normed_tile, b0, cur_valid);
    }

    /* Phase 2: attention — 90 batches × 5 tasks, ids [QW3_G2_TASK_BASE ..] */
    for (int64_t b = 0; b < user_batch; b += 1) {
        const size_t tix = (size_t)(b / QW3_TILE_ROWS);

        uint32_t all_raw_scores_ci_shapes[2] = {4096, 128};
        Tensor all_raw_scores = alloc_tensors(all_raw_scores_ci_shapes, 2, FLOAT32);
        uint32_t all_exp_padded_ci_shapes[2] = {4096, 128};
        Tensor all_exp_padded = alloc_tensors(all_exp_padded_ci_shapes, 2, BFLOAT16);
        uint32_t all_cur_mi_ci_shapes[2] = {4096, 1};
        Tensor all_cur_mi = alloc_tensors(all_cur_mi_ci_shapes, 2, FLOAT32);
        uint32_t all_cur_li_ci_shapes[2] = {4096, 1};
        Tensor all_cur_li = alloc_tensors(all_cur_li_ci_shapes, 2, FLOAT32);
        uint32_t all_oi_tmp_ci_shapes[2] = {4096, 128};
        Tensor all_oi_tmp = alloc_tensors(all_oi_tmp_ci_shapes, 2, FLOAT32);

        const int64_t ctx_len = 1024;
        const int64_t ctx_blocks = ((ctx_len + 127) / 128);
        const int64_t block_table_base = (b * 32);
        const int64_t slot = b;
        const int64_t slot_block = (slot / 128);
        const int64_t slot_offset = (slot - (slot_block * 128));

        /* Inter-group: qk_norm tile -> rope batch (90 edges). */
        macro_succeed_build((uint16_t)(QW3_G2_MACRO_BASE + b),
                            (uint16_t)(QW3_G1_MACRO_BASE + tix));

        apply_qw3_g2_group((uint16_t)(QW3_G2_TASK_BASE + b * QW3_G2_TASK_CNT), b, all_q_padded, ext_k_cache, ext_v_cache,
                         k_proj_norm, ext_rope_cos, ext_rope_sin, v_proj, q_proj_norm,
                         ext_block_table, all_raw_scores, all_exp_padded, all_cur_mi,
                         all_cur_li, all_oi_tmp, attn_out, ctx_blocks, ctx_len,
                         block_table_base, slot_block, slot_offset);
    }

    /* Phase 3: MLP — 6 tiles × 7 tasks, ids [QW3_G3_TASK_BASE ..] */
    for (int64_t b0 = 0; b0 < batch_padded; b0 += QW3_TILE_ROWS) {
        const size_t tix = (size_t)(b0 / QW3_TILE_ROWS);
        uint32_t resid1_tile_ci_shapes[2] = {16, 5120};
        Tensor resid1_tile = alloc_tensors(resid1_tile_ci_shapes, 2, FLOAT32);
        uint32_t gm_pipe_buffer_0_ci_shapes[2] = {16384, 40};
        Tensor gm_pipe_buffer_0 = alloc_tensors(gm_pipe_buffer_0_ci_shapes, 2, FLOAT32);
        uint32_t post_norm_tile_ci_shapes[2] = {16, 5120};
        Tensor post_norm_tile = alloc_tensors(post_norm_tile_ci_shapes, 2, BFLOAT16);
        uint32_t mlp_tile_ci_shapes[2] = {16, 17408};
        Tensor mlp_tile = alloc_tensors(mlp_tile_ci_shapes, 2, BFLOAT16);
        uint32_t gate_tile_ci_shapes[2] = {16, 17408};
        Tensor gate_tile = alloc_tensors(gate_tile_ci_shapes, 2, FLOAT32);
        uint32_t up_tile_ci_shapes[2] = {16, 17408};
        Tensor up_tile = alloc_tensors(up_tile_ci_shapes, 2, FLOAT32);
        uint32_t down_tile_ci_shapes[2] = {16, 5120};
        Tensor down_tile = alloc_tensors(down_tile_ci_shapes, 2, FLOAT32);
        const int64_t cur_valid =
            (user_batch - b0 > QW3_TILE_ROWS) ? QW3_TILE_ROWS : (user_batch - b0);

        /* Inter-group: online_softmax batches -> out_proj tile (90 edges total). */
        for (int64_t row = 0; row < cur_valid; row += 1) {
            macro_succeed_build((uint16_t)(QW3_G3_MACRO_BASE + tix),
                                (uint16_t)(QW3_G2_MACRO_BASE + b0 + row));
        }

        apply_qw3_g3_group((uint16_t)(QW3_G3_TASK_BASE + tix * QW3_G3_TASK_CNT), tix, b0, cur_valid, ext_hidden_states,
                         attn_out, ext_wo, ext_post_rms_weight, ext_w_gate, ext_w_up,
                         ext_w_down, ext_out, resid1_tile, gm_pipe_buffer_0, post_norm_tile,
                         gate_tile, up_tile, mlp_tile, down_tile);
    }

    orch_build_flush();
}
