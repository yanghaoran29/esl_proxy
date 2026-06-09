/*
 * test_tensormap.c - PTO2-aligned tensormap self-test + micro-benchmarks.
 */

#define _POSIX_C_SOURCE 200809L

#include "tensor.h"
#include "tensormap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static _Alignas(128) uint8_t g_buf[1u << 20];
static _Alignas(128) uint8_t g_buf2[1u << 20];

static TmConfig make_config(void) {
  TmConfig cfg = {0};
  cfg.num_buckets = 16;
  cfg.pool_size = 64;
  cfg.num_rings = 1;
  cfg.task_window[0] = 64;
  return cfg;
}

static TmRegion_onlytest region_1d(uint64_t base, uint64_t off, uint64_t len,
                          uint64_t storage) {
  TmRegion_onlytest r = {0};
  r.base_addr = base;
  r.storage_numel = storage;
  r.elem_size = 4;
  r.ndims = 1;
  r.start_offset = off;
  r.shapes[0] = (uint32_t)len;
  r.strides[0] = 1;
  return r;
}

static TmRegion_onlytest whole_buffer_region(uint64_t base) {
  TmRegion_onlytest r = {0};
  r.base_addr = base;
  r.storage_numel = 1;
  r.elem_size = 1;
  r.ndims = 2;
  r.start_offset = 0;
  r.shapes[0] = 1;
  r.shapes[1] = 1;
  r.strides[0] = 1;
  r.strides[1] = 1;
  return r;
}

static TmRegion_onlytest region_2d(uint64_t base, uint32_t stor_d0, uint32_t stor_d1,
                          uint32_t row0, uint32_t nrows, uint32_t d1,
                          uint32_t elem_size) {
  TmRegion_onlytest r;
  memset(&r, 0, sizeof r);
  r.base_addr = base;
  r.storage_numel = (uint64_t)stor_d0 * stor_d1;
  r.elem_size = (uint16_t)elem_size;
  r.ndims = 2;
  r.start_offset = (uint64_t)row0 * d1;
  r.shapes[0] = nrows;
  r.shapes[1] = d1;
  r.strides[0] = d1;
  r.strides[1] = 1;
  return r;
}

static Tensor tensor_from_region(const TmRegion_onlytest *r) {
  Tensor t;

  t.buffer_addr = r->base_addr;
  t.buffer_size = r->storage_numel * (uint64_t)r->elem_size;
  t.owner_task_id = 0;
  t.start_offset = r->start_offset;
  t.version = r->version;
  t.ndims = r->ndims;
  t.dtype = (uint8_t)r->elem_size;
  t.manual_dep = 0;
  t.is_contiguous = 1;
  t._pad_cl1 = 0;
  t.extent_elem_cache = 0;
  for (uint32_t i = 0; i < ESL_PROXY_TENSOR_MAX_DIMS; i++) {
    t.shapes[i] = 0;
    t.strides[i] = 0;
  }
  for (uint32_t i = 0; i < 36; i++) {
    t._pad_cl2[i] = 0;
  }
  uint64_t extent = 1;
  for (uint32_t i = 0; i < r->ndims; i++) {
    t.shapes[i] = r->shapes[i];
    t.strides[i] = r->strides[i];
    extent *= r->shapes[i];
  }
  t.extent_elem_cache = extent;
  return t;
}

typedef struct {
  uint64_t producers[32];
  TmOverlap statuses[32];
  int count;
} HitSink;

static bool collect_cb(TmEntry *e, TmOverlap st, void *ctx) {
  HitSink *s = (HitSink *)ctx;
  s->producers[s->count] = e->producer_id;
  s->statuses[s->count] = st;
  s->count++;
  return true;
}

static HitSink collect(TmTensorMap *map, TmRegion_onlytest probe) {
  HitSink s = {0};
  Tensor t = tensor_from_region(&probe);
  tm_lookup_onlytest(map, &t, collect_cb, &s);
  return s;
}

static bool remove_cb(TmEntry *e, TmOverlap st, void *ctx) {
  (void)st;
  tm_remove((TmTensorMap *)ctx, e);
  return true;
}

static bool sink_has_local(const HitSink *s, uint32_t local) {
  for (int i = 0; i < s->count; i++) {
    if (tm_local_of(s->producers[i]) == local) {
      return true;
    }
  }
  return false;
}

