/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 */

#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "conf.h"
#include "task.h"
#include "queue.h"

typedef struct ctrl {
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];

    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    queue_t ready_queue[TASK_TYPE_CNT];
    queue_t completed_queue;
    uint16_t tid;
} ctrl_t;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

void dispatch_bind(void *bridge);
void dispatch_tick_begin(int tid);
void dispatch_poll(int tid);
int dispatch_submit(ctrl_t *ctrl, int type, int exe_type, uint16_t task_id, int core, int slot,
                    uint64_t mask, uint32_t raw_duration, uint32_t jitter_mask);
void dispatch_drain_completions(int tid, uint16_t *task_ids, int *complete_cnt, int max_cnt);
void dispatch_after_push_completed(int tid, int complete_cnt);
uint32_t dispatch_executor_duration(uint32_t raw_duration);
int dispatch_stall_limit(void);

extern int g_completed_subtask_cnt;

#endif /* DISPATCH_H */
