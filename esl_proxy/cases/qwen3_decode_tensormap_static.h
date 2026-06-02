// Orchestration: qwen3_decode (task-group template + static intra deps +
// tensormap inter-group).
//
// Same task graph as cases/qwen3_decode_static.h. The intra-group dependencies
// are built EXACTLY like the static case — via the shared g1_dep/g2_dep/g3_dep
// blobs and qw3_g{1,2,3}_emit() helpers in build_static_subgraph.h — so the
// tensormap is NOT used inside a group. Only each group's *boundary* tensors
// touch the tensormap: a group inserts the outputs it hands to other groups and
// looks up the cross-group inputs it consumes. The producer/consumer overlap the
// tensormap discovers becomes the inter-group dependency edges, replacing the
// macro_group layer of the static case.
//
// Group boundaries:
//   G1 prefill tile -> inserts v_proj, q_proj_norm, k_proj_norm (tile rows).
//   G2 attn batch   -> looks up v_proj/q_proj_norm/k_proj_norm (its tile) at the
//                      rope entry; inserts attn_out row b at online_softmax.
//   G3 mlp tile     -> looks up attn_out rows [b0, b0+cur_valid) at out_proj;
//                      inserts the model output ext_out at down_proj_residual.
//
// Fixed shape: user_batch=90, tile_rows=16, batch_padded=96, num_tiles=6.

#include <stddef.h>
#include <stdint.h>

#ifndef USE_TENSORMAP
#error "qwen3_decode_tensormap_static.h requires -DUSE_TENSORMAP"
#endif

#include "build_static_subgraph.h"
#include "mem_pool.h"
#include "orch_build.h"
#include "ring_buf.h"
#include "tensormap.h"

