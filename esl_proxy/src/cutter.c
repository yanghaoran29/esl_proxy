#include "cutter.h"
#include "log.h"
#include "ring_buf.h"
#ifdef ESL_PROXY_ONBOARD
#include "onboard/onboard_crosscore_sync.h"
#include "onboard/onboard_trace.h"
#include "onboard/onboard_config.h"
#include "onboard_log.h"
#include "spin.h"
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

task_state* g_state_buf;

void init_state_buf(void) {
#ifdef ESL_PROXY_ONBOARD
    static task_state state_storage[RING_SIZE];
    g_state_buf = state_storage;
#else
    g_state_buf = malloc(sizeof(task_state) * RING_SIZE);
#endif
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_state_buf[i].state = TASK_STATUS_CREATING;
        g_state_buf[i].task_id = 0;
        g_state_buf[i].successor_cnt = 0;
    }
}

extern atomic_int g_min_uncomplete_task;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern _Atomic bool g_orch_is_done;
extern _Atomic bool g_is_done;

_Atomic uint16_t g_predecessor_cnt[RING_SIZE];
_Atomic uint16_t g_commit_task_id = 0;
uint16_t g_completed_task_cnt = 0;

static inline int ready_queue_index(task_type_t type)
{
    if (type == TASK_TYPE_MIX) {
        return TASK_TYPE_CUBE;
    }
    return (int)type;
}

static inline bool update_task_state(uint16_t cnt, uint16_t* cq_buf)
{
    if (cnt <= 0)
        return false;
    
    uint16_t task_id;
    uint16_t idx;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        uint16_t idx = task_id & RING_MASK;
        g_state_buf[idx].state = TASK_STATUS_COMPLETED;
    }
    uint16_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store(&g_min_uncomplete_task, i);
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_publish_counters();
#endif
    WORKER_LOGF("min_uncomplete_task,%u, completed_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d", \
        g_min_uncomplete_task, end, g_ctrl_t[0].ready_queue[2].cnt, g_ctrl_t[0].ready_queue[1].cnt);
}

void add_successors(uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE]) {
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    uint16_t commit = atomic_load_explicit(&g_commit_task_id, memory_order_acquire);
    uint16_t tmp = commit + ADD_BATCH_SIZE;
    end = tmp > end ? end : tmp;
    while (commit < end) {
        uint16_t task_idx = commit;
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_consume_task_slot(task_idx);
#endif
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        if (ptr->cnt <= 0) {
            task_type_t type = g_basic_buf[task_idx & RING_MASK].type;
            int q = ready_queue_index(type);
            rq_buf[q][ready_cnt[q]] = commit;
            ready_cnt[q]++;
            WORKER_LOGF("ready_cnt[%d],%d", q, ready_cnt[q]);
            commit++;
            atomic_store_explicit(&g_commit_task_id, commit, memory_order_release);
#ifdef ESL_PROXY_ONBOARD
            esl_onboard_publish_counters();
#endif
            continue;
        }
        uint16_t precessor_id = 0;
        uint16_t predecessor_cnt = 0;
        while (ptr->cnt > 0) {
            precessor_id = *(ptr->exp);
            uint16_t precessor_idx = precessor_id;
            if (g_state_buf[precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint16_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = commit;
                g_state_buf[precessor_idx].successor_cnt++;
                predecessor_cnt++;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u", precessor_id,
                            g_successor_buf[precessor_idx].cnt, commit);
            }
            ptr->cnt--;
            ptr->exp++;
        }
        atomic_store_explicit(&g_predecessor_cnt[task_idx], predecessor_cnt, memory_order_release);
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_publish_predecessor_cnt(task_idx);
#endif
        if (predecessor_cnt <= 0) {
            task_type_t type = g_basic_buf[task_idx & RING_MASK].type;
            int q = ready_queue_index(type);
            rq_buf[q][ready_cnt[q]] = commit;
            ready_cnt[q]++;
            WORKER_LOGF("ready_cnt[%d],%d", q, ready_cnt[q]);
        }
        commit++;
        atomic_store_explicit(&g_commit_task_id, commit, memory_order_release);
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_publish_counters();
#endif
    }
}

void send_2_ready_queue(uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE]) {
    for (uint16_t j = 0; j < 2; j++) {
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[j];
        if (ready_cnt[j] > 0)
        {
            WORKER_LOGF("batch_enqueue,%d,cnt,%u,first,%d",j, ready_cnt[j], rq_buf[j][0]);
            batch_enqueue(rq, rq_buf[j], ready_cnt[j]);
        }
    }
}

