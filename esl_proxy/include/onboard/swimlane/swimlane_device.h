/*
 * Device-side swimlane API: AICPU collection + AICore profiling state/record.
 */
#ifndef ESL_PROXY_SWIMLANE_DEVICE_H
#define ESL_PROXY_SWIMLANE_DEVICE_H

#include "swimlane/swimlane_types.h"
#include "tools.h"
#include <stdbool.h>

static inline uint64_t get_sys_cnt_aicpu(void)
{
    return esl_onboard_sys_cnt();
}

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base);
uint64_t get_platform_l2_swimlane_base(void);
void set_l2_swimlane_enabled(bool enable);
bool is_l2_swimlane_enabled(void);
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr);
uint64_t get_platform_l2_swimlane_aicore_rotation_table(void);
void l2_swimlane_aicpu_init(int worker_count);
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx);
void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
L2SwimlaneLevel get_l2_swimlane_level(void);
#endif

#if defined(__DAV_VEC__) || defined(__DAV_CUBE__)

#include "aicore.h"

#ifndef get_sys_cnt_aicore
#define get_sys_cnt_aicore() get_sys_cnt()
#endif

[[block_local]] static uint32_t s_aicore_profiling_flag;
[[block_local]] static __gm__ uint64_t *s_l2_swimlane_aicore_head_slot;
[[block_local]] static __gm__ L2SwimlaneActiveHead *s_l2_swimlane_aicore_head;

__attribute__((weak)) __aicore__ inline void set_aicore_profiling_flag(uint32_t flag)
{
    s_aicore_profiling_flag = flag;
}

__attribute__((weak)) __aicore__ inline uint32_t get_aicore_profiling_flag(void)
{
    return s_aicore_profiling_flag;
}

__attribute__((weak)) __aicore__ inline void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr)
{
    s_l2_swimlane_aicore_head_slot = slot_ptr;
    s_l2_swimlane_aicore_head = nullptr;
}

__attribute__((weak)) __aicore__ inline __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void)
{
    if (s_l2_swimlane_aicore_head_slot == nullptr) {
        return nullptr;
    }
    if (s_l2_swimlane_aicore_head == nullptr) {
        dcci(s_l2_swimlane_aicore_head_slot, SINGLE_CACHE_LINE);
        const uint64_t head_addr = *s_l2_swimlane_aicore_head_slot;
        if (head_addr != 0U) {
            s_l2_swimlane_aicore_head = reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(head_addr);
        }
    }
    return s_l2_swimlane_aicore_head;
}

struct L2SwimlaneAicoreLocalState {
    __gm__ L2SwimlaneAicoreTaskBuffer *cached_buf = nullptr;
    uint32_t cached_buf_seq = UINT32_MAX;
    uint32_t slot_within_buf = 0;
};

__aicore__ __attribute__((always_inline)) static inline void l2_swimlane_aicore_record_task(
    __gm__ L2SwimlaneActiveHead *head, L2SwimlaneAicoreLocalState *local, uint64_t task_token_raw, uint32_t reg_task_id,
    uint64_t start_time, uint64_t end_time
) {
    dcci(head, SINGLE_CACHE_LINE);
    if (head->current_buf_seq != local->cached_buf_seq) {
        local->cached_buf_seq = head->current_buf_seq;
        local->cached_buf = reinterpret_cast<__gm__ L2SwimlaneAicoreTaskBuffer *>(head->current_buf_ptr);
        local->slot_within_buf = 0;
    }
    if (local->cached_buf == nullptr) {
        return;
    }

    uint32_t slot = local->slot_within_buf;
    if (slot >= PLATFORM_AICORE_BUFFER_SIZE) {
        return;
    }

    __gm__ L2SwimlaneAicoreTaskRecord *record = &local->cached_buf->records[slot];
    record->start_time = start_time;
    record->end_time = end_time;
    record->task_token_raw = task_token_raw;
    record->reg_task_id = reg_task_id;
    local->slot_within_buf = slot + 1;

    dcci(record, SINGLE_CACHE_LINE, CACHELINE_OUT);
    dsb((mem_dsb_t)0);
}

#endif /* __DAV_VEC__ || __DAV_CUBE__ */

#endif /* ESL_PROXY_SWIMLANE_DEVICE_H */
