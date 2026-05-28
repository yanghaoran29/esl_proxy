// Orchestration Function: qwen3_decode (esl_proxy rewrite, Fake_Tensor form).
//
// Ported from simpler-V200/examples/qwen3/dynamic_manual_scope/orchestration/
// qwen3_decode.cpp. Each task is described with task.h's task_desc directly.
// All tensor storage uses Fake_Tensor<N, T> globals (BSS-resident); the
// orchestration code passes raw .data pointers to fake_runtime_info.
//
// dtype mapping (per user request): bfloat16 -> short, float32 -> int.
// Shapes/dtypes mirror simpler-V200/examples/qwen3/dynamic_manual_scope
// (test_qwen3_decode.py:87-110, 588-632).
//
// task_desc field usage (per spec 008):
//   .type   = AIC->TASK_TYPE_CUBE, AIV->TASK_TYPE_VECTOR, MIX->TASK_TYPE_MIX
//   .mode   = original set_block_num(N) -> ORG_MODE_SPMD_ASYNC (count=N),
//             otherwise ORG_MODE_SINGLE (count=1)
//   .kernel = single kernel pointer (MIX uses one kernel; the second AIV lane
//             from the original MixedKernels{10,11,11} is dropped)
//   .index  = 0; .prio = 0
//   .data   = pointer to fake_runtime_info

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

#include "dag/task.h"
#include "dag/fake_orchestration_api.h"

/* ========================== Compile-time configuration ====================
 * Mirrors test_qwen3_decode.py constants (test_qwen3_decode.py:87-110).
 * Change USER_BATCH to retarget the workload.
 * ==========================================================================*/

// ----- Model dimensions (== test_qwen3_decode.py) -----
#define USER_BATCH       90       // BATCH
#define MAX_SEQ          4096
#define NUM_HEADS        40
#define NUM_KV_HEADS     8
#define HEAD_DIM         128
#define HIDDEN           (NUM_HEADS * HEAD_DIM)        // 5120
#define KV_HIDDEN        (NUM_KV_HEADS * HEAD_DIM)     // 1024
#define INTERMEDIATE     17408
#define BATCH_TILE       16
#define BLOCK_SIZE       128
#define Q_HEAD_BATCH     5

// ----- Derived dims -----
#define HALF_HEAD_DIM        (HEAD_DIM / 2)            // 64
#define BATCH_PADDED         (((USER_BATCH + BATCH_TILE - 1) / BATCH_TILE) * BATCH_TILE)
#define NUM_TILES            (BATCH_PADDED / BATCH_TILE)
#define MAX_BLOCKS_PER_SEQ   ((MAX_SEQ + BLOCK_SIZE - 1) / BLOCK_SIZE)   // 32
#define CACHE_NUM_BLOCKS     (USER_BATCH * MAX_BLOCKS_PER_SEQ)           // 90*32 = 2880
#define CACHE_ROWS           (CACHE_NUM_BLOCKS * NUM_KV_HEADS * BLOCK_SIZE)
                                                                          // 2880*8*128 = 2,949,120

// ----- Tiling / chunk sizes (== test_qwen3_decode.py) -----
#define Q_OUT_CHUNK      256
#define KV_OUT_CHUNK     128
#define MLP_OUT_CHUNK    512
#define DOWN_OUT_CHUNK   128
#define NUM_OB           (INTERMEDIATE / MLP_OUT_CHUNK)     // 34
#define NUM_DOB          (HIDDEN / DOWN_OUT_CHUNK)          // 40

// ----- Per-batch attention scratch -----
#define ATTN_SCRATCH_ROWS    MAX_SEQ                       // 4096
#define ALL_Q_PADDED_ROWS    (USER_BATCH * HEAD_DIM)       // 11520 when BATCH=90

// ----- out_proj MIX task pipeline buffer -----
#define GM_PIPE_PER_BLOCK    16384
#define GM_PIPE_BLOCK_NUM    NUM_HEADS                     // 40
#define GM_PIPE_BUFFER_SIZE  (GM_PIPE_PER_BLOCK * GM_PIPE_BLOCK_NUM)

// ----- SPMD block counts -----
#define SPMD_BLOCK_NUM       4
#define MIX_BLOCK_NUM        NUM_HEADS                     // 40

