/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under
 * the terms and conditions of CANN Open Software License Agreement Version 2.0
 * (the "License"). Please refer to the License for details. You may not use
 * this file except in compliance with the License. THIS SOFTWARE IS PROVIDED ON
 * AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS
 * FOR A PARTICULAR PURPOSE. See LICENSE in the root of the software repository
 * for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */

/**
 * tensormap.h — PTO2-aligned producer-lookup map (pure C).
 *
 * Aligned with simpler PTO2TensorMap:
 *   - 128B cache-line TmEntry (relocatable pool indices, not pointers)
 *   - 2D overlap: L1 element-range fast reject + L2 row-major check + L3 OTHER
 *   - Periodic cleanup (TM_CLEANUP_INTERVAL) on tm_sync_tensormap
 */

#ifndef ESL_PROXY_TENSORMAP_H
#define ESL_PROXY_TENSORMAP_H

#ifdef USE_TENSORMAP

#include "conf.h"

#include "tensor.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TM_MAX_DIMS = 5,
    TM_MAX_RINGS = 8
};

#ifndef TM_CLEANUP_INTERVAL
#define TM_CLEANUP_INTERVAL 64
#endif

#define TM_REGION_ALIGN 64u
#define TM_ENTRY_ALIGN 128u

typedef struct TmConfig {
    uint32_t num_buckets;
    uint32_t pool_size;
    uint32_t num_rings;
    uint32_t task_window[TM_MAX_RINGS];
} TmConfig;

typedef struct TmRegion_onlytest {
    uint64_t base_addr;
    uint32_t storage_numel;
    uint16_t elem_size;
    int32_t version;
    uint32_t ndims;
    uint64_t start_offset;
    uint32_t shapes[TM_MAX_DIMS];
    uint32_t strides[TM_MAX_DIMS];
} TmRegion_onlytest;

typedef enum TmOverlap {
    TM_OVERLAP_NONE = 0,
    TM_OVERLAP_COVERED = 1,
    TM_OVERLAP_OTHER = 2
} TmOverlap;

/**
 * TmEntry — 128B (2 cache lines), mirrors PTO2TensorMapEntry layout semantics.
 * Hash-bucket and per-task links are pool indices (-1 = none) for relocatability.
 */
typedef struct {
    /* === cache line 1 (64B) === */
    uint64_t base_addr;
    int32_t next_in_bucket;
    int32_t _pad_nb;
    uint64_t producer_id;
    uint64_t start_offset;
    int32_t version;
    uint32_t ndims;
    uint16_t elem_size;
    uint8_t manual_dep;
    uint8_t is_contiguous;
    uint32_t shapes[TM_MAX_DIMS];

    /* === cache line 2 (64B) === */
    int32_t prev_in_bucket;
    int32_t next_in_task;
    int32_t prev_in_task;
    int32_t bucket_index;
    uint64_t storage_numel;
    uint64_t extent_elem_cache;
    uint32_t strides[TM_MAX_DIMS];
    uint32_t _line2_pad[3];
} TmEntry;

_Static_assert(sizeof(TmEntry) == 128, "TmEntry must be 128 bytes");

typedef struct TmHeader {
    TmConfig cfg;
    int32_t next_entry_idx;
    int32_t free_num;
    int32_t last_alive[TM_MAX_RINGS];
    int32_t last_cleanup[TM_MAX_RINGS];
    uint32_t off_buckets;
    uint32_t off_pool;
    uint32_t off_free;
    uint32_t off_task_heads[TM_MAX_RINGS];
} TmHeader;

typedef struct TmTensorMap {
    uint8_t *base;
} TmTensorMap;

#ifndef TM_PENDING_MAX_PRED
#define TM_PENDING_MAX_PRED 64u
#endif

/** Output of tm_submit lookup: predecessor task ids + INOUT remove policy. */
typedef struct {
    uint16_t consumer;
    uint16_t preds[TM_PENDING_MAX_PRED];
    int pn;
    bool is_inout;
} TmCollectCtx;

/* ---- layout helpers ------------------------------------------------------ */

