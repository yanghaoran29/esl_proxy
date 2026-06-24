/*
 * onboard_tensor.h - PTO2-aligned Tensor layout for AICore fake kernel (128B).
 * Field order matches esl_proxy/include/tensor.h / simpler runtime/tensor.h.
 */
#ifndef ESL_PROXY_ONBOARD_TENSOR_H
#define ESL_PROXY_ONBOARD_TENSOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ESL_ONBOARD_TENSOR_MAX_DIMS = 5 };

typedef struct EslOnboardTensor {
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
    uint32_t shapes[ESL_ONBOARD_TENSOR_MAX_DIMS];

    uint64_t extent_elem_cache;
    uint32_t strides[ESL_ONBOARD_TENSOR_MAX_DIMS];
    uint8_t _pad_cl2[36];
} __attribute__((aligned(64))) EslOnboardTensor;

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ONBOARD_TENSOR_H */