// ----- online_softmax launch grid -----
#define ONLINE_SOFTMAX_HEADS    NUM_KV_HEADS               // 8
#define ONLINE_SOFTMAX_STRIDE   2
#define ONLINE_SOFTMAX_LAUNCHES (ONLINE_SOFTMAX_HEADS / ONLINE_SOFTMAX_STRIDE)  // 4

// ----- Per-tile chunk counts (for global storage sizing) -----
#define Q_CHUNKS_PER_TILE    (HIDDEN / Q_OUT_CHUNK)        // 20
#define KV_CHUNKS_PER_TILE   (KV_HIDDEN / KV_OUT_CHUNK)    // 8

// Expected external arg count (formerly orchestration_config.expected_arg_count)
constexpr uint32_t EXPECTED_ARG_COUNT = 20;

/* ========================== External tensors (20) =========================
 * Shape and dtype per test_qwen3_decode.py:588-632.
 * dtype: torch.bfloat16 -> short; torch.float32 / torch.int32 -> int.
 * ==========================================================================*/
static Fake_Tensor<USER_BATCH * HIDDEN,             short> ext_hidden_states;    //  0 BF16
static Fake_Tensor<HIDDEN,                          int>   ext_input_rms_weight; //  1 F32
static Fake_Tensor<HIDDEN * HIDDEN,                 short> ext_wq;               //  2 BF16
static Fake_Tensor<HIDDEN * KV_HIDDEN,              short> ext_wk;               //  3 BF16
static Fake_Tensor<HIDDEN * KV_HIDDEN,              short> ext_wv;               //  4 BF16
static Fake_Tensor<HEAD_DIM,                        int>   ext_q_norm_weight;    //  5 F32
static Fake_Tensor<HEAD_DIM,                        int>   ext_k_norm_weight;    //  6 F32
static Fake_Tensor<USER_BATCH,                      int>   ext_seq_lens;         //  7 I32
static Fake_Tensor<USER_BATCH * MAX_BLOCKS_PER_SEQ, int>   ext_block_table;      //  8 I32
static Fake_Tensor<USER_BATCH,                      int>   ext_slot_mapping;     //  9 I32
static Fake_Tensor<MAX_SEQ * HEAD_DIM,              int>   ext_rope_cos;         // 10 F32
static Fake_Tensor<MAX_SEQ * HEAD_DIM,              int>   ext_rope_sin;         // 11 F32
static Fake_Tensor<CACHE_ROWS * HEAD_DIM,           short> ext_k_cache;          // 12 BF16
static Fake_Tensor<CACHE_ROWS * HEAD_DIM,           short> ext_v_cache;          // 13 BF16
static Fake_Tensor<HIDDEN * HIDDEN,                 short> ext_wo;               // 14 BF16
static Fake_Tensor<HIDDEN,                          int>   ext_post_rms_weight;  // 15 F32
static Fake_Tensor<HIDDEN * INTERMEDIATE,           short> ext_w_gate;           // 16 BF16
static Fake_Tensor<HIDDEN * INTERMEDIATE,           short> ext_w_up;             // 17 BF16
static Fake_Tensor<INTERMEDIATE * HIDDEN,           short> ext_w_down;           // 18 BF16
static Fake_Tensor<USER_BATCH * HIDDEN,             short> ext_out;              // 19 BF16

/* ========================== Intermediate tensors ==========================
 * Shapes and dtypes per the original qwen3_decode.cpp TensorCreateInfo calls.
 * Per-iteration buffers (normed_tile / all_raw_scores / ...) get one global
 * each; iterations conceptually reuse the same pool slot.
 * ==========================================================================*/
static Fake_Tensor<ALL_Q_PADDED_ROWS * HEAD_DIM, short> all_q_padded;
static Fake_Tensor<BATCH_PADDED * HIDDEN,        int>   q_proj;
static Fake_Tensor<BATCH_PADDED * KV_HIDDEN,     int>   k_proj;
static Fake_Tensor<BATCH_PADDED * KV_HIDDEN,     int>   v_proj;
static Fake_Tensor<BATCH_PADDED * HIDDEN,        int>   q_proj_norm;
static Fake_Tensor<BATCH_PADDED * KV_HIDDEN,     int>   k_proj_norm;
static Fake_Tensor<BATCH_PADDED * HIDDEN,        short> attn_out;

