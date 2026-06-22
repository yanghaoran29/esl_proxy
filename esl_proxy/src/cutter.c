#include "cutter.h"
#include "log.h"
#include "ring_buf.h"
#ifdef ESL_PROXY_ONBOARD
#include "onboard_shm_sync.h"
#include "onboard_sync.h"
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

uint16_t  g_predecessor_cnt[RING_SIZE];
uint16_t g_commit_task_id = 0;
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
        int idx = task_id;
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
    WORKER_LOGF("min_uncomplete_task,%u, completed_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d", \
        g_min_uncomplete_task, end, g_ctrl_t[0].ready_queue[2].cnt, g_ctrl_t[0].ready_queue[1].cnt);
}

void add_successors(uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE]) {
    uint16_t end = atomic_load(&g_task_id);
    uint16_t tmp = g_commit_task_id + ADD_BATCH_SIZE;
    end = tmp > end ? end : tmp;
    while ( g_commit_task_id <= end)
    {
        uint16_t task_idx = g_commit_task_id;
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        if (ptr->cnt <= 0) {
            // WORKER_LOGF("ready, task_id,%u, task_idx,%u, ready_cnt,%u", g_commit_task_id, task_idx, *ready_cnt);
            task_type_t type = g_basic_buf[g_commit_task_id].type;
            int q = ready_queue_index(type);
            rq_buf[q][ready_cnt[q]] = g_commit_task_id++;
            ready_cnt[q]++;
            WORKER_LOGF("ready_cnt[%d],%d", q, ready_cnt[q]);
            continue;
        }
        uint16_t precessor_id = 0;
        uint16_t predecessor_cnt = 0;
        while (ptr->cnt > 0)
        {
            precessor_id = *(ptr->exp);
            uint16_t precessor_idx = precessor_id;
            if(g_state_buf[precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint16_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = g_commit_task_id;
                g_state_buf[precessor_idx].successor_cnt++;
                predecessor_cnt++;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u", precessor_id, g_successor_buf[precessor_idx].cnt, g_commit_task_id);
            }
            ptr->cnt--;
            ptr->exp++;
        }
        g_predecessor_cnt[task_idx] = predecessor_cnt;
        if (predecessor_cnt <= 0)
        {
            task_type_t type = g_basic_buf[g_commit_task_id].type;
            int q = ready_queue_index(type);
            rq_buf[q][ready_cnt[q]] = g_commit_task_id;
            ready_cnt[q]++;
            WORKER_LOGF("ready_cnt[%d],%d", q, ready_cnt[q]);
        }
        g_commit_task_id++;
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
            g_predecessor_cnt[succ_id & RING_MASK]--;
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, g_predecessor_cnt[succ_id & RING_MASK]);
            if (g_predecessor_cnt[succ_id & RING_MASK] < 1) {
                task_type_t type = g_basic_buf[succ_id].type;
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                WORKER_LOGF("ready_cnt[%d],%d",type, ready_cnt[type]);
            }
        }
    }
}

void deal_completed_queue() {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint16_t cq_buf[CUTTER_BATCH_SIZE];
        uint16_t rq_buf[2][LOCAL_BUFFER_SIZE];
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
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_invalidate_shared_before_worker();
#endif
    deal_completed_queue();
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_flush_after_cutter();
#endif
}

void cutter_loop_run(void)
{
#ifdef ESL_PROXY_ONBOARD
    /* worker_enter called from executor.cpp */
#else
    init_state_buf();
#endif
#ifdef ESL_PROXY_ONBOARD
    esl_onboard_invalidate_shared_before_worker();
#endif
    while (!atomic_load(&g_is_done)) {
#ifdef ESL_PROXY_ONBOARD
        esl_onboard_invalidate_shared_before_worker();
#endif
        cutter_loop_once();
    }
    int stall = 0;
    uint16_t prev_commit = g_commit_task_id;
    while (g_commit_task_id < atomic_load(&g_task_id)) {
        esl_onboard_invalidate_shared_before_worker();
        cutter_loop_once();
        if (g_commit_task_id != prev_commit) {
            prev_commit = g_commit_task_id;
            stall = 0;
        } else {
            stall++;
            if (stall > 10000) {
                WORKER_LOGF("cutter stall timeout: commit=%u task_id=%u", (unsigned)g_commit_task_id,
                            (unsigned)atomic_load(&g_task_id));
                break;
            }
        }
    }
    WORKER_LOGF("cutter, commit_tasks_cnt,%d,completed_task_cnt,%d ", g_commit_task_id,
                g_completed_task_cnt);
}

void *cutter_worker(void *arg)
{
    (void)arg;
    cutter_loop_run();
    return NULL;
}