static void apply_qw3_g1_group(uint16_t base,
                               Tensor ext_hidden_states, Tensor ext_input_rms_weight,
                               Tensor ext_wq, Tensor ext_wk, Tensor ext_wv,
                               Tensor ext_q_norm_weight, Tensor ext_k_norm_weight,
                               Tensor q_proj, Tensor k_proj, Tensor v_proj,
                               Tensor q_proj_norm, Tensor k_proj_norm,
                               Tensor normed_tile, int64_t b0, int64_t cur_valid)
{
    const uint64_t tensors[] = {
        [G1T_EXT_HIDDEN]    = tensor_base(ext_hidden_states),
        [G1T_EXT_INPUT_RMS] = tensor_base(ext_input_rms_weight),
        [G1T_EXT_WQ]        = tensor_base(ext_wq),
        [G1T_EXT_WK]        = tensor_base(ext_wk),
        [G1T_EXT_WV]        = tensor_base(ext_wv),
        [G1T_EXT_Q_NW]      = tensor_base(ext_q_norm_weight),
        [G1T_EXT_K_NW]      = tensor_base(ext_k_norm_weight),
        [G1T_Q_PROJ]        = tensor_base(q_proj),
        [G1T_K_PROJ]        = tensor_base(k_proj),
        [G1T_V_PROJ]        = tensor_base(v_proj),
        [G1T_Q_PROJ_N]      = tensor_base(q_proj_norm),
        [G1T_K_PROJ_N]      = tensor_base(k_proj_norm),
        [G1T_NORMED]        = tensor_base(normed_tile),
    };
    const int64_t scalars[] = {
        [G1S_B0]        = b0,
        [G1S_CUR_VALID] = cur_valid,
        [G1S_ZERO]      = 0,
    };

    qw3_g1_emit(base, tensors, scalars);

    /* Group outputs consumed by G2: v_proj (slot 3), q/k_proj_norm (slot 4). */
    Tensor v_view  = tensor_view(v_proj,      (uint32_t)b0, QW3_TILE_ROWS);
    Tensor qn_view = tensor_view(q_proj_norm, (uint32_t)b0, QW3_TILE_ROWS);
    Tensor kn_view = tensor_view(k_proj_norm, (uint32_t)b0, QW3_TILE_ROWS);
    tm_tensor_insert((uint16_t)(base + 3), &v_view);
    tm_tensor_insert((uint16_t)(base + 4), &qn_view);
    tm_tensor_insert((uint16_t)(base + 4), &kn_view);

    /* rmsnorm (entry) is a true root; the static blob wires the rest of the tile. */
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

static void apply_qw3_g2_group(uint16_t base, int64_t b, int64_t b_tile0,
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
        [G2T_ALL_Q]           = tensor_base(all_q_padded),
        [G2T_EXT_K_CACHE]     = tensor_base(ext_k_cache),
        [G2T_EXT_V_CACHE]     = tensor_base(ext_v_cache),
        [G2T_K_PROJ_N]        = tensor_base(k_proj_norm),
        [G2T_EXT_ROPE_COS]    = tensor_base(ext_rope_cos),
        [G2T_EXT_ROPE_SIN]    = tensor_base(ext_rope_sin),
        [G2T_V_PROJ]          = tensor_base(v_proj),
        [G2T_Q_PROJ_N]        = tensor_base(q_proj_norm),
        [G2T_EXT_BLOCK_TABLE] = tensor_base(ext_block_table),
        [G2T_ALL_RAW]         = tensor_base(all_raw_scores),
        [G2T_ALL_EXP]         = tensor_base(all_exp_padded),
        [G2T_ALL_MI]          = tensor_base(all_cur_mi),
        [G2T_ALL_LI]          = tensor_base(all_cur_li),
        [G2T_ALL_OI]          = tensor_base(all_oi_tmp),
        [G2T_ATTN_OUT]        = tensor_base(attn_out),
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

    /* Cross-group inputs from the G1 tile this batch belongs to (rope = slot 0). */
    Tensor kn_view = tensor_view(k_proj_norm, (uint32_t)b_tile0, QW3_TILE_ROWS);
    Tensor v_view  = tensor_view(v_proj,      (uint32_t)b_tile0, QW3_TILE_ROWS);
    Tensor qn_view = tensor_view(q_proj_norm, (uint32_t)b_tile0, QW3_TILE_ROWS);
    tm_tensor_lookup((uint16_t)(base + 0), &kn_view, false);
    tm_tensor_lookup((uint16_t)(base + 0), &v_view, false);
    tm_tensor_lookup((uint16_t)(base + 0), &qn_view, false);

    /* Group output consumed by G3: attn_out row b (online_softmax = slot 4). */
    Tensor ao_view = tensor_view(attn_out, (uint32_t)b, 1);
    tm_tensor_insert((uint16_t)(base + 4), &ao_view);

    /* rope (slot 0) carries the cross-group predecessors, so submit all five. */
    {
        const uint16_t ss[5] = {
            (uint16_t)(base + 0),
            (uint16_t)(base + 1),
            (uint16_t)(base + 2),
            (uint16_t)(base + 3),
            (uint16_t)(base + 4),
        };
        orch_defer_batch_submit(5, ss);
    }
}

static void apply_qw3_g3_group(uint16_t base, int64_t b0, int64_t cur_valid,
                               Tensor ext_hidden_states, Tensor attn_out, Tensor ext_wo,
                               Tensor ext_post_rms_weight, Tensor ext_w_gate, Tensor ext_w_up,
                               Tensor ext_w_down, Tensor ext_out,
                               Tensor resid1_tile, Tensor gm_pipe_buffer_0,
                               Tensor post_norm_tile, Tensor gate_tile, Tensor up_tile,
                               Tensor mlp_tile, Tensor down_tile)
{
    const uint64_t tensors[] = {
        [G3T_EXT_HIDDEN]    = tensor_base(ext_hidden_states),
        [G3T_ATTN_OUT]      = tensor_base(attn_out),
        [G3T_EXT_WO]        = tensor_base(ext_wo),
        [G3T_RESID1]        = tensor_base(resid1_tile),
        [G3T_GM_PIPE]       = tensor_base(gm_pipe_buffer_0),
        [G3T_POST_NORM]     = tensor_base(post_norm_tile),
        [G3T_EXT_POST_RMS]  = tensor_base(ext_post_rms_weight),
        [G3T_EXT_W_GATE]    = tensor_base(ext_w_gate),
        [G3T_GATE]          = tensor_base(gate_tile),
        [G3T_EXT_W_UP]      = tensor_base(ext_w_up),
        [G3T_UP]            = tensor_base(up_tile),
        [G3T_MLP]           = tensor_base(mlp_tile),
        [G3T_EXT_W_DOWN]    = tensor_base(ext_w_down),
        [G3T_DOWN]          = tensor_base(down_tile),
        [G3T_EXT_OUT]       = tensor_base(ext_out),
    };
    const int64_t scalars[] = {
        [G3S_B0]        = b0,
        [G3S_CUR_VALID] = cur_valid,
    };

    qw3_g3_emit(base, tensors, scalars);

    /* Cross-group input from G2: attn_out rows [b0, b0+cur_valid) (out_proj = slot 0). */
    Tensor ao_view = tensor_view(attn_out, (uint32_t)b0, (uint32_t)cur_valid);
    tm_tensor_lookup((uint16_t)(base + 0), &ao_view, false);

    /* Group output: the model output ext_out (down_proj_residual = slot 6). */
    tm_tensor_insert((uint16_t)(base + 6), &ext_out);

    /* out_proj (slot 0) carries the cross-group predecessors, so submit all seven. */
    {
        const uint16_t ss[7] = {
            (uint16_t)(base + 0),
            (uint16_t)(base + 1),
            (uint16_t)(base + 2),
            (uint16_t)(base + 3),
            (uint16_t)(base + 4),
            (uint16_t)(base + 5),
            (uint16_t)(base + 6),
        };
        orch_defer_batch_submit(7, ss);
    }
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    Tensor ext_hidden_states = tensor_from_base(orch_args + 0);
    Tensor ext_input_rms_weight = tensor_from_base(orch_args + 1);
    Tensor ext_wq = tensor_from_base(orch_args + 2);
    Tensor ext_wk = tensor_from_base(orch_args + 3);
    Tensor ext_wv = tensor_from_base(orch_args + 4);
    Tensor ext_q_norm_weight = tensor_from_base(orch_args + 5);
    Tensor ext_k_norm_weight = tensor_from_base(orch_args + 6);
    Tensor ext_seq_lens = tensor_from_base(orch_args + 7);
    Tensor ext_block_table = tensor_from_base(orch_args + 8);
    Tensor ext_slot_mapping = tensor_from_base(orch_args + 9);
    Tensor ext_rope_cos = tensor_from_base(orch_args + 10);
    Tensor ext_rope_sin = tensor_from_base(orch_args + 11);
    Tensor ext_k_cache = tensor_from_base(orch_args + 12);
    Tensor ext_v_cache = tensor_from_base(orch_args + 13);
    Tensor ext_wo = tensor_from_base(orch_args + 14);
    Tensor ext_post_rms_weight = tensor_from_base(orch_args + 15);
    Tensor ext_w_gate = tensor_from_base(orch_args + 16);
    Tensor ext_w_up = tensor_from_base(orch_args + 17);
    Tensor ext_w_down = tensor_from_base(orch_args + 18);
    Tensor ext_out = tensor_from_base(orch_args + 19);
    (void)ext_seq_lens;
    (void)ext_slot_mapping;

    tm_deps_init();

    const int64_t user_batch = QW3_USER_BATCH;
    const int64_t batch_padded = QW3_BATCH_PADDED;
    ext_out = tensor_make_2d(tensor_base(ext_out), (uint32_t)batch_padded, 5120, BFLOAT16);

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

    /* Phase 1: prefill — 6 tiles × 5 tasks */
    for (int64_t b0 = 0; b0 < batch_padded; b0 += QW3_TILE_ROWS) {
        const size_t tix = (size_t)(b0 / QW3_TILE_ROWS);
        uint32_t normed_tile_ci_shapes[2] = {16, 5120};
        Tensor normed_tile = alloc_tensors(normed_tile_ci_shapes, 2, BFLOAT16);
        const int64_t cur_valid =
            (user_batch - b0 > QW3_TILE_ROWS) ? QW3_TILE_ROWS : (user_batch - b0);

        apply_qw3_g1_group((uint16_t)(QW3_G1_TASK_BASE + tix * QW3_G1_TASK_CNT),
                           ext_hidden_states, ext_input_rms_weight, ext_wq, ext_wk, ext_wv,
                           ext_q_norm_weight, ext_k_norm_weight, q_proj, k_proj, v_proj,
                           q_proj_norm, k_proj_norm, normed_tile, b0, cur_valid);
    }
    orch_build_flush();

    /* Phase 2: attention — 90 batches × 5 tasks */
    for (int64_t b = 0; b < user_batch; b += 1) {
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

        const int64_t b_tile0 = (b / QW3_TILE_ROWS) * QW3_TILE_ROWS;
        const int64_t ctx_len = 1024;
        const int64_t ctx_blocks = ((ctx_len + 127) / 128);
        const int64_t block_table_base = (b * 32);
        const int64_t slot = b;
        const int64_t slot_block = (slot / 128);
        const int64_t slot_offset = (slot - (slot_block * 128));

        apply_qw3_g2_group((uint16_t)(QW3_G2_TASK_BASE + b * QW3_G2_TASK_CNT), b, b_tile0,
                           all_q_padded, ext_k_cache, ext_v_cache, k_proj_norm, ext_rope_cos,
                           ext_rope_sin, v_proj, q_proj_norm, ext_block_table, all_raw_scores,
                           all_exp_padded, all_cur_mi, all_cur_li, all_oi_tmp, attn_out,
                           ctx_blocks, ctx_len, block_table_base, slot_block, slot_offset);
    }
    orch_build_flush();

    /* Phase 3: MLP — 6 tiles × 7 tasks */
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

        apply_qw3_g3_group((uint16_t)(QW3_G3_TASK_BASE + tix * QW3_G3_TASK_CNT), b0, cur_valid,
                           ext_hidden_states, attn_out, ext_wo, ext_post_rms_weight, ext_w_gate,
                           ext_w_up, ext_w_down, ext_out, resid1_tile, gm_pipe_buffer_0,
                           post_norm_tile, gate_tile, up_tile, mlp_tile, down_tile);
    }
    orch_build_flush();
}