static Fake_Tensor<BATCH_TILE * HIDDEN,          short> normed_tile;
static Fake_Tensor<BATCH_TILE * HIDDEN,          int>   resid1_tile;
static Fake_Tensor<GM_PIPE_BUFFER_SIZE,          int>   gm_pipe_buffer_0;
static Fake_Tensor<BATCH_TILE * HIDDEN,          short> post_norm_tile;
static Fake_Tensor<BATCH_TILE * INTERMEDIATE,    short> mlp_tile;

static Fake_Tensor<ATTN_SCRATCH_ROWS * HEAD_DIM, int>   all_raw_scores;
static Fake_Tensor<ATTN_SCRATCH_ROWS * HEAD_DIM, short> all_exp_padded;
static Fake_Tensor<ATTN_SCRATCH_ROWS,            int>   all_cur_mi;
static Fake_Tensor<ATTN_SCRATCH_ROWS,            int>   all_cur_li;
static Fake_Tensor<ATTN_SCRATCH_ROWS * HEAD_DIM, int>   all_oi_tmp;

static Fake_Tensor<BATCH_TILE * MLP_OUT_CHUNK,   int>   ret0__out;
static Fake_Tensor<BATCH_TILE * MLP_OUT_CHUNK,   int>   ret0__out_1;
static Fake_Tensor<BATCH_TILE * DOWN_OUT_CHUNK,  int>   fp32_chunk_gm;

/* ========================== Global task-id / runtime-info arrays ==========
 * Replace the previous std::vector storage. Sized to cover every iteration of
 * every loop so pointers handed to task_desc.data stay valid for the whole
 * orchestration build.
 * ==========================================================================*/
static task_id_t g_q_proj_id_per_tile [NUM_TILES];
static task_id_t g_k_proj_id_per_tile [NUM_TILES];
static task_id_t g_v_proj_id_per_tile [NUM_TILES];
static task_id_t g_qk_norm_id_per_tile[NUM_TILES];

static task_id_t g_online_softmax_ids_per_b[USER_BATCH][ONLINE_SOFTMAX_LAUNCHES];
static size_t    g_online_softmax_count_per_b[USER_BATCH];

static fake_runtime_info g_ri_q_per_chunk [NUM_TILES * Q_CHUNKS_PER_TILE];
static fake_runtime_info g_ri_k_per_chunk [NUM_TILES * KV_CHUNKS_PER_TILE];
static fake_runtime_info g_ri_v_per_chunk [NUM_TILES * KV_CHUNKS_PER_TILE];
static fake_runtime_info g_ri_os_per_chunk[USER_BATCH * ONLINE_SOFTMAX_LAUNCHES];

static task_id_t         g_silu_id_by_ob[NUM_OB];
static fake_runtime_info g_ri_mlp_storage [NUM_TILES * NUM_OB  * 3];
static fake_runtime_info g_ri_down_storage[NUM_TILES * NUM_DOB * 2];

/* ========================== Kernel symbols ================================
 * Each task_desc.kernel points to one of these. Real kernels link in their
 * place; here they are empty stubs so the example builds standalone.
 * ==========================================================================*/
namespace {
void k_rmsnorm()             {}   // id 0
void k_q_proj()              {}   // id 1
void k_k_proj()              {}   // id 2
void k_v_proj()              {}   // id 3
void k_qk_norm()             {}   // id 4
void k_rope_kv_cache()       {}   // id 5
void k_qk_matmul()           {}   // id 6
void k_softmax()             {}   // id 7
void k_sv_matmul()           {}   // id 8
void k_online_softmax()      {}   // id 9
void k_out_proj_residual()   {}   // id 10/11 (MIX, single kernel)
void k_post_rmsnorm()        {}   // id 12
void k_gate_proj()           {}   // id 13
void k_up_proj()             {}   // id 14
void k_silu()                {}   // id 15
void k_down_proj()           {}   // id 16
void k_down_proj_residual()  {}   // id 17

template <typename F>
inline void *kptr(F *fn) { return reinterpret_cast<void *>(fn); }
}  // namespace

