/*
 * fake_orchestration_api.h - Stub interfaces filling gaps that block porting
 * orchestration .cpp files (e.g. qwen3_decode) onto esl_proxy.
 *
 * Tensor storage:
 *   Fake_Tensor<N, T> is a template struct with a single member `T data[N]`.
 *   Instances are declared as globals so each tensor lives in the program's
 *   data / BSS segment. Element type T is short (bfloat16) or int (float32).
 *
 * Task submission goes through task.h's task_desc directly. The wrappers
 * fake_submit_*_task were removed — users populate a task_desc literal
 * (per spec 008) and pass it to fake_submit().
 *
 * Layout convention (matches spec 008 FR-010/011):
 *   task_desc       = description (id/type/mode/kernel/index/count/prio/data)
 *   task_desc.data  -> fake_runtime_info (input/output/inout pointers + scalars)
 *   deps            -> fake_add_dep / fake_add_deps (-> g_dep_buf)
 *   submission      -> fake_submit (-> ready_queue)
 */

#ifndef DAG_FAKE_ORCHESTRATION_API_H
#define DAG_FAKE_ORCHESTRATION_API_H

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <initializer_list>

#include "task.h"

/* === Fake_Tensor template ===
 *
 * Parameters: N = element count (compile-time int), T = element type (short
 * for bfloat16, int for float32). Member is a fixed-size array; place
 * instances at namespace scope so storage lives in .bss / .data.
 */
template <int N, typename T>
struct Fake_Tensor {
    T data[N];
};

/* === Task id allocator (placeholder for ring buffer index allocator) === */
static constexpr task_id_t FAKE_TASK_ID_INVALID = static_cast<task_id_t>(0xFFFFu);

inline task_id_t fake_next_task_id() {
    static task_id_t next = 0;
    return next++;
}

/* === Runtime info payload (target of task_desc.data) ===
 *
 * Stub equivalent of one entry in spec 008's Runtime Information Ring Buffer.
 * One task_desc carries one kernel pointer — MIX tasks rely solely on
 * task_desc.kernel + task_desc.type=TASK_TYPE_MIX (Dispatch routes them to a
 * dual-capability executor; no extra AIV kernel slot is needed here).
 */
struct fake_runtime_info {
    std::vector<void*>    inputs;
    std::vector<void*>    outputs;
    std::vector<void*>    inouts;
    std::vector<int64_t>  scalars;
};

/* === Dependency edges (target: g_dep_buf successor links per spec 008) === */
inline void fake_add_dep(task_id_t consumer, task_id_t producer) {
    (void)consumer; (void)producer;
    /* Real impl: append `consumer` to g_dep_buf[ring_idx(producer)] successor
     * list (3 inline + linked extension entries), bump consumer's pred count. */
}

inline void fake_add_deps(task_id_t consumer, std::initializer_list<task_id_t> producers) {
    for (task_id_t p : producers) {
        if (p != FAKE_TASK_ID_INVALID) fake_add_dep(consumer, p);
    }
}

template <typename It>
inline void fake_add_deps_range(task_id_t consumer, It first, It last) {
    for (auto it = first; it != last; ++it) {
        if (*it != FAKE_TASK_ID_INVALID) fake_add_dep(consumer, *it);
    }
}

/* === Submission (target: ready_enqueue(type, mode, ...)) === */
inline void fake_submit(const task_desc& d) {
    (void)d;
    /* Real impl:
     *   memcpy(&g_basic_buf[ring_idx(d.id)],   &d,  sizeof(task_desc));
     *   memcpy(g_runtime_buf[ring_idx(d.id)],  d.data, ...);
     *   ready_enqueue(d.type, d.mode, &d);
     */
}

/* === Manual-dependency scope (gap: spec 001 Graph) === */
inline void fake_scope_begin() {}
inline void fake_scope_end()   {}

struct FakeManualScope {
    FakeManualScope()  { fake_scope_begin(); }
    ~FakeManualScope() { fake_scope_end(); }
};

#define FAKE_MANUAL_SCOPE() FakeManualScope __fake_scope_guard

#endif /* DAG_FAKE_ORCHESTRATION_API_H */