static void test_overlap_semantics(void) {
  printf("Test: overlap_semantics\n");
  TmConfig cfg = make_config();
  assert(tm_bytes_required(&cfg) <= sizeof(g_buf));
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest a = region_1d(0x1000, 0, 128, 128);
  tm_insert_onlytest(&map, &a, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0x1000, 0, 128, 128));
  assert(s.count == 1);
  assert(tm_local_of(s.producers[0]) == 1);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  tm_init(&map, g_buf, &cfg);
  TmRegion_onlytest b = region_1d(0x2000, 0, 128, 256);
  tm_insert_onlytest(&map, &b, tm_make_id(0, 2));
  s = collect(&map, region_1d(0x2000, 128, 128, 256));
  assert(s.count == 0);

  tm_init(&map, g_buf, &cfg);
  TmRegion_onlytest c = region_1d(0x3000, 0, 128, 256);
  tm_insert_onlytest(&map, &c, tm_make_id(0, 3));
  s = collect(&map, region_1d(0x3000, 64, 128, 256));
  assert(s.count == 1);
  assert(s.statuses[0] == TM_OVERLAP_OTHER);

  tm_init(&map, g_buf, &cfg);
  TmRegion_onlytest d = region_1d(0x4000, 0, 128, 128);
  tm_insert_onlytest(&map, &d, tm_make_id(0, 4));
  s = collect(&map, region_1d(0x5000, 0, 128, 128));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_l1_fast_reject(void) {
  printf("Test: l1_fast_reject\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest prod = region_1d(0xB000, 0, 64, 256);
  tm_insert_onlytest(&map, &prod, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0xB000, 200, 32, 256));
  assert(s.count == 0);

  printf("  PASSED\n");
}

