/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */

#include "dispatch.h"
#include "log.h"
#include "ring_buf.h"
#include "swimlane.h"

#include <stdint.h>

extern atomic_int g_task_cnt;
extern atomic_int g_completed_cnt;
ctrl_t g_ctrl_t[THREAD_CNT];

/* Global ready queues fed by orchestration (ready_enqueue) and drained here. */
queue_t g_ready_queue[TASK_TYPE_CNT];
atomic_flag g_ready_lock[TASK_TYPE_CNT] = {
    ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT
};

#define SLOT_MASK 0xFFFFFFFFFFFFFFFULL /* AIC_CNT(60) valid slot bits */

/*
 * KNOWN ISSUE (temporary): msg_bitmap / task_id_map1 / task_id_map2 are only
 * EXE_TYPE_CNT(2) wide (AIC=CUBE, AIV=VECTOR) — there is no MIX column — but
 * free_bitmap and send_task are driven by task_type_t which includes MIX(=2).
 * Indexing the 2-wide arrays with MIX overruns them. Until a real MIX executor
 * pool exists, route MIX onto the AIC(CUBE) pool so nothing is indexed out of
 * bounds. Tracked in docs/swimlane.md.
 */
static inline int exe_type_of(int type)
{
    return (type == TASK_TYPE_MIX) ? TASK_TYPE_CUBE : type;
}

static inline void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

static inline void dispatch_init(int tid)
{
    g_ctrl_t[tid].tid = (uint16_t)tid;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] = 0xFFFFFFFFFFFFFFFULL;
            g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
        }
    }
    set_mix(tid);
}

static inline void update_exe_state(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
    set_mix(tid);
}

static inline void get_completed(uint64_t free_bitmap, uint16_t task_id[], int *complete_cnt,
                                 const uint16_t task_id_map[])
{
    int cnt = __builtin_popcountll(free_bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        task_id[(*complete_cnt)++] = task_id_map[idx];
        cnt--;
        free_bitmap &= free_bitmap - 1;
    }
}

// TODO: add counter for spmd
static inline void set_completed(int tid)
{
    uint16_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map1[i]);
        get_completed(g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt,
                      g_ctrl_t[tid].task_id_map2[i]);
    }
    /* consumed the completion signals — clear so they are not counted twice */
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        g_ctrl_t[tid].msg_bitmap[i][0] = 0;
        g_ctrl_t[tid].msg_bitmap[i][1] = 0;
    }
    for (int i = 0; i < complete_cnt; i++) {
        int slot = task_id[i] & RING_MASK;
        task_state s = atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
        s.state = COMPLETED;
        atomic_store_explicit(&g_state_buf[slot], s, memory_order_release);
        SWIM_STAMP(SL_DISPATCH(tid), task_id[i], SWIM_FINISH);
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, task_id, (uint16_t)complete_cnt);
    atomic_fetch_add_explicit(&g_completed_cnt, complete_cnt, memory_order_acquire);
    if (complete_cnt > 0) {
        WORKER_LOGF("dispatch", "tid=%d completed=%d", tid, complete_cnt);
    }
}

// TODO: Work Stealing
static inline void send_task(ctrl_t *ctrl, int type)
{
    const int et = exe_type_of(type); /* MIX -> AIC(CUBE); see KNOWN ISSUE above */
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1];
    int free_cnt = __builtin_popcountll(free_bitmap);
    int cnt = free_cnt > (int)ctrl->ready_cnt[type] ? (int)ctrl->ready_cnt[type] : free_cnt;
    int sent = cnt;
    uint16_t task_id;
    uint16_t head = (uint16_t)ctrl->ready_queue[type].head;
    ctrl->ready_queue[type].head += (uint64_t)cnt;
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        task_id = ctrl->ready_queue[type].tasks[head++];
        SWIM_STAMP(SL_DISPATCH(ctrl->tid), task_id, SWIM_DISPATCH);
#ifdef ESL_SWIMLANE
        {   /* executor lane: synthesize a bar from the task's declared duration */
            const uint64_t s = SWIM_NOW();
            const uint16_t dur = g_basic_buf[task_id & RING_MASK].duration;
            SWIM_TASK_RECORD(SL_EXEC(ctrl->tid, et, idx), task_id,
                             (uint32_t)g_basic_buf[task_id & RING_MASK].type, s, s + dur);
        }
#endif
        if ((ctrl->free_bitmap[type][0] & ((uint64_t)0x1 << idx)) == 0) {
            ctrl->task_id_map1[et][idx] = task_id;
            ctrl->free_bitmap[type][0] &= ~((uint64_t)0x1 << idx);
            ctrl->msg_bitmap[et][0] &= ~((uint64_t)0x1 << idx);
        } else {
            ctrl->task_id_map2[et][idx] = task_id;
            ctrl->free_bitmap[type][1] &= ~((uint64_t)0x1 << idx);
            ctrl->msg_bitmap[et][1] &= ~((uint64_t)0x1 << idx);
        }
        cnt--;
        free_bitmap &= free_bitmap - 1;
    }
    ctrl->ready_queue[type].cnt -= (uint64_t)sent;
    if (sent > 0) {
        WORKER_LOGF("dispatch", "tid=%d type=%d sent=%d", ctrl->tid, type, sent);
    }
}

/*
 * STAND-IN executor (temporary): main has no executor threads/kernels yet, so
 * mark every currently-busy slot as just-completed. This closes the pipeline
 * (assign -> "execute" -> complete) so root tasks actually drain and the
 * dispatch/executor swimlanes carry real data. Replace with the real 003
 * executor when it lands. Tracked in docs/swimlane.md.
 */
static inline void simulate_executor(int tid)
{
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].msg_bitmap[i][j] = (~g_ctrl_t[tid].free_bitmap[i][j]) & SLOT_MASK;
        }
    }
}

/* Drain ready tasks of `type` from the global queue into this dispatcher's
 * per-thread ready_queue and refresh the available count. */
static inline void pull_ready(int tid, int type)
{
    uint16_t buf[AIC_CNT];
    uint16_t n = ready_drain((task_type_t)type, buf, AIC_CNT);
    if (n > 0) {
        batch_enqueue(&g_ctrl_t[tid].ready_queue[type], buf, n);
    }
    g_ctrl_t[tid].ready_cnt[type] = (uint16_t)g_ctrl_t[tid].ready_queue[type].cnt;
}

void dispatch(int tid)
{
    simulate_executor(tid);
    update_exe_state(tid);
    set_completed(tid);
    pull_ready(tid, TASK_TYPE_CUBE);
    pull_ready(tid, TASK_TYPE_VECTOR);
    pull_ready(tid, TASK_TYPE_MIX);
    send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
}

/*
 * Dispatch worker thread entry point
 * Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    dispatch_init(tid);
    WORKER_LOGF("dispatch", "worker %d started", tid);
    while (1) {
        dispatch(tid);
    }
    return NULL;
}
