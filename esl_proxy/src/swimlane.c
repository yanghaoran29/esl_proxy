/*
 * swimlane.c - Swimlane collection runtime + raw-JSON dumper.
 *
 * Owns the single definitions of the global lane registry and per-lane rings.
 * The ENTIRE file is gated by ESL_SWIMLANE; a default build never compiles or
 * links it.
 *
 * Each lane is written by exactly one thread; swim_dump() runs after the run is
 * quiesced, so no atomics are needed. Rings fill up to SWIM_LANE_CAP then drop,
 * counting drops per lane.
 */

#ifdef ESL_SWIMLANE

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L /* mkdir() under -std=c11 */
#endif

#include "swimlane.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    swim_record_t *ring;
    uint32_t cap;
    uint32_t head;     /* committed records in [0, head) */
    uint32_t dropped;
    uint8_t  kind;
    uint8_t  active;
    char     name[32];
    swim_record_t pend_task;   /* single-writer scratch for begin/end */
    swim_record_t pend_phase;
} swim_lane_t;

static swim_lane_t g_lanes[SWIM_MAX_LANES];
static uint64_t g_freq = 1;

void swim_init(void) {
    memset(g_lanes, 0, sizeof(g_lanes));
    g_freq = swim_freq();
}

void swim_lane_register(uint16_t lane, uint8_t kind, const char *name) {
    swim_lane_t *l = &g_lanes[lane];
    l->ring = (swim_record_t *)malloc((size_t)SWIM_LANE_CAP * sizeof(swim_record_t));
    l->cap = SWIM_LANE_CAP;
    l->head = 0;
    l->dropped = 0;
    l->kind = kind;
    l->active = 1;
    snprintf(l->name, sizeof(l->name), "%s", name ? name : "");
}

static inline void swim_commit(swim_lane_t *l, const swim_record_t *r) {
    if (l->head < l->cap) {
        l->ring[l->head++] = *r;
    } else {
        l->dropped++;
    }
}

void swim_task_begin(uint16_t lane, uint64_t task_id, uint32_t func_id) {
    swim_lane_t *l = &g_lanes[lane];
    swim_record_t *r = &l->pend_task;
    memset(r, 0, sizeof(*r));
    r->start = swim_now();
    r->task_id = task_id;
    r->func_id = func_id;
    r->lane = lane;
    r->kind = l->kind;
    r->rtype = SWIM_RT_TASK;
}

void swim_task_end(uint16_t lane, uint64_t task_id) {
    swim_lane_t *l = &g_lanes[lane];
    l->pend_task.task_id = task_id;
    l->pend_task.end = swim_now();
    swim_commit(l, &l->pend_task);
}

void swim_task_record(uint16_t lane, uint64_t task_id, uint32_t func_id,
                      uint64_t start, uint64_t end) {
    swim_lane_t *l = &g_lanes[lane];
    swim_record_t r;
    memset(&r, 0, sizeof(r));
    r.start = start;
    r.end = end;
    r.task_id = task_id;
    r.func_id = func_id;
    r.lane = lane;
    r.kind = l->kind;
    r.rtype = SWIM_RT_TASK;
    swim_commit(l, &r);
}

void swim_stamp(uint16_t lane, uint64_t task_id, int which) {
    swim_lane_t *l = &g_lanes[lane];
    swim_record_t r;
    memset(&r, 0, sizeof(r));
    const uint64_t now = swim_now();
    r.start = now;
    r.end = now;
    r.task_id = task_id;
    r.lane = lane;
    r.kind = l->kind;
    r.rtype = SWIM_RT_STAMP;
    if (which == SWIM_FINISH) {
        r.finish = now;
    } else {
        r.dispatch = now;
    }
    swim_commit(l, &r);
}

