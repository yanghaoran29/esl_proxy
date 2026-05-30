#ifndef CONF_H
#define CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048

#define SUCC_NODE_CNT 64
#define THREAD_CNT 2

#define AIC_OSTD 2
#define AIC_CNT 60

#define CUTTER_BATCH_SIZE 64
#define CUTTER_THREAD_CNT 2

#define DISPATCH_THREAD_CNT 2

#define EXE_TYPE_CNT 2

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#define WORKER_LOG 1

/* Swimlane lane-id mapping (pure arithmetic; only consumed when ESL_SWIMLANE is
 * built, but harmless to define unconditionally). Lanes: orch=0, manager=1,
 * dispatch=2+d, cutter=4+c, executor=8+slot where slot in [0, SL_EXEC_COUNT). */
#define SL_ORCH                 0
#define SL_MANAGER              1
#define SL_DISPATCH(d)          (2 + (d))
#define SL_CUTTER(c)            (4 + (c))
#define SL_EXEC(tid, type, idx) (8 + (tid) * (EXE_TYPE_CNT * AIC_CNT) + (type) * AIC_CNT + (idx))
#define SL_EXEC_COUNT           (THREAD_CNT * EXE_TYPE_CNT * AIC_CNT)

#endif /* CONF_H */
