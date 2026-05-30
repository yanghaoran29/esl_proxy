/*
 * test_swimlane.c - self-test for the swimlane module.
 *
 * Exercises the SWIM_* macro API across several lanes (orchestrator / dispatch /
 * cutter / executor), dumps to JSON, re-reads it, and asserts the expected lanes,
 * record types, and a known task id are present with no dropped records.
 *
 * Build (ON — the only meaningful mode):
 *   gcc -std=c11 -Wall -Werror -Wextra -pedantic -DESL_SWIMLANE \
 *       -I include tests/test_swimlane.c src/swimlane.c -o /tmp/test_swimlane
 *
 * Built without -DESL_SWIMLANE every SWIM_* is a no-op and the test trivially
 * passes (there is nothing to collect).
 */

#include "swimlane.h"

#include <stdio.h>
#include <string.h>

/* lane ids per the plan: orch=0, dispatch=2+d, cutter=4+c, exec=8+e */
#define L_ORCH      0
#define L_DISPATCH0 2
#define L_CUTTER0   4
#define L_EXEC0     8
#define L_EXEC1     9

#define EXEC_TASKS 100

#ifdef ESL_SWIMLANE
#include <assert.h>
#include <stdlib.h>

static char *slurp(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    assert(buf != NULL);
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len) *out_len = (long)got;
    return buf;
}
#endif

int main(void) {
    const char *path = "/tmp/swim_test.json";

    SWIM_INIT();
    SWIM_LANE(L_ORCH, LANE_ORCH, "orch");
    SWIM_LANE(L_DISPATCH0, LANE_DISPATCH, "dispatch-0");
    SWIM_LANE(L_CUTTER0, LANE_CUTTER, "cutter-0");
    SWIM_LANE(L_EXEC0, LANE_EXEC, "exec-0");
    SWIM_LANE(L_EXEC1, LANE_EXEC, "exec-1");

    /* orchestrator submits two tasks */
    SWIM_PHASE_BEGIN(L_ORCH, PH_ORCH_SUBMIT);
    SWIM_PHASE_END(L_ORCH);

    /* dispatch hands task 5 to an executor */
    SWIM_PHASE_BEGIN(L_DISPATCH0, PH_DISPATCH);
    SWIM_STAMP(L_DISPATCH0, 5, SWIM_DISPATCH);
    SWIM_PHASE_END(L_DISPATCH0);

    /* executor lanes run a batch of tasks */
    for (int i = 0; i < EXEC_TASKS; i++) {
        SWIM_TASK_BEGIN((i & 1) ? L_EXEC1 : L_EXEC0, (uint64_t)(i + 1), (uint32_t)(i % 3));
        SWIM_TASK_END((i & 1) ? L_EXEC1 : L_EXEC0, (uint64_t)(i + 1));
    }

    /* cutter finalizes task 5 */
    SWIM_PHASE_BEGIN(L_CUTTER0, PH_COMPLETE);
    SWIM_STAMP(L_CUTTER0, 5, SWIM_FINISH);
    SWIM_PHASE_END(L_CUTTER0);

    SWIM_DUMP(path);
    SWIM_SHUTDOWN();

#ifdef ESL_SWIMLANE
    long len = 0;
    char *json = slurp(path, &len);
    assert(len > 0);

    /* lanes present */
    assert(strstr(json, "\"kind\": \"orchestrator\"") != NULL);
    assert(strstr(json, "\"kind\": \"dispatch\"") != NULL);
    assert(strstr(json, "\"kind\": \"cutter\"") != NULL);
    assert(strstr(json, "\"kind\": \"executor\"") != NULL);

    /* record types and phases present */
    assert(strstr(json, "\"rtype\": \"task\"") != NULL);
    assert(strstr(json, "\"rtype\": \"stamp\"") != NULL);
    assert(strstr(json, "\"rtype\": \"phase\"") != NULL);
    assert(strstr(json, "\"phase\": \"orch_submit\"") != NULL);
    assert(strstr(json, "\"phase\": \"complete\"") != NULL);

    /* known task id and frequency present */
    assert(strstr(json, "\"task_id\": 5") != NULL);
    assert(strstr(json, "\"cntfrq\":") != NULL);

    /* nothing dropped for this sized workload */
    assert(strstr(json, "\"dropped\": 0") != NULL);
    assert(strstr(json, "\"dropped\": 1") == NULL);

    free(json);
    printf("test_swimlane: PASSED (json %ld bytes)\n", len);
#else
    (void)path;
    printf("test_swimlane: swimlane disabled (no-op build) — OK\n");
#endif
    return 0;
}