void resolve_dep(uint16_t cnt, uint16_t* cq_buf, uint16_t rq_buf[][LOCAL_BUFFER_SIZE], uint16_t* ready_cnt) {
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[idx];
        succ_cnt = (uint16_t)st.successor_cnt;
        WORKER_LOGF("completed,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint16_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            uint16_t pred_left =
                atomic_fetch_sub_explicit(&g_predecessor_cnt[succ_id & RING_MASK], 1U, memory_order_acq_rel) - 1U;
#ifdef ESL_PROXY_ONBOARD
            esl_onboard_publish_predecessor_cnt(succ_id);
#endif
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, pred_left);
            if (pred_left < 1) {
                /* ready_queue_index maps MIX→CUBE (MIX dispatched as AIC). Using
                 * the raw type indexes rq_buf[MIX=2] out of bounds (rq_buf is
                 * [2][...]), losing the successor — qwen3's MIX tasks got stuck. */
                int q = ready_queue_index(g_basic_buf[succ_id].type);
                rq_buf[q][ready_cnt[q]] = succ_id;
                ready_cnt[q]++;
                WORKER_LOGF("ready_cnt[%d],%d", q, ready_cnt[q]);
            }
        }
    }
}

void deal_completed_queue() {
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_invalidate_sched_snapshot();
#endif
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint16_t cq_buf[CUTTER_BATCH_SIZE];
        /* static: LOCAL_BUFFER_SIZE is sized to RING_SIZE to hold a high-fanout
         * burst (one completion can ready hundreds of successors), which is too
         * large for the AICPU stack. Safe because CUTTER_THREAD_CNT == 1. */
        static uint16_t rq_buf[2][LOCAL_BUFFER_SIZE];
        uint16_t ready_cnt[2] = {0, 0};
        queue_t *cq = &g_ctrl_t[i].completed_queue;
        uint16_t cnt = CUTTER_BATCH_SIZE;
        batch_dequeue(cq, cq_buf, &cnt);
        g_completed_task_cnt += cnt;
        // for (size_t i = 0; i < cnt; i++)
        // {
        //     WORKER_LOGF("cutter, completed_task_id,%d ", cq_buf[i]);
        // }
        update_task_state(cnt, cq_buf);
        add_successors(ready_cnt, rq_buf);
        resolve_dep(cnt, cq_buf, rq_buf, ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
    }
}

void cutter_loop_once(void)
{
    deal_completed_queue();
}

void cutter_loop_run(void)
{
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_trace(ESL_AICPU_ROLE_CUTTER, ESL_TRACE_CUTTER_LOOP_ENTER, 0, 0, 0);
    uint32_t loop_iter = 0;
#else
    init_state_buf();
#endif
    while (!atomic_load(&g_is_done)) {
#ifdef ESL_PROXY_ONBOARD
        if ((loop_iter & 0x3FFFFU) == 0) {
            esl_onboard_trace(ESL_AICPU_ROLE_CUTTER, ESL_TRACE_CUTTER_LOOP, loop_iter,
                              (uint64_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire),
                              (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire));
        }
        loop_iter++;
#endif
        cutter_loop_once();
#ifdef ESL_PROXY_ONBOARD
        spin_wait();
#endif
    }
    int stall = 0;
    uint16_t prev_commit = atomic_load_explicit(&g_commit_task_id, memory_order_acquire);
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_trace(ESL_AICPU_ROLE_CUTTER, ESL_TRACE_CUTTER_DRAIN, prev_commit,
                      (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire), 0);
#endif
    while (atomic_load_explicit(&g_commit_task_id, memory_order_acquire) <
           atomic_load_explicit(&g_task_id, memory_order_acquire)) {
        cutter_loop_once();
        uint16_t cur_commit = atomic_load_explicit(&g_commit_task_id, memory_order_acquire);
        if (cur_commit != prev_commit) {
            prev_commit = cur_commit;
            stall = 0;
        } else {
            stall++;
            if (stall > 10000) {
                LOG_ERROR("cutter stall timeout: commit=%u task_id=%u", (unsigned)cur_commit,
                          (unsigned)atomic_load(&g_task_id));
                esl_onboard_trace(ESL_AICPU_ROLE_CUTTER, ESL_TRACE_CUTTER_DRAIN, (uint64_t)cur_commit,
                                  (uint64_t)atomic_load_explicit(&g_task_id, memory_order_acquire),
                                  0xDEADBEEFULL);
                break;
            }
        }
    }
    WORKER_LOGF("cutter, commit_tasks_cnt,%d,completed_task_cnt,%d ",
                (int)atomic_load_explicit(&g_commit_task_id, memory_order_acquire),
                g_completed_task_cnt);
}

void *cutter_worker(void *arg)
{
    (void)arg;
    cutter_loop_run();
    return NULL;
}