// Tier-aware qwen3 static group builders (QWEN3_SPMD_TIER 0..4).
// Replaces all-spmd-only template emit for G1/G2/G3 when SPMD width < total_chunks.

#ifndef QW3_BUILD_STATIC_TIER_H
#define QW3_BUILD_STATIC_TIER_H

#include <stdatomic.h>
#include <stdint.h>

#include "orch_build.h"
#include "qw3_layout.h"
#include "ring_buf.h"

#ifndef QWEN3_SPMD_TIER
#define QWEN3_SPMD_TIER 2
#endif
#if QWEN3_SPMD_TIER < 0 || QWEN3_SPMD_TIER > 4
#error "QWEN3_SPMD_TIER must be 0..4"
#endif

#define QW3_DUR_RMSNORM 23950u
#define QW3_DUR_Q_PROJ 26060u
#define QW3_DUR_K_PROJ 18170u
#define QW3_DUR_V_PROJ 17890u
#define QW3_DUR_QK_NORM 13190u
#define QW3_DUR_ROPE 9480u
#define QW3_DUR_QK_MM 29350u
#define QW3_DUR_SOFTMAX 19400u
#define QW3_DUR_SV_MM 31650u
#define QW3_DUR_ONLINE 20820u
#define QW3_DUR_OUT_PROJ 40750u
#define QW3_DUR_POST_RMS 24390u
#define QW3_DUR_GATE 95700u
#define QW3_DUR_UP 97140u
#define QW3_DUR_SILU 2820u
#define QW3_DUR_DOWN 72220u
#define QW3_DUR_DOWN_RES 2590u

static uint16_t g_qw3_qk_norm_per_tile[6];
static uint16_t g_qw3_v_ids_per_tile[6][8];
static int g_qw3_v_cnt_per_tile[6];
static uint16_t g_qw3_os_by_b[90][4];
static int g_qw3_os_cnt_by_b[90];

static inline int qw3_bpt(int total_chunks)
{
    static const int targets[5] = {1, 2, 4, 8, 1 << 30};
    const int target = targets[QWEN3_SPMD_TIER];
    return total_chunks < target ? total_chunks : target;
}

static inline int qw3_n_tasks(int total_chunks)
{
    const int bpt = qw3_bpt(total_chunks);
    int n = 0;
    for (int base = 0; base < total_chunks; base += bpt)
        n++;
    return n;
}

static inline int qw3_cur_blocks(int total_chunks, int base, int bpt)
{
    return bpt < (total_chunks - base) ? bpt : (total_chunks - base);
}

static inline Tensor qw3_col_view(Tensor t, uint32_t col0, uint32_t ncols)
{
    return tensor_view(t, 1u, col0, ncols);
}

static inline void qw3_claim_at(uint16_t base, uint16_t n)
{
    atomic_store_explicit(&g_task_id, (int)base - 1, memory_order_relaxed);
    task_claim_range(n);
}

static inline void qw3_reset_desc(uint16_t id)
{
    struct task_desc *d = &g_basic_buf[id & RING_MASK];
    d->tensor_cnt = 0;
    d->scalar_cnt = 0;
}

static inline void qw3_set_single(uint16_t id, task_type_t type, uint16_t dur)
{
    struct task_desc *d = &g_basic_buf[id & RING_MASK];
    d->type = type;
    d->mode = ORG_MODE_SINGLE;
    d->count = 1;
    d->duration = dur;
}

static inline void qw3_set_spmd(uint16_t id, task_type_t type, uint32_t blocks,
                                uint16_t dur)
{
    struct task_desc *d = &g_basic_buf[id & RING_MASK];
    d->type = type;
    d->mode = ORG_MODE_SPMD_SYNC;
    d->count = blocks;
    d->duration = dur;
}

static inline void qw3_add_t(uint16_t id, Tensor t)
{
    struct task_desc *d = &g_basic_buf[id & RING_MASK];
    d->data[d->tensor_cnt++] = tensor_base(t);
}

static inline void qw3_add_s(uint16_t id, int64_t s)
{
    struct task_desc *d = &g_basic_buf[id & RING_MASK];
    d->scalar[d->scalar_cnt++] = s;
}

static inline void qw3_dep_all(uint16_t consumer, const uint16_t *ids, int n)
{
    for (int i = 0; i < n; i++)
        dep_install(consumer, ids[i]);
}

