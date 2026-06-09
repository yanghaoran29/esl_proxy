/*
 * tensor.h - PTO2-aligned Tensor layout (2 x 64B cache lines, 128B total).
 *
 * Field order and offsets match simpler runtime/tensor.h so cache line 1 can be
 * memcpy'd into TmEntry overlap fields (bytes [24,64) from start_offset onward).
 */

#ifndef ESL_PROXY_TENSOR_H
#define ESL_PROXY_TENSOR_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


typedef enum {
    BFLOAT16 = 2,
    FLOAT32  = 4,
} dtype_t;

enum { ESL_PROXY_TENSOR_MAX_DIMS = 5 };

struct Tensor {
    /* === Cache line 1 (64B) — hot path === */
    uint64_t buffer_addr;
    uint64_t buffer_size;
    uint64_t owner_task_id;
    uint64_t start_offset;
    int32_t version;
    uint32_t ndims;
    uint8_t dtype;
    uint8_t manual_dep;
    uint8_t is_contiguous;
    uint8_t _pad_cl1;
    uint32_t shapes[ESL_PROXY_TENSOR_MAX_DIMS];

    /* === Cache line 2 (64B) — warm path === */
    uint64_t extent_elem_cache;
    uint32_t strides[ESL_PROXY_TENSOR_MAX_DIMS];
    uint8_t _pad_cl2[36];
} __attribute__((aligned(64)));

typedef struct Tensor Tensor;

_Static_assert(sizeof(Tensor) == 128, "Tensor must be exactly 2 cache lines (128 bytes)");
_Static_assert(_Alignof(Tensor) == 64, "Tensor must be 64-byte aligned");
_Static_assert(offsetof(Tensor, owner_task_id) == 16,
               "owner_task_id must be at bytes 16-23 (cacheline 1)");
_Static_assert(offsetof(Tensor, start_offset) == 24,
               "start_offset must be at bytes 24-31 (cacheline 1)");
_Static_assert(offsetof(Tensor, version) == 32, "version offset");
_Static_assert(offsetof(Tensor, ndims) == 36, "ndims offset");
_Static_assert(offsetof(Tensor, dtype) == 40, "dtype offset");
_Static_assert(offsetof(Tensor, manual_dep) == 41, "manual_dep offset");
_Static_assert(offsetof(Tensor, is_contiguous) == 42, "is_contiguous offset");
_Static_assert(offsetof(Tensor, shapes) == 44, "shapes must start at byte 44 (cacheline 1)");
_Static_assert(offsetof(Tensor, extent_elem_cache) == 64,
               "extent_elem_cache must start at byte 64 (cacheline 2)");
_Static_assert(offsetof(Tensor, strides) == 72, "strides offset");

static inline uint64_t tensor_base(Tensor t)
{
    return t.buffer_addr;
}

static inline uint64_t tensor_numel(const Tensor *t)
{
    uint64_t n = 1;
    for (uint32_t i = 0; i < t->ndims; i++) {
        n *= t->shapes[i];
    }
    return n;
}

/* Element extent hull: 1 + sum((shapes[i]-1)*strides[i]). Matches simpler refresh_derived. */
static inline uint64_t tensor_extent_elem_hull(const Tensor *t)
{
    uint64_t e = 1;
    for (int32_t i = (int32_t)t->ndims - 1; i >= 0; i--) {
        if (t->shapes[i] > 0u) {
            e += (uint64_t)(t->shapes[i] - 1u) * (uint64_t)t->strides[i];
        }
    }
    return e;
}

static inline void tensor_refresh_derived(Tensor *t)
{
    uint64_t expected = 1;
    uint8_t contig = 1;
    for (int32_t i = (int32_t)t->ndims - 1; i >= 0; i--) {
        if (t->strides[i] != expected) {
            contig = 0;
        }
        expected *= t->shapes[i];
    }
    t->is_contiguous = contig;
    t->extent_elem_cache = tensor_extent_elem_hull(t);
}

static inline void tensor_fill_row_major_strides(uint32_t ndims,
                                                 const uint32_t shapes[],
                                                 uint32_t strides_out[])
{
    uint32_t s = 1;
    for (int32_t i = (int32_t)ndims - 1; i >= 0; i--) {
        strides_out[i] = s;
        s *= shapes[i];
    }
}

static inline Tensor tensor_from_base(uint64_t base)
{
    Tensor t;
    memset(&t, 0, sizeof t);
    t.buffer_addr = base;
    return t;
}

static inline Tensor tensor_make_contiguous(uint64_t base, uint64_t buffer_size,
                                            const uint32_t shapes[],
                                            uint32_t ndims, dtype_t dtype)
{
    Tensor t;
    memset(&t, 0, sizeof t);
    t.buffer_addr = base;
    t.buffer_size = buffer_size;
    t.start_offset = 0;
    t.version = 0;
    t.owner_task_id = 0;
    t.ndims = ndims;
    t.dtype = (uint8_t)dtype;
    t.manual_dep = 0;
    t.is_contiguous = 1;
    for (uint32_t i = 0; i < ndims; i++) {
        t.shapes[i] = shapes[i];
    }
    tensor_fill_row_major_strides(ndims, t.shapes, t.strides);
    tensor_refresh_derived(&t);
    return t;
}

static inline Tensor tensor_make_2d(uint64_t base, uint32_t d0, uint32_t d1, dtype_t dtype)
{
    const uint32_t shapes[2] = {d0, d1};
    const uint64_t buffer_size =
        (uint64_t)d0 * (uint64_t)d1 * (uint64_t)dtype;
    return tensor_make_contiguous(base, buffer_size, shapes, 2, dtype);
}


static inline Tensor tensor_view(Tensor t, uint32_t dim, uint32_t off, uint32_t n)
{
    if (dim < t.ndims) {
        t.start_offset += (uint64_t)off * (uint64_t)t.strides[dim];
        t.shapes[dim] = n;
        tensor_refresh_derived(&t);
    }
    return t;
}

/* 2D subview in one step (simpler view(shapes, offsets)): off/shape per dim 0,1. */
static inline Tensor tensor_view_2d(Tensor t, uint32_t off0, uint32_t off1, uint32_t n0,
                                  uint32_t n1)
{
    if (t.ndims >= 2) {
        t.start_offset += (uint64_t)off0 * (uint64_t)t.strides[0]
                        + (uint64_t)off1 * (uint64_t)t.strides[1];
        t.shapes[0] = n0;
        t.shapes[1] = n1;
        tensor_refresh_derived(&t);
    }
    return t;
}


#endif /* ESL_PROXY_TENSOR_H */