int main() {
    static_assert(EXPECTED_ARG_COUNT == 20, "ext tensor count must stay in sync");

    FAKE_MANUAL_SCOPE();

    // ===== Tile loop 1: rmsnorm + q/k/v_proj + qk_norm =====
    for (size_t i = 0; i < NUM_TILES; ++i) {
        g_q_proj_id_per_tile [i] = FAKE_TASK_ID_INVALID;
        g_k_proj_id_per_tile [i] = FAKE_TASK_ID_INVALID;
        g_v_proj_id_per_tile [i] = FAKE_TASK_ID_INVALID;
        g_qk_norm_id_per_tile[i] = FAKE_TASK_ID_INVALID;
    }

    for (int64_t b0 = 0; b0 < BATCH_PADDED; b0 += BATCH_TILE) {
        const size_t tix = static_cast<size_t>(b0 / BATCH_TILE);
        const int64_t cur_valid = std::min<int64_t>(USER_BATCH - b0, BATCH_TILE);

        // ---- Task 0: rmsnorm (SINGLE VECTOR) ----
        fake_runtime_info ri_rms;
        ri_rms.inputs  = {ext_hidden_states.data, ext_input_rms_weight.data};
        ri_rms.outputs = {normed_tile.data};
        ri_rms.scalars = {b0, cur_valid};
        task_id_t rmsnorm_id = fake_next_task_id();
        task_desc d_rms = {
            /*id=*/    rmsnorm_id,
            /*type=*/  TASK_TYPE_VECTOR,
            /*mode=*/  ORG_MODE_SINGLE,
            /*kernel=*/kptr(&k_rmsnorm),
            /*index=*/ 0,
            /*count=*/ 1,
            /*prio=*/  0,
            /*data=*/  &ri_rms,
        };
        fake_submit(d_rms);

        // ---- Task 1: q_proj (SINGLE CUBE x HIDDEN/Q_OUT_CHUNK chunks) ----
        for (int64_t q0 = 0; q0 < HIDDEN; q0 += Q_OUT_CHUNK) {
            const size_t gi = tix * Q_CHUNKS_PER_TILE + static_cast<size_t>(q0 / Q_OUT_CHUNK);
            auto& ri = g_ri_q_per_chunk[gi];
            ri.inputs  = {normed_tile.data, ext_wq.data};
            ri.outputs = {q_proj.data};
            ri.scalars = {q0, b0};
            task_id_t id = fake_next_task_id();
            task_desc d = {id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                           kptr(&k_q_proj), 0, 1, 0, &ri};
            fake_add_dep(id, rmsnorm_id);
            fake_submit(d);
            g_q_proj_id_per_tile[tix] = id;
        }

        // ---- Task 2: k_proj (SINGLE CUBE x KV_HIDDEN/KV_OUT_CHUNK chunks) ----
        for (int64_t kv0 = 0; kv0 < KV_HIDDEN; kv0 += KV_OUT_CHUNK) {
            const size_t gi = tix * KV_CHUNKS_PER_TILE + static_cast<size_t>(kv0 / KV_OUT_CHUNK);
            auto& ri = g_ri_k_per_chunk[gi];
            ri.inputs  = {normed_tile.data, ext_wk.data};
            ri.outputs = {k_proj.data};
            ri.scalars = {kv0, b0};
            task_id_t id = fake_next_task_id();
            task_desc d = {id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                           kptr(&k_k_proj), 0, 1, 0, &ri};
            fake_add_dep(id, rmsnorm_id);
            fake_submit(d);
            g_k_proj_id_per_tile[tix] = id;
        }

        // ---- Task 3: v_proj ----
        for (int64_t kv0 = 0; kv0 < KV_HIDDEN; kv0 += KV_OUT_CHUNK) {
            const size_t gi = tix * KV_CHUNKS_PER_TILE + static_cast<size_t>(kv0 / KV_OUT_CHUNK);
            auto& ri = g_ri_v_per_chunk[gi];
            ri.inputs  = {normed_tile.data, ext_wv.data};
            ri.outputs = {v_proj.data};
            ri.scalars = {kv0, b0};
            task_id_t id = fake_next_task_id();
            task_desc d = {id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                           kptr(&k_v_proj), 0, 1, 0, &ri};
            fake_add_dep(id, rmsnorm_id);
            fake_submit(d);
            g_v_proj_id_per_tile[tix] = id;
        }

        // ---- Task 4: qk_norm (SINGLE VECTOR) ----
        fake_runtime_info ri_qkn;
        ri_qkn.inputs  = {q_proj.data, ext_q_norm_weight.data,
                          k_proj.data, ext_k_norm_weight.data};
        ri_qkn.outputs = {k_proj_norm.data, q_proj_norm.data};
        ri_qkn.scalars = {b0};
        task_id_t qk_norm_id = fake_next_task_id();
        task_desc d_qkn = {qk_norm_id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                           kptr(&k_qk_norm), 0, 1, 0, &ri_qkn};
        fake_add_deps(qk_norm_id, {
            g_q_proj_id_per_tile[tix],
            g_k_proj_id_per_tile[tix],
            g_v_proj_id_per_tile[tix],
        });
        fake_submit(d_qkn);
        g_qk_norm_id_per_tile[tix] = qk_norm_id;
    }

    // ===== Per-batch loop =====
    for (int64_t b = 0; b < USER_BATCH; b += 1) {
        // Runtime tensor-data lookups (seq_lens[b], slot_mapping[b]).
        int32_t ctx_len = ext_seq_lens.data[b];
        int32_t slot    = ext_slot_mapping.data[b];
        int64_t pos        = (static_cast<int64_t>(ctx_len) - 1);
        int64_t ctx_blocks = ((static_cast<int64_t>(ctx_len) + BLOCK_SIZE - 1) / BLOCK_SIZE);
        int64_t block_table_base = (b * MAX_BLOCKS_PER_SEQ);
        int64_t slot_block  = (static_cast<int64_t>(slot) / BLOCK_SIZE);
        int64_t slot_offset = (static_cast<int64_t>(slot) - (slot_block * BLOCK_SIZE));

        // cos/sin row + lo/hi half pointers (replaces Tensor::view from PTO2).
        void* cos_lo_ptr = &ext_rope_cos.data[pos * HEAD_DIM + 0];
        void* cos_hi_ptr = &ext_rope_cos.data[pos * HEAD_DIM + HALF_HEAD_DIM];
        void* sin_lo_ptr = &ext_rope_sin.data[pos * HEAD_DIM + 0];
        void* sin_hi_ptr = &ext_rope_sin.data[pos * HEAD_DIM + HALF_HEAD_DIM];

        // ---- Task 5: rope_kv_cache (SINGLE VECTOR) ----
        fake_runtime_info ri_rope;
        ri_rope.inputs  = {k_proj_norm.data, cos_lo_ptr, sin_lo_ptr,
                           cos_hi_ptr, sin_hi_ptr, v_proj.data, q_proj_norm.data};
        ri_rope.outputs = {all_q_padded.data, ext_k_cache.data, ext_v_cache.data};
        ri_rope.scalars = {slot_block, slot_offset, b};
        task_id_t rope_kv_id = fake_next_task_id();
        task_desc d_rope = {rope_kv_id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                            kptr(&k_rope_kv_cache), 0, 1, 0, &ri_rope};
        fake_add_dep(rope_kv_id, g_qk_norm_id_per_tile[static_cast<size_t>(b / BATCH_TILE)]);
        fake_submit(d_rope);

        // attn_out row pointer (one HIDDEN-wide row at batch index b).
        void* attn_row_ptr = &attn_out.data[b * HIDDEN];

        // ---- Task 6: qk_matmul (SPMD CUBE, count = SPMD_BLOCK_NUM) ----
        fake_runtime_info ri_qkm;
        ri_qkm.inputs  = {all_q_padded.data, ext_block_table.data, ext_k_cache.data};
        ri_qkm.outputs = {all_raw_scores.data};
        ri_qkm.scalars = {b, ctx_blocks, block_table_base};
        task_id_t qk_matmul_id = fake_next_task_id();
        task_desc d_qkm = {qk_matmul_id, TASK_TYPE_CUBE, ORG_MODE_SPMD_ASYNC,
                           kptr(&k_qk_matmul), 0, SPMD_BLOCK_NUM, 0, &ri_qkm};
        fake_add_dep(qk_matmul_id, rope_kv_id);
        fake_submit(d_qkm);

        // ---- Task 7: softmax (SPMD VECTOR, count = SPMD_BLOCK_NUM) ----
        fake_runtime_info ri_sm;
        ri_sm.inputs  = {all_raw_scores.data};
        ri_sm.outputs = {all_cur_li.data, all_cur_mi.data, all_exp_padded.data};
        ri_sm.scalars = {ctx_blocks, ctx_len};
        task_id_t softmax_id = fake_next_task_id();
        task_desc d_sm = {softmax_id, TASK_TYPE_VECTOR, ORG_MODE_SPMD_ASYNC,
                          kptr(&k_softmax), 0, SPMD_BLOCK_NUM, 0, &ri_sm};
        fake_add_dep(softmax_id, qk_matmul_id);
        fake_submit(d_sm);

        // ---- Task 8: sv_matmul (SPMD CUBE, count = SPMD_BLOCK_NUM) ----
        fake_runtime_info ri_sv;
        ri_sv.inputs  = {ext_block_table.data, all_exp_padded.data, ext_v_cache.data};
        ri_sv.outputs = {all_oi_tmp.data};
        ri_sv.scalars = {ctx_blocks, block_table_base};
        task_id_t sv_matmul_id = fake_next_task_id();
        task_desc d_sv = {sv_matmul_id, TASK_TYPE_CUBE, ORG_MODE_SPMD_ASYNC,
                          kptr(&k_sv_matmul), 0, SPMD_BLOCK_NUM, 0, &ri_sv};
        fake_add_deps(sv_matmul_id, {rope_kv_id, softmax_id});
        fake_submit(d_sv);

        // ---- Task 9: online_softmax (SINGLE VECTOR x ONLINE_SOFTMAX_LAUNCHES) ----
        size_t os_count = 0;
        for (int64_t gi0 = 0; gi0 < ONLINE_SOFTMAX_HEADS; gi0 += ONLINE_SOFTMAX_STRIDE) {
            const size_t gi = static_cast<size_t>(b) * ONLINE_SOFTMAX_LAUNCHES + os_count;
            auto& ri = g_ri_os_per_chunk[gi];
            ri.inputs  = {all_oi_tmp.data, all_cur_mi.data, all_cur_li.data};
            ri.outputs = {attn_row_ptr};
            ri.scalars = {gi0, ctx_blocks};
            task_id_t id = fake_next_task_id();
            task_desc d = {id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                           kptr(&k_online_softmax), 0, 1, 0, &ri};
            fake_add_dep(id, sv_matmul_id);
            fake_submit(d);
            g_online_softmax_ids_per_b[static_cast<size_t>(b)][os_count] = id;
            ++os_count;
        }
        g_online_softmax_count_per_b[static_cast<size_t>(b)] = os_count;
    }

    // ===== Tile loop 2: out_proj_mixed -> post_rmsnorm -> mlp -> down_proj_residual =====
    for (int64_t b0 = 0; b0 < BATCH_PADDED; b0 += BATCH_TILE) {
        const size_t tix = static_cast<size_t>(b0 / BATCH_TILE);
        const int64_t cur_valid = std::min<int64_t>(USER_BATCH - b0, BATCH_TILE);

        // ---- Task 10 (MIX out_proj_residual, SPMD count = MIX_BLOCK_NUM) ----
        fake_runtime_info ri_op;
        ri_op.inputs  = {ext_hidden_states.data, attn_out.data, ext_wo.data};
        ri_op.inouts  = {resid1_tile.data};
        ri_op.outputs = {gm_pipe_buffer_0.data};
        ri_op.scalars = {b0, cur_valid};
        task_id_t out_proj_id = fake_next_task_id();
        task_desc d_op = {out_proj_id, TASK_TYPE_MIX, ORG_MODE_SPMD_ASYNC,
                          kptr(&k_out_proj_residual), 0, MIX_BLOCK_NUM, 0, &ri_op};
        for (int64_t row = 0; row < cur_valid; ++row) {
            const int64_t bb = b0 + row;
            const size_t  bs = static_cast<size_t>(bb);
            const size_t  n  = g_online_softmax_count_per_b[bs];
            fake_add_deps_range(out_proj_id,
                                &g_online_softmax_ids_per_b[bs][0],
                                &g_online_softmax_ids_per_b[bs][0] + n);
        }
        fake_submit(d_op);

        // ---- Task 12: post_rmsnorm ----
        fake_runtime_info ri_post;
        ri_post.inputs  = {resid1_tile.data, ext_post_rms_weight.data};
        ri_post.outputs = {post_norm_tile.data};
        task_id_t post_rmsnorm_id = fake_next_task_id();
        task_desc d_post = {post_rmsnorm_id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                            kptr(&k_post_rmsnorm), 0, 1, 0, &ri_post};
        fake_add_dep(post_rmsnorm_id, out_proj_id);
        fake_submit(d_post);

        // ---- MLP gate / up / silu loop (per-ob, SINGLE tasks) ----
        for (size_t i = 0; i < NUM_OB; ++i) {
            g_silu_id_by_ob[i] = FAKE_TASK_ID_INVALID;
        }
        for (int64_t ob = 0; ob < NUM_OB; ob += 1) {
            int64_t mlp_o0 = (ob * MLP_OUT_CHUNK);
            const size_t mlp_base = (tix * NUM_OB + static_cast<size_t>(ob)) * 3;

            // Task 13: gate_proj
            auto& ri_gate = g_ri_mlp_storage[mlp_base + 0];
            ri_gate.inputs  = {post_norm_tile.data, ext_w_gate.data};
            ri_gate.outputs = {ret0__out.data};
            ri_gate.scalars = {mlp_o0};
            task_id_t gate_id = fake_next_task_id();
            task_desc d_gate = {gate_id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                                kptr(&k_gate_proj), 0, 1, 0, &ri_gate};
            fake_add_dep(gate_id, post_rmsnorm_id);
            fake_submit(d_gate);

            // Task 14: up_proj
            auto& ri_up = g_ri_mlp_storage[mlp_base + 1];
            ri_up.inputs  = {post_norm_tile.data, ext_w_up.data};
            ri_up.outputs = {ret0__out_1.data};
            ri_up.scalars = {mlp_o0};
            task_id_t up_id = fake_next_task_id();
            task_desc d_up = {up_id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                              kptr(&k_up_proj), 0, 1, 0, &ri_up};
            fake_add_dep(up_id, post_rmsnorm_id);
            fake_submit(d_up);

            // mlp_tile column slice (start of column mlp_o0).
            void* ret0__out_2_ptr = &mlp_tile.data[mlp_o0];

            // Task 15: silu
            auto& ri_silu = g_ri_mlp_storage[mlp_base + 2];
            ri_silu.inputs  = {ret0__out.data, ret0__out_1.data};
            ri_silu.outputs = {ret0__out_2_ptr};
            task_id_t silu_id = fake_next_task_id();
            task_desc d_silu = {silu_id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                                kptr(&k_silu), 0, 1, 0, &ri_silu};
            fake_add_deps(silu_id, {gate_id, up_id});
            fake_submit(d_silu);
            g_silu_id_by_ob[static_cast<size_t>(ob)] = silu_id;
        }

        // ---- down_proj + down_proj_residual loop (per-dob, SINGLE tasks) ----
        for (int64_t dob = 0; dob < NUM_DOB; dob += 1) {
            int64_t d0 = (dob * DOWN_OUT_CHUNK);
            const size_t down_base = (tix * NUM_DOB + static_cast<size_t>(dob)) * 2;

            // Task 16: down_proj
            auto& ri_down = g_ri_down_storage[down_base + 0];
            ri_down.inputs  = {mlp_tile.data, ext_w_down.data};
            ri_down.inouts  = {fp32_chunk_gm.data};
            ri_down.scalars = {d0};
            task_id_t down_proj_id = fake_next_task_id();
            task_desc d_down = {down_proj_id, TASK_TYPE_CUBE, ORG_MODE_SINGLE,
                                kptr(&k_down_proj), 0, 1, 0, &ri_down};
            fake_add_deps_range(down_proj_id,
                                &g_silu_id_by_ob[0],
                                &g_silu_id_by_ob[0] + NUM_OB);
            fake_submit(d_down);

            // Task 17: down_proj_residual
            auto& ri_dr = g_ri_down_storage[down_base + 1];
            ri_dr.inputs  = {fp32_chunk_gm.data, resid1_tile.data};
            ri_dr.outputs = {ext_out.data};
            ri_dr.scalars = {d0, cur_valid, b0};
            task_id_t dr_id = fake_next_task_id();
            task_desc d_dr = {dr_id, TASK_TYPE_VECTOR, ORG_MODE_SINGLE,
                              kptr(&k_down_proj_residual), 0, 1, 0, &ri_dr};
            fake_add_deps(dr_id, {down_proj_id, out_proj_id});
            fake_submit(d_dr);
        }
    }

    return 0;
}