static inline int qw3_g1_tasks_per_tile(void)
{
    return 2 + qw3_n_tasks(20) + qw3_n_tasks(8) + qw3_n_tasks(8);
}

static inline int qw3_g2_tasks_per_batch(void)
{
    return 1 + 4 * qw3_n_tasks(4);
}

static inline int qw3_g3_tasks_per_tile(void)
{
    return qw3_n_tasks(40) + 1 + qw3_n_tasks(34) * 3 + qw3_n_tasks(40) * 2;
}

static inline uint16_t qw3_g1_task_base(size_t tix)
{
    return (uint16_t)(QW3_G1_TASK_BASE + tix * (size_t)qw3_g1_tasks_per_tile());
}

static inline uint16_t qw3_g2_task_base(int64_t b)
{
    return (uint16_t)(QW3_G1_TASK_BASE +
                      (size_t)QW3_NUM_TILES * (size_t)qw3_g1_tasks_per_tile() +
                      (size_t)b * (size_t)qw3_g2_tasks_per_batch());
}

static inline uint16_t qw3_g3_task_base(size_t tix)
{
    return (uint16_t)(QW3_G1_TASK_BASE +
                      (size_t)QW3_NUM_TILES * (size_t)qw3_g1_tasks_per_tile() +
                      (size_t)QW3_USER_BATCH * (size_t)qw3_g2_tasks_per_batch() +
                      tix * (size_t)qw3_g3_tasks_per_tile());
}

typedef struct {
    uint16_t base;
    uint16_t rmsnorm;
    uint16_t qk_norm;
    uint16_t v_last;
    uint16_t n_tasks;
} qw3_g1_layout_t;

typedef struct {
    uint16_t base;
    uint16_t rope;
    uint16_t online_last;
    uint16_t n_tasks;
} qw3_g2_layout_t;

typedef struct {
    uint16_t base;
    uint16_t out_first;
    uint16_t post_rms;
    uint16_t down_res_last;
    uint16_t n_tasks;
} qw3_g3_layout_t;

