/*
 * cutter.h - Dependency resolution worker
 */

#ifndef CUTTER_H
#define CUTTER_H

#include "conf.h"
#include "queue.h"
#include "task.h"

extern task_state state_storage[RING_SIZE];
extern task_state *g_state_buf;

void cutter(queue_t *cq, queue_t *rq);
void *cutter_worker(void *arg);
void init_state_buf(void);

#endif /* CUTTER_H */
