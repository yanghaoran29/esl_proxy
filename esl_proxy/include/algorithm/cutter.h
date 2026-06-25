/*
 * cutter.h - Dependency resolution worker
 */

#ifndef CUTTER_H
#define CUTTER_H

#include "conf.h"
#include "queue.h"
#include "task.h"

void cutter(queue_t *cq, queue_t *rq);
void *cutter_worker(void *arg);
void init_state_buf(void);

/* Onboard AICPU: cutter / dispatch / orchestrator on separate threads */
void cutter_loop_once(void);
void cutter_loop_run(void);

#endif /* CUTTER_H */
