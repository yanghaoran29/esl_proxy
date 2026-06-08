// Orchestration: qwen3_decode static manual (task-group + intra deps + macro inter-group).
//
// Fixed shape: user_batch=90, tile_rows=16, batch_padded=96, num_tiles=6.
// Task counts follow QWEN3_SPMD_TIER (default 2 → 864 tasks).

#include <stddef.h>
#include <stdint.h>

#include "build_static_tier.h"
#include "macro_group.h"
#include "mem_pool.h"
#include "orch_build.h"
#include "ring_buf.h"

static void qw3_defer_non_entry(uint16_t base, uint16_t n_tasks, uint16_t entry)
{
    for (uint16_t i = 0; i < n_tasks; i++) {
        const uint16_t id = (uint16_t)(base + i);
        if (id != entry)
            orch_defer_submit(id);
    }
}

static void apply_qw3_g1_group(size_t tix, Tensor ext_hidden_states,
                               Tensor ext_input_rms_weight, Tensor ext_wq, Tensor ext_wk,
                               Tensor ext_wv, Tensor ext_q_norm_weight,
                               Tensor ext_k_norm_weight, Tensor q_proj, Tensor k_proj,
                               Tensor v_proj, Tensor q_proj_norm, Tensor k_proj_norm,
                               Tensor normed_tile, int64_t b0, int64_t cur_valid)
{
    const uint16_t base = qw3_g1_task_base(tix);
    const qw3_g1_layout_t ly = qw3_g1_build_group(
        base, tix, ext_hidden_states, ext_input_rms_weight, ext_wq, ext_wk, ext_wv,
        ext_q_norm_weight, ext_k_norm_weight, q_proj, k_proj, v_proj, q_proj_norm,
        k_proj_norm, normed_tile, b0, cur_valid);

    macro_group_bind((uint16_t)(QW3_G1_MACRO_BASE + tix), ly.rmsnorm, ly.qk_norm);
    qw3_defer_non_entry(base, ly.n_tasks, ly.rmsnorm);
    orch_defer_submit(ly.qk_norm);
    orch_defer_root(ly.rmsnorm);
}

static void apply_qw3_g2_group(int64_t b, int64_t b_tile0, size_t tix,
                               Tensor all_q_padded, Tensor ext_k_cache, Tensor ext_v_cache,
                               Tensor k_proj_norm, Tensor ext_rope_cos, Tensor ext_rope_sin,
                               Tensor v_proj, Tensor q_proj_norm, Tensor ext_block_table,
                               Tensor all_raw_scores, Tensor all_exp_padded,
                               Tensor all_cur_mi, Tensor all_cur_li, Tensor all_oi_tmp,
                               Tensor attn_out, int64_t ctx_blocks, int64_t ctx_len,
                               int64_t block_table_base, int64_t slot_block,
                               int64_t slot_offset)
{
    const uint16_t base = qw3_g2_task_base(b);
    const qw3_g2_layout_t ly = qw3_g2_build_group(
        base, b, b_tile0, tix, all_q_padded, ext_k_cache, ext_v_cache, k_proj_norm,
        ext_rope_cos, ext_rope_sin, v_proj, q_proj_norm, ext_block_table, all_raw_scores,
        all_exp_padded, all_cur_mi, all_cur_li, all_oi_tmp, attn_out, ctx_blocks,
        ctx_len, block_table_base, slot_block, slot_offset);

    macro_group_bind((uint16_t)(QW3_G2_MACRO_BASE + b), ly.rope, ly.online_last);
    macro_gate_micro_build(ly.rope, 1);
    /* cross-group v/qk_norm preds already installed in qw3_g2_build_group */
    qw3_defer_non_entry(base, ly.n_tasks, ly.rope);
}