static void test_version_guard(void) {
  printf("Test: version_guard\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest prod = region_1d(0xB100, 0, 64, 64);
  prod.version = 0;
  tm_insert_onlytest(&map, &prod, tm_make_id(0, 1));

  TmRegion_onlytest probe = region_1d(0xB100, 0, 64, 64);
  probe.version = 1;
  HitSink s = collect(&map, probe);
  assert(s.count == 1);
  assert(s.statuses[0] == TM_OVERLAP_OTHER);

  printf("  PASSED\n");
}

static void test_lazy_invalidation_and_reuse(void) {
  printf("Test: lazy_invalidation_and_reuse\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest r = region_1d(0x6000, 0, 128, 128);
  tm_insert_onlytest(&map, &r, tm_make_id(0, 5));
  assert(tm_valid_count(&map) == 1);

  tm_sync(&map, 0, 6);
  assert(tm_valid_count(&map) == 0);
  assert(collect(&map, region_1d(0x6000, 0, 128, 128)).count == 0);

  tm_init(&map, g_buf, &cfg);
  for (uint32_t local = 0; local < 8; local++) {
    TmRegion_onlytest rr = region_1d(0x7000, 0, 16, 128);
    tm_insert_onlytest(&map, &rr, tm_make_id(0, local));
  }
  tm_sync_tensormap(&map, 0, (int32_t)TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
  assert(tm_valid_count(&map) == 0);

  TmRegion_onlytest fresh = region_1d(0x7000, 0, 16, 128);
  tm_insert_onlytest(&map, &fresh, tm_make_id(0, 100));
  HitSink s = collect(&map, region_1d(0x7000, 0, 16, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 100);

  printf("  PASSED\n");
}

static void test_sync_interval_gating(void) {
  printf("Test: sync_interval_gating\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  for (uint32_t i = 0; i < 8; i++) {
    TmRegion_onlytest r = region_1d(0xE000, 0, 16, 128);
    tm_insert_onlytest(&map, &r, tm_make_id(0, i));
  }
  assert(tm_hdr(&map)->next_entry_idx == 8);
  assert(tm_hdr(&map)->free_num == 0);

  tm_sync_tensormap(&map, 0, 5, 1);
  assert(tm_hdr(&map)->last_cleanup[0] == 0);
  assert(tm_hdr(&map)->free_num == 0);

  tm_sync_tensormap(&map, 0, (int32_t)TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
  assert(tm_hdr(&map)->last_cleanup[0] == (int32_t)TM_CLEANUP_INTERVAL);
  assert(tm_hdr(&map)->free_num >= 8);

  printf("  PASSED\n");
}

static void test_remove_in_callback(void) {
  printf("Test: remove_in_callback\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest r = region_1d(0x8000, 0, 64, 64);
  tm_insert_onlytest(&map, &r, tm_make_id(0, 0));
  TmRegion_onlytest probe = region_1d(0x8000, 0, 64, 64);
  Tensor probe_t = tensor_from_region(&probe);
  tm_lookup_onlytest(&map, &probe_t, remove_cb, &map);
  assert(collect(&map, region_1d(0x8000, 0, 64, 64)).count == 0);

  printf("  PASSED\n");
}

static void test_attach_relocated_image(void) {
  printf("Test: attach_relocated_image\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest r = region_1d(0x9000, 0, 128, 128);
  tm_insert_onlytest(&map, &r, tm_make_id(1 % cfg.num_rings, 7));

  uint64_t bytes = tm_bytes_required(&cfg);
  assert(bytes <= sizeof(g_buf2));
  memcpy(g_buf2, g_buf, bytes);
  TmTensorMap map2;
  tm_attach(&map2, g_buf2);

  HitSink s = collect(&map2, region_1d(0x9000, 0, 128, 128));
  assert(s.count == 1 && tm_local_of(s.producers[0]) == 7);
  assert(s.statuses[0] == TM_OVERLAP_COVERED);

  printf("  PASSED\n");
}

static void test_multi_producer_same_base(void) {
  printf("Test: multi_producer_same_base\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  TmRegion_onlytest p0 = region_1d(0xA000, 0, 64, 256);
  TmRegion_onlytest p1 = region_1d(0xA000, 0, 64, 256);
  tm_insert_onlytest(&map, &p0, tm_make_id(0, 0));
  tm_insert_onlytest(&map, &p1, tm_make_id(0, 1));
  HitSink s = collect(&map, region_1d(0xA000, 0, 64, 256));
  assert(s.count == 2);

  printf("  PASSED\n");
}

static void entry_from_tensor_view(const Tensor *t, TmEntry *e) {
  memset(e, 0, sizeof *e);
  e->start_offset = t->start_offset;
  e->version = t->version;
  e->ndims = t->ndims;
  e->elem_size = (uint16_t)t->dtype;
  e->is_contiguous = t->is_contiguous;
  memcpy(e->shapes, t->shapes, sizeof e->shapes);
  e->storage_numel = t->dtype != 0 ? t->buffer_size / (uint64_t)t->dtype : 0u;
  e->extent_elem_cache = t->extent_elem_cache;
  memcpy(e->strides, t->strides, sizeof e->strides);
}

static void test_qwen3_dim1_column_slice(void) {
  printf("Test: qwen3_dim1_column_slice (gate_tile dim=1 piece overlap)\n");
  const uint32_t tile_shapes[2] = {16, 17408};
  Tensor tile = tensor_from_base_layout(0xE0000, tile_shapes, 2, FLOAT32);
  Tensor prod0 = view(tile, 0, 0, 16, 512);
  Tensor cons1 = view(tile, 0, 512, 16, 512);

  assert(prod0.is_contiguous == 0);
  assert(prod0.strides[0] == 17408u && prod0.strides[1] == 1u);

  TmEntry entry;
  entry_from_tensor_view(&prod0, &entry);

  assert(tm_check_overlap(&cons1, &entry) == TM_OVERLAP_NONE);

  Tensor cons0 = view(tile, 0, 0, 16, 512);
  assert(tm_check_overlap(&cons0, &entry) == TM_OVERLAP_COVERED);

  printf("  PASSED\n");
}

static void test_2d_row_tile_overlap(void) {
  printf("Test: 2d_row_tile_overlap (qwen3-style tile rows)\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  const uint64_t base = 0xD0000;
  const uint32_t batch = 96, hidden = 5120, tile = 16;

  TmRegion_onlytest prod = region_2d(base, batch, hidden, 16, tile, hidden, 4);
  tm_insert_onlytest(&map, &prod, tm_make_id(0, 3));

  HitSink hit = collect(&map, region_2d(base, batch, hidden, 16, tile, hidden, 4));
  assert(hit.count == 1 && tm_local_of(hit.producers[0]) == 3);

  HitSink miss = collect(&map, region_2d(base, batch, hidden, 0, tile, hidden, 4));
  assert(miss.count == 0);

  HitSink part = collect(&map, region_2d(base, batch, hidden, 8, 16, hidden, 4));
  assert(part.count == 1 && part.statuses[0] == TM_OVERLAP_OTHER);

  printf("  PASSED\n");
}

static void test_coexistence_demo(void) {
  printf("Test: coexistence_demo\n");
  TmConfig cfg = make_config();
  TmTensorMap map;
  tm_init(&map, g_buf, &cfg);

  const uint64_t X = 0xBEEF000;
  const uint64_t Y = 0xCAFE000;

  TmRegion_onlytest rx = whole_buffer_region(X);
  TmRegion_onlytest ry = whole_buffer_region(Y);
  tm_insert_onlytest(&map, &rx, tm_make_id(0, 10));
  tm_insert_onlytest(&map, &rx, tm_make_id(0, 11));
  tm_insert_onlytest(&map, &ry, tm_make_id(0, 12));

  HitSink cx = collect(&map, whole_buffer_region(X));
  assert(cx.count == 2);
  assert(sink_has_local(&cx, 10) && sink_has_local(&cx, 11));

  HitSink cy = collect(&map, whole_buffer_region(Y));
  assert(cy.count == 1 && sink_has_local(&cy, 12));

  tm_sync(&map, 0, 12);
  assert(collect(&map, whole_buffer_region(X)).count == 0);
  assert(collect(&map, whole_buffer_region(Y)).count == 1);

  printf("  PASSED\n");
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t ceil_pow2(uint32_t x) {
  uint32_t p = 1;
  while (p < x) {
    p <<= 1;
  }
  return p;
}

static void *alloc_image(const TmConfig *cfg) {
  uint64_t bytes = tm_bytes_required(cfg);
  bytes = (bytes + 127u) & ~127u;
  void *p = aligned_alloc(128, bytes);
  assert(p != NULL);
  return p;
}

static void report(const char *name, double ops, double secs) {
  const double ns_per = ops > 0.0 ? (secs / ops) * 1e9 : 0.0;
  const double mops = secs > 0.0 ? (ops / secs) / 1e6 : 0.0;
  printf("  %-30s %11.0f ops  %8.2f ms  %8.2f ns/op  %9.2f Mops/s\n", name,
         ops, secs * 1e3, ns_per, mops);
}

static volatile uint64_t g_perf_sink;
static bool perf_count_cb(TmEntry *e, TmOverlap st, void *ctx) {
  (void)st;
  *(uint64_t *)ctx += e->producer_id;
  return true;
}

static uint64_t perf_rng(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

static void bench_insert(uint32_t n) {
  TmConfig cfg = {0};
  cfg.num_buckets = 65536;
  cfg.pool_size = n;
  cfg.num_rings = 1;
  cfg.task_window[0] = 1024;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  const double t0 = now_sec();
  for (uint32_t i = 0; i < n; i++) {
    TmRegion_onlytest r = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert_onlytest(&map, &r, tm_make_id(0, i & (cfg.task_window[0] - 1)));
  }
  report("insert", (double)n, now_sec() - t0);
  free(img);
}

static void bench_lookup(uint32_t m, uint32_t queries) {
  TmConfig cfg = {0};
  cfg.num_buckets = ceil_pow2(m);
  cfg.pool_size = m;
  cfg.num_rings = 1;
  cfg.task_window[0] = 1024;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  for (uint32_t i = 0; i < m; i++) {
    TmRegion_onlytest r = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    tm_insert_onlytest(&map, &r, tm_make_id(0, i));
  }

  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  uint64_t acc = 0;
  const double t0 = now_sec();
  for (uint32_t q = 0; q < queries; q++) {
    const uint32_t i = (uint32_t)(perf_rng(&rng) % m);
    TmRegion_onlytest probe = region_1d(0x100000u + (uint64_t)i * 256u, 0, 64, 64);
    Tensor probe_t = tensor_from_region(&probe);
    tm_lookup_onlytest(&map, &probe_t, perf_count_cb, &acc);
  }
  g_perf_sink = acc;
  report("lookup (covered hit)", (double)queries, now_sec() - t0);
  free(img);
}

static void bench_submit(uint32_t window, uint32_t per_task, uint32_t iters) {
  TmConfig cfg = {0};
  cfg.num_buckets = 4096;
  cfg.pool_size = (window + 2) * per_task;
  cfg.num_rings = 1;
  cfg.task_window[0] = window;
  void *img = alloc_image(&cfg);
  TmTensorMap map;
  tm_init(&map, img, &cfg);

  uint32_t cur = 0;
  for (uint32_t w = 0; w < window; w++, cur++) {
    for (uint32_t k = 0; k < per_task; k++) {
      TmRegion_onlytest r =
          region_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert_onlytest(&map, &r, tm_make_id(0, cur));
    }
  }

  const double t0 = now_sec();
  for (uint32_t i = 0; i < iters; i++, cur++) {
    for (uint32_t k = 0; k < per_task; k++) {
      TmRegion_onlytest r =
          region_1d(0x200000u + (((uint64_t)cur * per_task + k) & 4095u) * 256u,
                    0, 64, 64);
      tm_insert_onlytest(&map, &r, tm_make_id(0, cur));
    }
    tm_sync_tensormap(&map, 0, (int32_t)(cur - window + 1), cur);
  }
  report("submit (insert+sync+cleanup)", (double)iters, now_sec() - t0);
  free(img);
}

static void test_kv_cache_view_offset_matches_bind(void) {
  printf("Test: kv_cache_view_offset_matches_bind\n");
  const uint64_t base = 0x8800000u;
  const uint32_t user_batch = 90u;
  const uint32_t kv_shapes[2] = {2949120u, 128u};
  Tensor ext_k = tensor_from_base_layout(base, kv_shapes, 2, BFLOAT16);

  for (uint32_t b = 0; b < user_batch; b++) {
    const uint32_t bind_shapes[2] = {8u, 128u};
    const uint64_t bind_base =
        base + (uint64_t)b * 1024u * (uint64_t)BFLOAT16;
    Tensor bound = tensor_from_base_layout(bind_base, bind_shapes, 2, BFLOAT16);
    Tensor viewed = view(ext_k, b * 8u, 0u, 8u, 128u);

    assert(tensor_data_addr(&bound) == tensor_data_addr(&viewed));
    assert(viewed.shapes[0] == 8u && viewed.shapes[1] == 128u);
    assert(viewed.start_offset == (uint64_t)b * 1024u);
  }
  printf("  PASSED\n");
}

static Tensor bench_kv_bind_slice(uint64_t base, uint32_t b) {
  const uint32_t shapes[2] = {8u, 128u};
  const uint64_t slice_base = base + (uint64_t)b * 1024u * (uint64_t)BFLOAT16;
  return tensor_from_base_layout(slice_base, shapes, 2, BFLOAT16);
}

static void bench_kv_cache_bind_vs_view(uint32_t rounds) {
  const uint64_t base = 0x9000000u;
  const uint32_t user_batch = 90u;
  const uint32_t kv_shapes[2] = {2949120u, 128u};
  Tensor ext_k = tensor_from_base_layout(base, kv_shapes, 2, BFLOAT16);
  volatile uint64_t sink = 0;

  const double t_bind = now_sec();
  for (uint32_t r = 0; r < rounds; r++) {
    for (uint32_t b = 0; b < user_batch; b++) {
      Tensor local = bench_kv_bind_slice(base, b);
      sink += tensor_data_addr(&local);
      sink += local.shapes[0] + local.shapes[1];
    }
  }
  const double bind_secs = now_sec() - t_bind;

  const double t_view = now_sec();
  for (uint32_t r = 0; r < rounds; r++) {
    for (uint32_t b = 0; b < user_batch; b++) {
      Tensor local = view(ext_k, b * 8u, 0u, 8u, 128u);
      sink += tensor_data_addr(&local);
      sink += local.shapes[0] + local.shapes[1];
    }
  }
  const double view_secs = now_sec() - t_view;
  g_perf_sink = sink;

  const double ops = (double)rounds * (double)user_batch;
  report("kv_cache bind (make_2d style)", ops, bind_secs);
  report("kv_cache view (batch slice)", ops, view_secs);
  if (view_secs > 0.0) {
    const double ratio = bind_secs / view_secs;
    printf("  view vs bind speedup: %.3fx (bind/view)\n", ratio);
  }
}

static void run_benchmarks(uint32_t scale) {
  printf("=== Performance (scale=%u) ===\n", scale);
  bench_insert(200000u * scale);
  bench_lookup(100000u, 1000000u * scale);
  bench_submit(1024u, 4u, 200000u * scale);
  bench_kv_cache_bind_vs_view(200000u * scale);
  printf("\n");
}

int main(int argc, char **argv) {
  printf("=== TensorMap PTO2-aligned Tests ===\n\n");

  test_overlap_semantics();
  test_l1_fast_reject();
  test_version_guard();
  test_lazy_invalidation_and_reuse();
  test_sync_interval_gating();
  test_remove_in_callback();
  test_attach_relocated_image();
  test_multi_producer_same_base();
  test_2d_row_tile_overlap();
  test_qwen3_dim1_column_slice();
  test_kv_cache_view_offset_matches_bind();
  test_coexistence_demo();

  printf("\n=== All Tests Passed ===\n\n");

  uint32_t scale = 1;
  if (argc > 1) {
    unsigned long s = strtoul(argv[1], NULL, 10);
    if (s >= 1 && s <= 1000) {
      scale = (uint32_t)s;
    }
  }
  run_benchmarks(scale);
  return 0;
}
