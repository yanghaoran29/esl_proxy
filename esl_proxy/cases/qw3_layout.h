// Fixed qwen3_decode static orchestration layout (user_batch=90, batch_padded=96).
//
// Shared by build_static_tier.h and the static case entrypoints
// (qwen3_decode_static.h, qwen3_decode_tensormap_static.h).

#ifndef QW3_LAYOUT_H
#define QW3_LAYOUT_H

enum {
    QW3_USER_BATCH    = 90,
    QW3_BATCH_PADDED  = 96,
    QW3_TILE_ROWS     = 16,
    QW3_NUM_TILES     = 6,
    QW3_G1_MACRO_BASE = 1,
    QW3_G1_TASK_BASE  = 1,
    QW3_G2_MACRO_BASE = 7,
    QW3_G3_MACRO_BASE = 97,
};

#endif /* QW3_LAYOUT_H */