static void apply_qw3_g3_group(size_t tix, int64_t b0, int64_t cur_valid,
                               Tensor ext_hidden_states, Tensor attn_out, Tensor ext_wo,
                               Tensor ext_post_rms_weight, Tensor ext_w_gate, Tensor ext_w_up,
                               Tensor ext_w_down, Tensor ext_out, Tensor resid1_tile,
                               Tensor gm_pipe_buffer_0, Tensor post_norm_tile, Tensor gate_tile,
                               Tensor up_tile, Tensor mlp_tile, Tensor down_tile)
{
    const uint16_t base = qw3_g3_task_base(tix);
    const qw3_g3_layout_t ly = qw3_g3_build_group(
        base, b0, cur_valid, ext_hidden_states, attn_out, ext_wo, ext_post_rms_weight,
        ext_w_gate, ext_w_up, ext_w_down, ext_out, resid1_tile, gm_pipe_buffer_0,
        post_norm_tile, gate_tile, up_tile, mlp_tile, down_tile);

    macro_group_bind((uint16_t)(QW3_G3_MACRO_BASE + tix), ly.out_first, ly.down_res_last);
    macro_gate_micro_build(ly.out_first, 1);
    qw3_defer_non_entry(base, ly.n_tasks, ly.out_first);
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

    const int64_t user_batch = QW3_USER_BATCH;
    const int64_t batch_padded = QW3_BATCH_PADDED;
    ext_out = tensor_make_2d(tensor_base(ext_out), (uint32_t)batch_padded, 5120,
                             BFLOAT16);

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

    for (int64_t b0 = 0; b0 < batch_padded; b0 += QW3_TILE_ROWS) {
        const size_t tix = (size_t)(b0 / QW3_TILE_ROWS);
        uint32_t normed_tile_ci_shapes[2] = {16, 5120};
        Tensor normed_tile = alloc_tensors(normed_tile_ci_shapes, 2, BFLOAT16);
        const int64_t cur_valid =
            (user_batch - b0 > QW3_TILE_ROWS) ? QW3_TILE_ROWS : (user_batch - b0);

        apply_qw3_g1_group(tix, ext_hidden_states, ext_input_rms_weight, ext_wq, ext_wk,
                         ext_wv, ext_q_norm_weight, ext_k_norm_weight, q_proj, k_proj,
                         v_proj, q_proj_norm, k_proj_norm, normed_tile, b0, cur_valid);
    }

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

        const int64_t b_tile0 = (b / QW3_TILE_ROWS) * QW3_TILE_ROWS;
        const int64_t ctx_len = 1024;
        const int64_t ctx_blocks = ((ctx_len + 127) / 128);
        const int64_t block_table_base = (b * 32);
        const int64_t slot = b;
        const int64_t slot_block = (slot / 128);
        const int64_t slot_offset = (slot - (slot_block * 128));

        macro_succeed_build((uint16_t)(QW3_G2_MACRO_BASE + b),
                            (uint16_t)(QW3_G1_MACRO_BASE + tix));

        apply_qw3_g2_group(b, b_tile0, tix, all_q_padded, ext_k_cache, ext_v_cache,
                         k_proj_norm, ext_rope_cos, ext_rope_sin, v_proj, q_proj_norm,
                         ext_block_table, all_raw_scores, all_exp_padded, all_cur_mi,
                         all_cur_li, all_oi_tmp, attn_out, ctx_blocks, ctx_len,
                         block_table_base, slot_block, slot_offset);
    }

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

        for (int64_t row = 0; row < cur_valid; row += 1) {
            macro_succeed_build((uint16_t)(QW3_G3_MACRO_BASE + tix),
                                (uint16_t)(QW3_G2_MACRO_BASE + b0 + row));
        }

        apply_qw3_g3_group(tix, b0, cur_valid, ext_hidden_states, attn_out, ext_wo,
                         ext_post_rms_weight, ext_w_gate, ext_w_up, ext_w_down, ext_out,
                         resid1_tile, gm_pipe_buffer_0, post_norm_tile, gate_tile, up_tile,
                         mlp_tile, down_tile);
    }

    orch_build_flush();
}