static inline uint64_t tm_align_up(uint64_t v, uint64_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

static inline uint64_t tm_layout(const TmConfig *cfg, TmHeader *out) {
    uint64_t cur = sizeof(TmHeader);
    const uint64_t off_buckets = cur;
    cur += (uint64_t)cfg->num_buckets * sizeof(int32_t);
    cur = tm_align_up(cur, TM_ENTRY_ALIGN);
    const uint64_t off_pool = cur;
    cur += (uint64_t)cfg->pool_size * sizeof(TmEntry);
    const uint64_t off_free = cur;
    cur += (uint64_t)cfg->pool_size * sizeof(int32_t);
    uint64_t off_task[TM_MAX_RINGS] = {0};
    for (uint32_t r = 0; r < cfg->num_rings; r++) {
        off_task[r] = cur;
        cur += (uint64_t)cfg->task_window[r] * sizeof(int32_t);
    }
    if (out != NULL) {
        out->off_buckets = (uint32_t)off_buckets;
        out->off_pool = (uint32_t)off_pool;
        out->off_free = (uint32_t)off_free;
        for (uint32_t r = 0; r < cfg->num_rings; r++) {
            out->off_task_heads[r] = (uint32_t)off_task[r];
        }
    }
    return cur;
}

static inline uint64_t tm_make_id(uint32_t ring, uint32_t local) {
    return ((uint64_t)ring << 32) | local;
}
static inline uint32_t tm_ring_of(uint64_t id) {
    return (uint32_t)(id >> 32);
}
static inline uint32_t tm_local_of(uint64_t id) {
    return (uint32_t)(id & 0xFFFFFFFFu);
}

static inline uint64_t tm_entry_extent_elem(const TmEntry *e) {
    if (e->is_contiguous) {
        uint64_t n = 1;
        for (uint32_t i = 0; i < e->ndims; i++) {
            n *= e->shapes[i];
        }
        return n;
    }
    return e->extent_elem_cache;
}

static inline void tm_copy_region_to_entry_onlytest(const TmRegion_onlytest *r, TmEntry *e) {
    e->base_addr = r->base_addr;
    e->version = r->version;
    e->ndims = r->ndims;
    e->elem_size = r->elem_size;
    e->manual_dep = 0;
    e->is_contiguous = 1;
    e->start_offset = r->start_offset;
    e->storage_numel = r->storage_numel;
    uint64_t extent = 1;
    for (uint32_t i = 0; i < r->ndims; i++) {
        e->shapes[i] = r->shapes[i];
        e->strides[i] = r->strides[i];
        extent *= r->shapes[i];
    }
    e->extent_elem_cache = extent;
}

/**
 * PTO2-style overlap: L1 element-range, L2 row-major per-dim, L3 OTHER.
 * Order matches simpler PTO2TensorMapEntry::check_overlap.
 */
static inline TmOverlap tm_check_overlap(const Tensor *in, const TmEntry *e) {
    /* Consumer tensor is newer than producer entry → conservative fanin (L3). */
    if (in->version > e->version) {
        return TM_OVERLAP_OTHER;
    }

    /*
     * -------- ndims == 2 fast path --------
     * Semantically identical to the general path below; unrolled for qwen3
     * (most views are 2-D row-major). Falls through to the else branch for
     * ndims != 2 or ndims == 0.
     */
    if (in->ndims == 2u && e->ndims == 2u) {
        /* L1: element extent (contiguous → prod(shapes); else cache). */
        uint64_t extent_elem;
        if (in->is_contiguous) {
            extent_elem = (uint64_t)in->shapes[0] * in->shapes[1];
        } else {
            extent_elem = in->extent_elem_cache;
        }

        uint64_t ent_extent;
        if (e->is_contiguous) {
            ent_extent = (uint64_t)e->shapes[0] * e->shapes[1];
        } else {
            ent_extent = e->extent_elem_cache;
        }

        /* L1: 1D element-range intersection (O(1) fast reject). */
        const uint64_t in_end = in->start_offset + extent_elem;
        const uint64_t ent_end = e->start_offset + ent_extent;
        if (!(in_end > e->start_offset && ent_end > in->start_offset)) {
            return TM_OVERLAP_NONE;
        }

        /* L2 prereqs: same dtype / shared row-major strides. */
        if ((uint16_t)in->dtype != e->elem_size) {
            return TM_OVERLAP_OTHER;
        }
        if (in->strides[0] != e->strides[0] || in->strides[1] != e->strides[1]) {
            return TM_OVERLAP_OTHER;
        }
        if (e->strides[1] != 1u) {
            return TM_OVERLAP_OTHER;
        }
        if (e->strides[0] % e->strides[1] != 0u) {
            return TM_OVERLAP_OTHER;
        }

        /* Derive reference buffer shape from entry strides + storage_numel. */
        const uint32_t ref_shape1 = e->strides[0] / e->strides[1];
        const uint32_t stride0 = e->strides[0];
        if (stride0 == 0u || e->storage_numel % stride0 != 0u) {
            return TM_OVERLAP_OTHER;
        }
        const uint32_t ref_shape0 = (uint32_t)(e->storage_numel / stride0);

        /*
         * Decompose start_offset into 2-D offsets under shared strides:
         *   off0 = remain / s0;  off1 = (remain % s0) / s1
         */
        const uint32_t s0 = e->strides[0];
        const uint32_t s1 = e->strides[1];
        uint64_t in_remain = in->start_offset;
        uint64_t ent_remain = e->start_offset;
        const uint32_t in_off0 = (uint32_t)(in_remain / s0);
        in_remain %= s0;
        const uint32_t in_off1 = (uint32_t)(in_remain / s1);
        in_remain %= s1;
        const uint32_t ent_off0 = (uint32_t)(ent_remain / s0);
        ent_remain %= s0;
        const uint32_t ent_off1 = (uint32_t)(ent_remain / s1);
        ent_remain %= s1;
        /* Offset must land on stride grid. */
        if (in_remain != 0u || ent_remain != 0u) {
            return TM_OVERLAP_OTHER;
        }

        /* Defense-in-depth: each view fits inside ref_shapes (storage bounds). */
        if ((uint64_t)in_off0 + in->shapes[0] > ref_shape0 ||
            (uint64_t)ent_off0 + e->shapes[0] > ref_shape0) {
            return TM_OVERLAP_OTHER;
        }
        if ((uint64_t)in_off1 + in->shapes[1] > ref_shape1 ||
            (uint64_t)ent_off1 + e->shapes[1] > ref_shape1) {
            return TM_OVERLAP_OTHER;
        }

        /* L2 core: per-dim line-segment intersection (axis 0, then axis 1). */
        const uint64_t in_a1_0 = (uint64_t)in_off0 + in->shapes[0];
        const uint64_t ent_b1_0 = (uint64_t)ent_off0 + e->shapes[0];
        if (!(in_a1_0 > ent_off0 && ent_b1_0 > in_off0)) {
            return TM_OVERLAP_NONE;
        }
        bool input_contains_entry = (in_off0 <= ent_off0 && ent_b1_0 <= in_a1_0);

        const uint64_t in_a1_1 = (uint64_t)in_off1 + in->shapes[1];
        const uint64_t ent_b1_1 = (uint64_t)ent_off1 + e->shapes[1];
        if (!(in_a1_1 > ent_off1 && ent_b1_1 > in_off1)) {
            return TM_OVERLAP_NONE;
        }
        if (!(in_off1 <= ent_off1 && ent_b1_1 <= in_a1_1)) {
            input_contains_entry = false;
        }

        return input_contains_entry ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
    } else {
        uint64_t extent_elem;
        if (in->is_contiguous) {
            extent_elem = 1;
            for (uint32_t i = 0; i < in->ndims; i++) {
                extent_elem *= in->shapes[i];
            }
        } else {
            extent_elem = in->extent_elem_cache;
        }

        /* -------- L1: 1D element-range intersection (O(1) fast reject) -------- */
        const uint64_t in_begin = in->start_offset;
        const uint64_t in_end = in->start_offset + extent_elem;
        const uint64_t ent_begin = e->start_offset;
        const uint64_t ent_end = e->start_offset + tm_entry_extent_elem(e);
        /* Disjoint [in_begin,in_end) vs [ent_begin,ent_end) → no dependency. */
        if (!(in_end > ent_begin && ent_end > in_begin)) {
            return TM_OVERLAP_NONE;
        }

        /* -------- L2 prereqs: same dtype / ndims / per-dim strides -------- */
        /* dtype or rank mismatch, or empty view → cannot do hyper-rect check. */
        if ((uint16_t)in->dtype != e->elem_size || in->ndims != e->ndims ||
            in->ndims == 0) {
            return TM_OVERLAP_OTHER;
        }
        for (uint32_t i = 0; i < in->ndims; i++) {
            /* Transpose / permute / different slice step → layout not comparable. */
            if (in->strides[i] != e->strides[i]) {
                return TM_OVERLAP_OTHER;
            }
        }
        /* Innermost stride must be 1 (element axis) for row-major ref shape. */
        if (e->strides[in->ndims - 1u] != 1u) {
            return TM_OVERLAP_OTHER;
        }
        for (uint32_t i = 1; i < in->ndims; i++) {
            /* strides[i-1] must be an integer multiple of strides[i] (row-major). */
            if (e->strides[i - 1u] % e->strides[i] != 0u) {
                return TM_OVERLAP_OTHER;
            }
        }

        /* Derive reference buffer shape from entry strides + storage_numel. */
        uint32_t ref_shapes[TM_MAX_DIMS] = {0};
        for (uint32_t i = 1; i < in->ndims; i++) {
            ref_shapes[i] = e->strides[i - 1u] / e->strides[i];
        }
        const uint32_t stride0 = e->strides[0];
        /* stride0==0 or storage not divisible → invalid row-major layout. */
        if (stride0 == 0u || e->storage_numel % stride0 != 0u) {
            return TM_OVERLAP_OTHER;
        }
        ref_shapes[0] = (uint32_t)(e->storage_numel / stride0);

        /* Decompose start_offset into per-dim offsets under shared strides. */
        uint32_t in_offsets[TM_MAX_DIMS] = {0};
        uint32_t ent_offsets[TM_MAX_DIMS] = {0};
        uint64_t in_remain = in->start_offset;
        uint64_t ent_remain = e->start_offset;
        for (uint32_t i = 0; i < in->ndims; i++) {
            const uint32_t s = e->strides[i];
            in_offsets[i] = (uint32_t)(in_remain / s);
            ent_offsets[i] = (uint32_t)(ent_remain / s);
            in_remain %= s;
            ent_remain %= s;
        }
        /* Offset not aligned to stride grid → not a valid row-major view origin. */
        if (in_remain != 0u || ent_remain != 0u) {
            return TM_OVERLAP_OTHER;
        }

        /* Defense-in-depth: each side must fit inside ref_shapes (storage bounds). */
        for (uint32_t i = 0; i < in->ndims; i++) {
            /* Consumer view or producer view extends past reference storage on axis i. */
            if ((uint64_t)in_offsets[i] + in->shapes[i] > ref_shapes[i] || (uint64_t)ent_offsets[i] + e->shapes[i] > ref_shapes[i]) {
                return TM_OVERLAP_OTHER;
            }
        }

        /* -------- L2 core: per-dim hyper-rectangle intersection -------- */
        bool input_contains_entry = true;
        for (uint32_t i = 0; i < in->ndims; i++) {
            const uint64_t a0 = in_offsets[i];
            const uint64_t a1 = a0 + in->shapes[i];
            const uint64_t b0 = ent_offsets[i];
            const uint64_t b1 = b0 + e->shapes[i];
            /* No overlap on axis i → overall NO_OVERLAP (L1 had a 1D hit). */
            if (!(a1 > b0 && b1 > a0)) {
                return TM_OVERLAP_NONE;
            }
            /* Overlap but consumer does not fully contain producer on axis i. */
            if (!(a0 <= b0 && b1 <= a1)) {
                input_contains_entry = false;
            }
        }
        /* All dims intersect and consumer contains producer → ownership transfer. */
        return input_contains_entry ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
    }
}

/* ---- in-buffer accessors ------------------------------------------------- */

static inline TmHeader *tm_hdr(const TmTensorMap *self) {
    return (TmHeader *)self->base;
}
static inline int32_t *tm_buckets(const TmTensorMap *self) {
    return (int32_t *)(self->base + tm_hdr(self)->off_buckets);
}
static inline TmEntry *tm_pool(const TmTensorMap *self) {
    return (TmEntry *)(self->base + tm_hdr(self)->off_pool);
}
static inline int32_t *tm_free_list(const TmTensorMap *self) {
    return (int32_t *)(self->base + tm_hdr(self)->off_free);
}
static inline int32_t *tm_task_heads(const TmTensorMap *self, uint32_t ring) {
    return (int32_t *)(self->base + tm_hdr(self)->off_task_heads[ring]);
}

static inline uint32_t tm_hash(const TmTensorMap *self, uint64_t key) {
    key *= 0x9E3779B97F4A7C15ULL;
    return (uint32_t)(key >> (64 - __builtin_ctz(tm_hdr(self)->cfg.num_buckets)));
}

static inline bool tm_entry_valid(const TmTensorMap *self, const TmEntry *e) {
    return (int32_t)tm_local_of(e->producer_id) >=
           tm_hdr(self)->last_alive[tm_ring_of(e->producer_id)];
}

static inline void tm_remove_from_task(TmTensorMap *self, int32_t idx) {
    TmEntry *pl = tm_pool(self);
    TmEntry *e = &pl[idx];
    if (e->prev_in_task == -1) {
        const uint32_t ring = tm_ring_of(e->producer_id);
        const uint32_t slot =
            tm_local_of(e->producer_id) & (tm_hdr(self)->cfg.task_window[ring] - 1u);
        tm_task_heads(self, ring)[slot] = e->next_in_task;
    } else {
        pl[e->prev_in_task].next_in_task = e->next_in_task;
    }
    if (e->next_in_task != -1) {
        pl[e->next_in_task].prev_in_task = e->prev_in_task;
    }
    e->next_in_task = e->prev_in_task = -1;
}

static inline void tm_free_entry(TmTensorMap *self, int32_t idx) {
    TmEntry *pl = tm_pool(self);
    int32_t *bk = tm_buckets(self);
    TmEntry *e = &pl[idx];
    if (e->prev_in_bucket == -1) {
        bk[e->bucket_index] = e->next_in_bucket;
    } else {
        pl[e->prev_in_bucket].next_in_bucket = e->next_in_bucket;
    }
    if (e->next_in_bucket != -1) {
        pl[e->next_in_bucket].prev_in_bucket = e->prev_in_bucket;
    }

    tm_free_list(self)[tm_hdr(self)->free_num++] = idx;
    e->bucket_index = -1;
    e->next_in_bucket = e->prev_in_bucket = -1;
    e->next_in_task = e->prev_in_task = -1;
}

/* ---- public API ---------------------------------------------------------- */

static inline uint64_t tm_bytes_required(const TmConfig *cfg) {
    return tm_layout(cfg, NULL);
}

static inline void tm_init(TmTensorMap *self, void *base, const TmConfig *cfg) {
    self->base = (uint8_t *)base;
    TmHeader *h = tm_hdr(self);
    h->cfg = *cfg;
    h->next_entry_idx = 0;
    h->free_num = 0;
    for (uint32_t r = 0; r < TM_MAX_RINGS; r++) {
        h->last_alive[r] = 0;
        h->last_cleanup[r] = 0;
        h->off_task_heads[r] = 0;
    }
    tm_layout(cfg, h);

    int32_t *bk = tm_buckets(self);
    for (uint32_t i = 0; i < cfg->num_buckets; i++) {
        bk[i] = -1;
    }
    TmEntry *pl = tm_pool(self);
    for (uint32_t i = 0; i < cfg->pool_size; i++) {
        memset(&pl[i], 0, sizeof(TmEntry));
        pl[i].bucket_index = -1;
        pl[i].next_in_bucket = pl[i].prev_in_bucket = -1;
        pl[i].next_in_task = pl[i].prev_in_task = -1;
    }
    for (uint32_t r = 0; r < cfg->num_rings; r++) {
        int32_t *th = tm_task_heads(self, r);
        for (uint32_t i = 0; i < cfg->task_window[r]; i++) {
            th[i] = -1;
        }
    }
}

static inline void tm_attach(TmTensorMap *self, void *base) {
    self->base = (uint8_t *)base;
}

static inline void tm_insert_onlytest(TmTensorMap *self, const TmRegion_onlytest *r,
    uint64_t producer_id) {
    TmHeader *h = tm_hdr(self);
    int32_t idx;
    if (h->free_num > 0) {
        idx = tm_free_list(self)[--h->free_num];
    } else if (h->next_entry_idx >= (int32_t)h->cfg.pool_size) {
        return;
    } else {
        idx = h->next_entry_idx++;
    }

    TmEntry *pl = tm_pool(self);
    TmEntry *e = &pl[idx];
    tm_copy_region_to_entry_onlytest(r, e);

    e->producer_id = producer_id;

    int32_t *bk = tm_buckets(self);
    const uint32_t b = tm_hash(self, r->base_addr);
    e->bucket_index = (int32_t)b;
    e->prev_in_bucket = -1;
    e->next_in_bucket = bk[b];
    if (bk[b] != -1) {
        pl[bk[b]].prev_in_bucket = idx;
    }
    bk[b] = idx;

    const uint32_t ring = tm_ring_of(producer_id);
    const uint32_t slot =
        tm_local_of(producer_id) & (h->cfg.task_window[ring] - 1u);
    int32_t *th = tm_task_heads(self, ring);
    e->prev_in_task = -1;
    e->next_in_task = th[slot];
    if (th[slot] != -1) {
        pl[th[slot]].prev_in_task = idx;
    }
    th[slot] = idx;
}

static inline void tm_remove(TmTensorMap *self, TmEntry *e) {
    const int32_t idx = (int32_t)(e - tm_pool(self));
    tm_remove_from_task(self, idx);
    tm_free_entry(self, idx);
}

static inline void tm_sync(TmTensorMap *self, uint32_t ring, int32_t last_alive) {
    tm_hdr(self)->last_alive[ring] = last_alive;
}

static inline void tm_cleanup_retired(TmTensorMap *self, uint32_t ring,
    int32_t old_alive, int32_t new_alive) {
    const uint32_t mask = tm_hdr(self)->cfg.task_window[ring] - 1u;
    int32_t *th = tm_task_heads(self, ring);
    TmEntry *pl = tm_pool(self);
    for (int32_t local = old_alive; local < new_alive; local++) {
        const uint32_t slot = (uint32_t)local & mask;
        int32_t cur = th[slot];
        while (cur != -1) {
            const int32_t next = pl[cur].next_in_task;
            tm_free_entry(self, cur);
            cur = next;
        }
        th[slot] = -1;
    }
    tm_hdr(self)->last_cleanup[ring] = new_alive;
}

static inline uint32_t tm_task_slot(uint32_t ring, uint32_t local,
    const TmConfig *cfg) {
    return local & (cfg->task_window[ring] - 1u);
}

/* PTO2-aligned: periodic cleanup + slot-wrap trigger. */
static inline void tm_sync_tensormap(TmTensorMap *self, uint32_t ring,
    int32_t last_alive, uint32_t current_local) {
    tm_sync(self, ring, last_alive);
    const TmHeader *h = tm_hdr(self);
    const int32_t old = h->last_cleanup[ring];
    const bool slot_overlap =
        tm_task_slot(ring, current_local, &h->cfg) ==
        tm_task_slot(ring, (uint32_t)old, &h->cfg);
    if (last_alive - old >= (int32_t)TM_CLEANUP_INTERVAL || slot_overlap) {
        tm_cleanup_retired(self, ring, old, last_alive);
    }
}

static inline int32_t tm_valid_count(const TmTensorMap *self) {
    const TmHeader *h = tm_hdr(self);
    const TmEntry *pl = tm_pool(self);
    int32_t n = 0;
    for (int32_t i = 0; i < h->next_entry_idx; i++) {
        if (pl[i].bucket_index != -1 && tm_entry_valid(self, &pl[i])) {
            n++;
        }
    }
    return n;
}

static inline bool tm_tensor_strides_row_major(const Tensor *t) {
    uint32_t expected = 1u;
    for (int32_t i = (int32_t)t->ndims - 1; i >= 0; i--) {
        if (t->strides[i] != expected) {
            return false;
        }
        expected *= t->shapes[i];
    }
    return true;
}

/**
 * Lookup producers overlapping consumer `tensor`; collect preds and optionally
 * remove COVERED entries when ctx->is_inout (tm_submit path).
 */
static inline void tm_lookup(TmTensorMap *self, const Tensor *tensor,
    TmCollectCtx *ctx) {
    const uint32_t bucket_index = tm_hash(self, tensor->buffer_addr);
    int32_t cur = tm_buckets(self)[bucket_index];
    TmEntry *entry_pool = tm_pool(self);

    while (cur != -1) {
        const int32_t next = entry_pool[cur].next_in_bucket;
        TmEntry *entry = &entry_pool[cur];
        if (tm_entry_valid(self, entry) &&
            entry->base_addr == tensor->buffer_addr) {
            const TmOverlap status = tm_check_overlap(tensor, entry);
            if (status != TM_OVERLAP_NONE) {
                const uint16_t producer =
                    (uint16_t)tm_local_of(entry->producer_id);
                if (producer != ctx->consumer) {
                    bool duplicate = false;
                    for (int i = 0; i < ctx->pn; i++) {
                        if (ctx->preds[i] == producer) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate && ctx->pn < (int)TM_PENDING_MAX_PRED) {
                        ctx->preds[ctx->pn++] = producer;
                    }
                }
                if (ctx->is_inout && status == TM_OVERLAP_COVERED) {
                    tm_remove(self, entry);
                }
            }
        }
        cur = next;
    }
}

typedef bool (*TmMatchFnOnlytest)(TmEntry *entry, TmOverlap status, void *ctx);

/** Test/bench only: generic bucket walk with pluggable per-match handler. */
static inline void tm_lookup_onlytest(TmTensorMap *self, const Tensor *tensor,
    TmMatchFnOnlytest on_match, void *ctx) {
    const uint32_t bucket_index = tm_hash(self, tensor->buffer_addr);
    int32_t cur = tm_buckets(self)[bucket_index];
    TmEntry *entry_pool = tm_pool(self);

    while (cur != -1) {
        const int32_t next = entry_pool[cur].next_in_bucket;
        TmEntry *entry = &entry_pool[cur];
        if (tm_entry_valid(self, entry) &&
            entry->base_addr == tensor->buffer_addr) {
            const TmOverlap status = tm_check_overlap(tensor, entry);
            if (status != TM_OVERLAP_NONE) {
                if (!on_match(entry, status, ctx)) {
                    return;
                }
            }
        }
        cur = next;
    }
}

/**
 * Register a producer Tensor in the tensormap (called on task output).
 * Allocates a pool slot, copies overlap-relevant metadata, then inserts at
 * the head of the hash-bucket chain and the per-task chain.
 */
static inline void tm_insert(TmTensorMap *self, const Tensor *tensor,
    uint16_t task_id) {
    TmHeader *header = tm_hdr(self);

    /* ---- allocate entry from free list or bump pointer ---- */
    int32_t entry_index;
    if (header->free_num > 0) {
        entry_index = tm_free_list(self)[--header->free_num];
    } else if (header->next_entry_idx >= (int32_t)header->cfg.pool_size) {
        return; /* pool exhausted */
    } else {
        entry_index = header->next_entry_idx++;
    }

    TmEntry *entry_pool = tm_pool(self);
    TmEntry *entry = &entry_pool[entry_index];

    /* ---- copy overlap fields from Tensor into TmEntry ---- */
    entry->base_addr = tensor->buffer_addr;
    /* Bytes [24,40): start_offset, version, ndims — PTO2 layout match. */
    memcpy((uint8_t *)entry + 24, (const uint8_t *)tensor + 24, 16);
    entry->elem_size = tensor->dtype;
    entry->manual_dep = tensor->manual_dep;
    entry->is_contiguous = tensor->is_contiguous;
    memcpy(entry->shapes, tensor->shapes, sizeof entry->shapes);
    entry->storage_numel = tensor->buffer_size / tensor->dtype;
    entry->extent_elem_cache = tensor->extent_elem_cache;
    memcpy(entry->strides, tensor->strides, sizeof entry->strides);

    const uint64_t producer_id = tm_make_id(0, task_id);
    entry->producer_id = producer_id;

    /* ---- link into hash bucket (head insert, keyed by buffer_addr) ---- */
    int32_t *bucket_heads = tm_buckets(self);
    const uint32_t bucket_index = tm_hash(self, tensor->buffer_addr);
    entry->bucket_index = bucket_index;
    entry->prev_in_bucket = -1;
    entry->next_in_bucket = bucket_heads[bucket_index];
    if (bucket_heads[bucket_index] != -1) {
        entry_pool[bucket_heads[bucket_index]].prev_in_bucket = entry_index;
    }
    bucket_heads[bucket_index] = entry_index;

    /* ---- link into per-ring per-task list (head insert) ---- */
    const uint32_t ring_id = tm_ring_of(producer_id);
    const uint32_t task_slot =
        tm_local_of(producer_id) & (header->cfg.task_window[ring_id] - 1u);
    int32_t *task_entry_heads = tm_task_heads(self, ring_id);
    entry->prev_in_task = -1;
    entry->next_in_task = task_entry_heads[task_slot];
    if (task_entry_heads[task_slot] != -1) {
        entry_pool[task_entry_heads[task_slot]].prev_in_task = entry_index;
    }
    task_entry_heads[task_slot] = entry_index;
}

/* ===========================================================================
 * High-level tensormap dependency layer
 * ========================================================================== */
#ifdef DAG_RING_BUF_H

#ifndef TMD_POOL_SIZE
#define TMD_POOL_SIZE 8192u
#endif
#ifndef TMD_NUM_BUCKETS
#define TMD_NUM_BUCKETS 2048u
#endif
#ifndef TMD_TASK_WINDOW
#define TMD_TASK_WINDOW 65536u
#endif

#define TMD_BUF_BYTES                                                 \
    (sizeof(TmHeader) + (uint64_t)TMD_NUM_BUCKETS * sizeof(int32_t) + \
        TM_ENTRY_ALIGN + (uint64_t)TMD_POOL_SIZE * sizeof(TmEntry) +  \
        (uint64_t)TMD_POOL_SIZE * sizeof(int32_t) +                   \
        (uint64_t)TMD_TASK_WINDOW * sizeof(int32_t))

static TmTensorMap g_tm_map;
static _Alignas(TM_ENTRY_ALIGN) uint8_t g_tm_buf[TMD_BUF_BYTES];

#ifndef TM_PENDING_MAX_IO
#define TM_PENDING_MAX_IO 16u
#endif
#ifndef TM_PENDING_MAX_PRED
#define TM_PENDING_MAX_PRED 64u
#endif

enum {
    TM_PEND_IN = 1u,
    TM_PEND_OUT = 2u,
    TM_PEND_INOUT = (TM_PEND_IN | TM_PEND_OUT)
};

typedef struct {
    const Tensor *t;
    uint8_t kind;
} TmPendingSlot;

static TmPendingSlot g_tm_pend[TM_PENDING_MAX_IO];
static int g_tm_pend_n;

static inline void tm_pending_clear(void) {
    g_tm_pend_n = 0;
}

static inline void tm_pending_push(const Tensor *t, uint8_t kind) {
    if (g_tm_pend_n < (int)TM_PENDING_MAX_IO) {
        g_tm_pend[g_tm_pend_n].t = t;
        g_tm_pend[g_tm_pend_n].kind = kind;
        g_tm_pend_n++;
    }
}

static inline void tm_deps_init(void) {
    TmConfig cfg;
    cfg.num_buckets = TMD_NUM_BUCKETS;
    cfg.pool_size = TMD_POOL_SIZE;
    cfg.num_rings = 1;
    cfg.task_window[0] = TMD_TASK_WINDOW;
    for (uint32_t r = 1; r < TM_MAX_RINGS; r++) {
        cfg.task_window[r] = 1;
    }
    tm_init(&g_tm_map, g_tm_buf, &cfg);
    tm_pending_clear();
}

static inline void tm_in_ptr(uint16_t tid, const Tensor *t) {
    add_input_ptr(tid, t);
    tm_pending_push(t, TM_PEND_IN);
}

static inline void tm_in_ro_ptr(uint16_t tid, const Tensor *t) {
    add_input_ptr(tid, t);
}

static inline void tm_out_ro_ptr(uint16_t tid, const Tensor *t) {
    add_output_ptr(tid, t);
}

static inline void tm_inout_ro_ptr(uint16_t tid, const Tensor *t) {
    add_inout_ptr(tid, t);
}

static inline void tm_out_ptr(uint16_t tid, const Tensor *t) {
    add_output_ptr(tid, t);
    tm_pending_push(t, TM_PEND_OUT);
}

static inline void tm_inout_ptr(uint16_t tid, const Tensor *t) {
    add_inout_ptr(tid, t);
    tm_pending_push(t, TM_PEND_INOUT);
}

#define tm_in(tid, t) tm_in_ptr((tid), &(t))
#define tm_in_ro(tid, t) tm_in_ro_ptr((tid), &(t))
#define tm_out(tid, t) tm_out_ptr((tid), &(t))
#define tm_out_ro(tid, t) tm_out_ro_ptr((tid), &(t))
#define tm_inout(tid, t) tm_inout_ptr((tid), &(t))
#define tm_inout_ro(tid, t) tm_inout_ro_ptr((tid), &(t))

static inline void tm_submit(uint16_t tid) {
    TmCollectCtx ctx = {.consumer = tid, .pn = 0};
    int i;

    for (i = 0; i < g_tm_pend_n; i++) {
        if (g_tm_pend[i].kind & TM_PEND_IN) {
            ctx.is_inout = (g_tm_pend[i].kind == TM_PEND_INOUT);
            tm_lookup(&g_tm_map, g_tm_pend[i].t, &ctx);
        }
    }
    for (i = 0; i < ctx.pn; i++) {
        succeed(tid, ctx.preds[i]);
    }
    for (i = 0; i < g_tm_pend_n; i++) {
        if (g_tm_pend[i].kind & TM_PEND_OUT) {
            tm_insert(&g_tm_map, g_tm_pend[i].t, tid);
        }
    }
    tm_pending_clear();
    submit(tid);
    tm_sync_tensormap(&g_tm_map, 0, (int32_t)g_min_uncomplete_task, tid);
}

#endif /* DAG_RING_BUF_H */

#ifdef __cplusplus
}
#endif

#endif /* USE_TENSORMAP */

#endif /* ESL_PROXY_TENSORMAP_H */
