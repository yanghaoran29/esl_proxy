/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DISPATCH_H
#define DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "conf.h"
#include "task.h"
#include "queue.h"

#include "runtime.h"

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];

    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    uint16_t tid;
} ctrl_t;

extern EslRuntime *g_runtime;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* 把硬件 AICore 完成事件拉到 msg_bitmap。Onboard：读硬件完成寄存器并置位对应
 * msg_bitmap bit，供 dispatch_drain_completions 解码；sim：no-op（fake kernel
 * 直接置位）。 */
void dispatch_poll(int tid);

/* 核心动作一——向 AICore 下发一个任务。将完整的 slot 占用语义（duration/idx/
 * task_id_map/free_bitmap 清位）与 bridge 下发调用打包在一起；下发失败时调用方
 * 回退入队。成功返回 0，bridge 拒绝返回非 0。 */
int dispatch_submit(ctrl_t *ctrl, int type, int exe_type, uint16_t task_id, int core, int slot,
                    uint64_t mask);

/* 核心动作二——回收已完成任务。扫描 msg_bitmap 的置位 bit，通过 task_id_map1/2
 * 反查回 task_id，最多收集 max_cnt 个完成事件。 */
void dispatch_drain_completions(int tid, uint16_t *task_ids, int *complete_cnt, int max_cnt);

uint32_t dispatch_executor_duration(uint32_t raw_duration);

#endif /* DISPATCH_H */