void swim_phase_begin(uint16_t lane, uint8_t phase) {
    swim_lane_t *l = &g_lanes[lane];
    swim_record_t *r = &l->pend_phase;
    memset(r, 0, sizeof(*r));
    r->start = swim_now();
    r->lane = lane;
    r->kind = l->kind;
    r->phase = phase;
    r->rtype = SWIM_RT_PHASE;
}

void swim_phase_end(uint16_t lane) {
    swim_lane_t *l = &g_lanes[lane];
    l->pend_phase.end = swim_now();
    swim_commit(l, &l->pend_phase);
}

static const char *swim_kind_str(uint8_t kind) {
    switch (kind) {
    case LANE_ORCH:     return "orchestrator";
    case LANE_MANAGER:  return "manager";
    case LANE_DISPATCH: return "dispatch";
    case LANE_CUTTER:   return "cutter";
    case LANE_EXEC:     return "executor";
    default:            return "unknown";
    }
}

static const char *swim_phase_str(uint8_t phase) {
    switch (phase) {
    case PH_DISPATCH:    return "dispatch";
    case PH_COMPLETE:    return "complete";
    case PH_ORCH_SUBMIT: return "orch_submit";
    default:             return "unknown";
    }
}

static const char *swim_rtype_str(uint8_t rtype) {
    switch (rtype) {
    case SWIM_RT_TASK:  return "task";
    case SWIM_RT_STAMP: return "stamp";
    case SWIM_RT_PHASE: return "phase";
    default:            return "unknown";
    }
}

/* Create every parent directory of `path` (mkdir -p); the file itself is left
 * to fopen. Errors (EEXIST, etc.) are ignored on purpose. */
static void swim_mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0755);
            *p = '/';
        }
    }
}

void swim_dump(const char *path) {
    swim_mkdir_p(path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "swim_dump: cannot open %s\n", path);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"cntfrq\": %" PRIu64 ",\n", g_freq);

    fprintf(f, "  \"lanes\": [");
    int first = 1;
    for (uint32_t i = 0; i < SWIM_MAX_LANES; i++) {
        const swim_lane_t *l = &g_lanes[i];
        if (!l->active) continue;
        fprintf(f, "%s\n    {\"id\": %u, \"kind\": \"%s\", \"name\": \"%s\", \"records\": %u, \"dropped\": %u}",
                first ? "" : ",", i, swim_kind_str(l->kind), l->name, l->head, l->dropped);
        first = 0;
    }
    fprintf(f, "\n  ],\n");

    fprintf(f, "  \"records\": [");
    first = 1;
    for (uint32_t i = 0; i < SWIM_MAX_LANES; i++) {
        const swim_lane_t *l = &g_lanes[i];
        if (!l->active) continue;
        for (uint32_t k = 0; k < l->head; k++) {
            const swim_record_t *r = &l->ring[k];
            fprintf(f,
                    "%s\n    {\"lane\": %u, \"kind\": \"%s\", \"rtype\": \"%s\", "
                    "\"task_id\": %" PRIu64 ", \"func_id\": %u, \"phase\": \"%s\", "
                    "\"start_time\": %" PRIu64 ", \"end_time\": %" PRIu64 ", "
                    "\"dispatch_time\": %" PRIu64 ", \"finish_time\": %" PRIu64 "}",
                    first ? "" : ",", r->lane, swim_kind_str(r->kind), swim_rtype_str(r->rtype),
                    r->task_id, r->func_id, swim_phase_str(r->phase),
                    r->start, r->end, r->dispatch, r->finish);
            first = 0;
        }
    }
    fprintf(f, "\n  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

void swim_shutdown(void) {
    for (uint32_t i = 0; i < SWIM_MAX_LANES; i++) {
        free(g_lanes[i].ring);
        g_lanes[i].ring = NULL;
        g_lanes[i].active = 0;
    }
}

#else  /* !ESL_SWIMLANE — avoid an empty translation unit under -pedantic */
typedef int swim_disabled_translation_unit;
#endif /* ESL_SWIMLANE */
