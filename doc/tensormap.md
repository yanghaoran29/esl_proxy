# Tensor Map
1. 支持创建VIEW


## Tensor描述
```cpp
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
    uint8_t reserved_c1;
    uint32_t shapes[ESL_PROXY_TENSOR_MAX_DIMS];

    /* === Cache line 2 (64B) — warm path === */
    uint64_t extent_elem_cache;
    uint32_t strides[ESL_PROXY_TENSOR_MAX_DIMS];
    uint8_t reserved[36];
} __attribute__((aligned(64)));
```



## Overlap检测

## TensorMap
1. 以基地址作为KEY计算HASH
2. 