static inline qw3_g1_layout_t qw3_g1_build_group(
    uint16_t base, size_t tix, Tensor ext_hidden_states,
    Tensor ext_input_rms_weight, Tensor ext_wq, Tensor ext_wk, Tensor ext_wv,
    Tensor ext_q_norm_weight, Tensor ext_k_norm_weight, Tensor q_proj,
    Tensor k_proj, Tensor v_proj, Tensor q_proj_norm, Tensor k_proj_norm,
    Tensor normed_tile, int64_t b0, int64_t cur_valid)
{
    const int q_tot = 20, k_tot = 8, v_tot = 8;
    const int q_bpt = qw3_bpt(q_tot), k_bpt = qw3_bpt(k_tot), v_bpt = qw3_bpt(v_tot);
    const int q_n = qw3_n_tasks(q_tot), k_n = qw3_n_tasks(k_tot), v_n = qw3_n_tasks(v_tot);
    const int n_tasks = 2 + q_n + k_n + v_n;
    uint16_t q_ids[20];
    uint16_t k_ids[8];
    uint16_t v_ids[8];
    uint16_t off = 0;

    (void)cur_valid;
    qw3_claim_at(base, (uint16_t)n_tasks);

    const uint16_t rmsnorm_id = (uint16_t)(base + off);
    qw3_reset_desc(rmsnorm_id);
    qw3_set_single(rmsnorm_id, TASK_TYPE_VECTOR, QW3_DUR_RMSNORM);
    qw3_add_t(rmsnorm_id, ext_hidden_states);
    qw3_add_t(rmsnorm_id, normed_tile);
    qw3_add_t(rmsnorm_id, ext_input_rms_weight);
    qw3_add_s(rmsnorm_id, b0);
    qw3_add_s(rmsnorm_id, cur_valid);
    off++;

    for (int qi = 0, chunk = 0; chunk < q_tot; chunk += q_bpt) {
        const int cur_blocks = qw3_cur_blocks(q_tot, chunk, q_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_Q_PROJ);
        qw3_add_t(id, normed_tile);
        qw3_add_t(id, ext_wq);
        Tensor q0 = tensor_row_view(q_proj, (uint32_t)b0, QW3_TILE_ROWS);
        qw3_add_t(id, qw3_col_view(q0, (uint32_t)(chunk * 256u),
                                   (uint32_t)(cur_blocks * 256u)));
        qw3_add_s(id, b0);
        qw3_add_s(id, chunk);
        dep_install(id, rmsnorm_id);
        q_ids[qi++] = id;
    }

    for (int ki = 0, chunk = 0; chunk < k_tot; chunk += k_bpt) {
        const int cur_blocks = qw3_cur_blocks(k_tot, chunk, k_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_K_PROJ);
        qw3_add_t(id, normed_tile);
        qw3_add_t(id, ext_wk);
        Tensor k0 = tensor_row_view(k_proj, (uint32_t)b0, QW3_TILE_ROWS);
        qw3_add_t(id, qw3_col_view(k0, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_s(id, b0);
        qw3_add_s(id, chunk);
        dep_install(id, rmsnorm_id);
        k_ids[ki++] = id;
    }

    for (int vi = 0, chunk = 0; chunk < v_tot; chunk += v_bpt) {
        const int cur_blocks = qw3_cur_blocks(v_tot, chunk, v_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_V_PROJ);
        qw3_add_t(id, normed_tile);
        qw3_add_t(id, ext_wv);
        Tensor v0 = tensor_row_view(v_proj, (uint32_t)b0, QW3_TILE_ROWS);
        qw3_add_t(id, qw3_col_view(v0, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_s(id, b0);
        qw3_add_s(id, chunk);
        dep_install(id, rmsnorm_id);
        v_ids[vi++] = id;
    }

    for (int i = 0; i < v_n; i++)
        g_qw3_v_ids_per_tile[tix][i] = v_ids[i];
    g_qw3_v_cnt_per_tile[tix] = v_n;

    const uint16_t qk_norm_id = (uint16_t)(base + off);
    qw3_reset_desc(qk_norm_id);
    qw3_set_single(qk_norm_id, TASK_TYPE_VECTOR, QW3_DUR_QK_NORM);
    qw3_add_t(qk_norm_id, tensor_row_view(k_proj_norm, (uint32_t)b0, QW3_TILE_ROWS));
    qw3_add_t(qk_norm_id, tensor_row_view(q_proj_norm, (uint32_t)b0, QW3_TILE_ROWS));
    qw3_add_t(qk_norm_id, tensor_row_view(q_proj, (uint32_t)b0, QW3_TILE_ROWS));
    qw3_add_t(qk_norm_id, ext_q_norm_weight);
    qw3_add_t(qk_norm_id, tensor_row_view(k_proj, (uint32_t)b0, QW3_TILE_ROWS));
    qw3_add_t(qk_norm_id, ext_k_norm_weight);
    qw3_add_s(qk_norm_id, 0);
    qw3_add_s(qk_norm_id, b0);
    qw3_dep_all(qk_norm_id, q_ids, q_n);
    qw3_dep_all(qk_norm_id, k_ids, k_n);
    g_qw3_qk_norm_per_tile[tix] = qk_norm_id;

    qw3_g1_layout_t layout = {
        .base = base,
        .rmsnorm = rmsnorm_id,
        .qk_norm = qk_norm_id,
        .v_last = v_ids[v_n - 1],
        .n_tasks = (uint16_t)n_tasks,
    };
    return layout;
}

static inline qw3_g2_layout_t qw3_g2_build_group(
    uint16_t base, int64_t b, int64_t b_tile0, size_t tix, Tensor all_q_padded,
    Tensor ext_k_cache, Tensor ext_v_cache, Tensor k_proj_norm, Tensor ext_rope_cos,
    Tensor ext_rope_sin, Tensor v_proj, Tensor q_proj_norm, Tensor ext_block_table,
    Tensor all_raw_scores, Tensor all_exp_padded, Tensor all_cur_mi,
    Tensor all_cur_li, Tensor all_oi_tmp, Tensor attn_out, int64_t ctx_blocks,
    int64_t ctx_len, int64_t block_table_base, int64_t slot_block,
    int64_t slot_offset)
{
    const int qk_tot = 4, sm_tot = 4, sv_tot = 4, os_tot = 4;
    const int qk_bpt = qw3_bpt(qk_tot), sm_bpt = qw3_bpt(sm_tot);
    const int sv_bpt = qw3_bpt(sv_tot), os_bpt = qw3_bpt(os_tot);
    const int qk_n = qw3_n_tasks(qk_tot), sm_n = qw3_n_tasks(sm_tot);
    const int sv_n = qw3_n_tasks(sv_tot), os_n = qw3_n_tasks(os_tot);
    const int n_tasks = 1 + qk_n + sm_n + sv_n + os_n;
    uint16_t qk_ids[4], sm_ids[4], sv_ids[4];
    uint16_t off = 0;

    (void)tix;
    qw3_claim_at(base, (uint16_t)n_tasks);

    const uint64_t kv_row_bytes = (uint64_t)1024u * (uint64_t)BFLOAT16;
    Tensor k_cache_local =
        tensor_make_2d(tensor_base(ext_k_cache) + (uint64_t)b * kv_row_bytes, 1u,
                       1024u, BFLOAT16);
    Tensor v_cache_local =
        tensor_make_2d(tensor_base(ext_v_cache) + (uint64_t)b * kv_row_bytes, 1u,
                       1024u, BFLOAT16);
    const uint16_t rope_id = (uint16_t)(base + off);
    qw3_reset_desc(rope_id);
    qw3_set_single(rope_id, TASK_TYPE_VECTOR, QW3_DUR_ROPE);
    qw3_add_t(rope_id, all_q_padded);
    qw3_add_t(rope_id, k_cache_local);
    qw3_add_t(rope_id, v_cache_local);
    qw3_add_t(rope_id, tensor_row_view(k_proj_norm, (uint32_t)b_tile0, QW3_TILE_ROWS));
    qw3_add_t(rope_id, ext_rope_cos);
    qw3_add_t(rope_id, ext_rope_sin);
    qw3_add_t(rope_id, ext_rope_cos);
    qw3_add_t(rope_id, ext_rope_sin);
    qw3_add_t(rope_id, tensor_row_view(v_proj, (uint32_t)b_tile0, QW3_TILE_ROWS));
    qw3_add_t(rope_id, tensor_row_view(q_proj_norm, (uint32_t)b_tile0, QW3_TILE_ROWS));
    qw3_add_s(rope_id, slot_block);
    qw3_add_s(rope_id, slot_offset);
    qw3_add_s(rope_id, b);
    dep_install(rope_id, g_qw3_qk_norm_per_tile[b / QW3_TILE_ROWS]);
    qw3_dep_all(rope_id, g_qw3_v_ids_per_tile[b / QW3_TILE_ROWS],
                g_qw3_v_cnt_per_tile[b / QW3_TILE_ROWS]);
    off++;

    for (int qki = 0, chunk = 0; chunk < qk_tot; chunk += qk_bpt) {
        const int cur_blocks = qw3_cur_blocks(qk_tot, chunk, qk_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_QK_MM);
        qw3_add_t(id, all_q_padded);
        qw3_add_t(id, tensor_row_view(all_raw_scores, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, ext_block_table);
        qw3_add_t(id, k_cache_local);
        qw3_add_s(id, b);
        qw3_add_s(id, ctx_blocks);
        qw3_add_s(id, block_table_base);
        qw3_add_s(id, chunk);
        dep_install(id, rope_id);
        qk_ids[qki++] = id;
    }

    for (int smi = 0, chunk = 0; chunk < sm_tot; chunk += sm_bpt) {
        const int cur_blocks = qw3_cur_blocks(sm_tot, chunk, sm_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, QW3_DUR_SOFTMAX);
        qw3_add_t(id, tensor_row_view(all_cur_li, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, tensor_row_view(all_cur_mi, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, tensor_row_view(all_exp_padded, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, tensor_row_view(all_raw_scores, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_s(id, ctx_blocks);
        qw3_add_s(id, ctx_len);
        qw3_add_s(id, chunk);
        dep_install(id, qk_ids[smi]);
        sm_ids[smi++] = id;
    }

    for (int svi = 0, chunk = 0; chunk < sv_tot; chunk += sv_bpt) {
        const int cur_blocks = qw3_cur_blocks(sv_tot, chunk, sv_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_SV_MM);
        qw3_add_t(id, tensor_row_view(all_oi_tmp, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, ext_block_table);
        qw3_add_t(id, tensor_row_view(all_exp_padded, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, v_cache_local);
        qw3_add_s(id, ctx_blocks);
        qw3_add_s(id, block_table_base);
        qw3_add_s(id, chunk);
        dep_install(id, rope_id);
        dep_install(id, sm_ids[svi]);
        sv_ids[svi++] = id;
    }

    int osi = 0;
    uint16_t online_last = rope_id;
    for (int chunk = 0; chunk < os_tot; chunk += os_bpt) {
        const int cur_blocks = qw3_cur_blocks(os_tot, chunk, os_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, QW3_DUR_ONLINE);
        qw3_add_t(id, tensor_row_view(all_oi_tmp, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, tensor_row_view(all_cur_mi, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        qw3_add_t(id, tensor_row_view(all_cur_li, (uint32_t)(chunk * 1024),
                                      (uint32_t)(cur_blocks * 1024)));
        Tensor attn_out_row = tensor_row_view(attn_out, (uint32_t)b, 1u);
        qw3_add_t(id, qw3_col_view(attn_out_row, (uint32_t)(chunk * 1280u),
                                   (uint32_t)(cur_blocks * 1280u)));
        qw3_add_s(id, ctx_blocks);
        qw3_add_s(id, chunk);
        dep_install(id, sv_ids[osi]);
        dep_install(id, sm_ids[osi]);
        g_qw3_os_by_b[b][osi] = id;
        online_last = id;
        osi++;
    }
    g_qw3_os_cnt_by_b[b] = osi;

    qw3_g2_layout_t layout = {
        .base = base,
        .rope = rope_id,
        .online_last = online_last,
        .n_tasks = (uint16_t)n_tasks,
    };
    return layout;
}

static inline qw3_g3_layout_t qw3_g3_build_group(
    uint16_t base, int64_t b0, int64_t cur_valid, Tensor ext_hidden_states,
    Tensor attn_out, Tensor ext_wo, Tensor ext_post_rms_weight, Tensor ext_w_gate,
    Tensor ext_w_up, Tensor ext_w_down, Tensor ext_out, Tensor resid1_tile,
    Tensor gm_pipe_buffer_0, Tensor post_norm_tile, Tensor gate_tile,
    Tensor up_tile, Tensor mlp_tile, Tensor down_tile)
{
    const int op_tot = 40, gate_tot = 34, up_tot = 34, silu_tot = 34;
    const int down_tot = 40, dres_tot = 40;
    const int op_bpt = qw3_bpt(op_tot), gate_bpt = qw3_bpt(gate_tot);
    const int up_bpt = qw3_bpt(up_tot), silu_bpt = qw3_bpt(silu_tot);
    const int down_bpt = qw3_bpt(down_tot), dres_bpt = qw3_bpt(dres_tot);
    const int op_n = qw3_n_tasks(op_tot), gate_n = qw3_n_tasks(gate_tot);
    const int up_n = qw3_n_tasks(up_tot), silu_n = qw3_n_tasks(silu_tot);
    const int down_n = qw3_n_tasks(down_tot), dres_n = qw3_n_tasks(dres_tot);
    const int n_tasks = op_n + 1 + gate_n + up_n + silu_n + down_n + dres_n;
    uint16_t op_ids[40], gate_ids[34], up_ids[34], silu_ids[34], down_ids[40];
    uint16_t off = 0;

    qw3_claim_at(base, (uint16_t)n_tasks);

    for (int opi = 0, chunk = 0; chunk < op_tot; chunk += op_bpt) {
        const int cur_blocks = qw3_cur_blocks(op_tot, chunk, op_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_MIX, (uint32_t)cur_blocks, QW3_DUR_OUT_PROJ);
        qw3_add_t(id, qw3_col_view(ext_hidden_states, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_t(id, tensor_row_view(attn_out, (uint32_t)b0, (uint32_t)cur_valid));
        qw3_add_t(id, qw3_col_view(ext_wo, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_t(id, qw3_col_view(resid1_tile, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_t(id, gm_pipe_buffer_0);
        qw3_add_s(id, b0);
        qw3_add_s(id, cur_valid);
        qw3_add_s(id, chunk);
        for (int64_t row = 0; row < cur_valid; row++) {
            const int64_t bb = b0 + row;
            qw3_dep_all(id, g_qw3_os_by_b[bb], g_qw3_os_cnt_by_b[bb]);
        }
        op_ids[opi++] = id;
    }

    const uint16_t post_id = (uint16_t)(base + off++);
    qw3_reset_desc(post_id);
    qw3_set_single(post_id, TASK_TYPE_VECTOR, QW3_DUR_POST_RMS);
    qw3_add_t(post_id, resid1_tile);
    qw3_add_t(post_id, post_norm_tile);
    qw3_add_t(post_id, ext_post_rms_weight);
    qw3_dep_all(post_id, op_ids, op_n);

    for (int gi = 0, chunk = 0; chunk < gate_tot; chunk += gate_bpt) {
        const int cur_blocks = qw3_cur_blocks(gate_tot, chunk, gate_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_GATE);
        qw3_add_t(id, post_norm_tile);
        qw3_add_t(id, qw3_col_view(ext_w_gate, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_t(id, qw3_col_view(gate_tile, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_s(id, chunk);
        dep_install(id, post_id);
        gate_ids[gi++] = id;
    }

    for (int ui = 0, chunk = 0; chunk < up_tot; chunk += up_bpt) {
        const int cur_blocks = qw3_cur_blocks(up_tot, chunk, up_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_UP);
        qw3_add_t(id, post_norm_tile);
        qw3_add_t(id, qw3_col_view(ext_w_up, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_t(id, qw3_col_view(up_tile, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_s(id, chunk);
        dep_install(id, post_id);
        up_ids[ui++] = id;
    }

    for (int si = 0, chunk = 0; chunk < silu_tot; chunk += silu_bpt) {
        const int cur_blocks = qw3_cur_blocks(silu_tot, chunk, silu_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, QW3_DUR_SILU);
        qw3_add_t(id, qw3_col_view(gate_tile, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_t(id, qw3_col_view(up_tile, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_t(id, qw3_col_view(mlp_tile, (uint32_t)(chunk * 512u),
                                   (uint32_t)(cur_blocks * 512u)));
        qw3_add_s(id, chunk);
        dep_install(id, gate_ids[si]);
        dep_install(id, up_ids[si]);
        silu_ids[si++] = id;
    }

    for (int di = 0, chunk = 0; chunk < down_tot; chunk += down_bpt) {
        const int cur_blocks = qw3_cur_blocks(down_tot, chunk, down_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_CUBE, (uint32_t)cur_blocks, QW3_DUR_DOWN);
        qw3_add_t(id, mlp_tile);
        qw3_add_t(id, qw3_col_view(ext_w_down, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_t(id, qw3_col_view(down_tile, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_s(id, chunk);
        qw3_dep_all(id, silu_ids, silu_n);
        down_ids[di++] = id;
    }

    uint16_t down_res_last = post_id;
    for (int dri = 0, chunk = 0; chunk < dres_tot; chunk += dres_bpt) {
        const int cur_blocks = qw3_cur_blocks(dres_tot, chunk, dres_bpt);
        const uint16_t id = (uint16_t)(base + off++);
        qw3_reset_desc(id);
        qw3_set_spmd(id, TASK_TYPE_VECTOR, (uint32_t)cur_blocks, QW3_DUR_DOWN_RES);
        qw3_add_t(id, qw3_col_view(down_tile, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_t(id, qw3_col_view(resid1_tile, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        Tensor out_tile = tensor_row_view(ext_out, (uint32_t)b0, (uint32_t)cur_valid);
        qw3_add_t(id, qw3_col_view(out_tile, (uint32_t)(chunk * 128u),
                                   (uint32_t)(cur_blocks * 128u)));
        qw3_add_s(id, cur_valid);
        qw3_add_s(id, b0);
        qw3_add_s(id, chunk);
        dep_install(id, down_ids[dri]);
        dep_install(id, op_ids[dri]);
        down_res_last = id;
        dri++;
    }

    qw3_g3_layout_t layout = {
        .base = base,
        .out_first = (uint16_t)base,
        .post_rms = post_id,
        .down_res_last = down_res_last,
        .n_tasks = (uint16_t)n_tasks,
    };
    return layout;
}

#endif /* QW3_BUILD_STATIC_TIER_H */
