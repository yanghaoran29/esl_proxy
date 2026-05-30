#include "conf.h"
#include "task.h"
#include "queue.h"

void cutter(queue_t* cq, queue_t* rq) {
    uint16_t cq_buf[CUTTER_BATCH_SIZE];
    uint16_t rq_buf[AIC_CNT];
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;
    uint16_t ready_cnt = 0;

    uint16_t cnt = batch_dequeue(cq, buf, AIC_CNT);
    for (uint32_t i = 0; i < cnt; i++) {
        task_id = buf[i];
        idx = task_id & RING_MASK;
        succ_cnt = g_state_buf[idx].successor_cnt;
        for (uint16_t j = 0; j < succ_cnt; j++) {
            succ_id = g_successor_buf[idx].successor[j];
            g_predecessor_buf[succ_id & RING_MASK]--;
            if (g_predecessor_buf[succ_id & RING_MASK] == 0) {
                rq_buf[ready_cnt++] = succ_id;
            }
        }
    }
    while(!batch_enqueue(rq, rq_buf, ready_cnt)) {
        wait();
    }
}

