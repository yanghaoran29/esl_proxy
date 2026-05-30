# Swimlane Profiling

A per-thread timeline (swimlane) of the esl_proxy runtime — orchestrator submit activity and (as the runtime fills in) dispatch / cutter / executor timing — exported as a [Perfetto](https://ui.perfetto.dev) Chrome-Trace. Ported in spirit from simpler's L2 swimlane.

## On / off (full collection)

Swimlane is a binary, compile-time feature. There are no runtime levels.

- **Off (default):** build normally. Every `SWIM_*` site is `((void)0)`; the binary contains no swimlane code, data, or symbols and behaves exactly as before (no JSON written).
- **On:** add `-DESL_SWIMLANE` and link `src/swimlane.c`. Full collection is active; `main` snapshots the swimlane after a bounded run window and writes `swimlane_records.json` on exit.

Everything swimlane-related is gated solely by `#ifdef ESL_SWIMLANE`.

## Build & run (main runtime)

```sh
SRC="src/main.c src/dispatch.c src/cutter.c src/manager.c src/log.c src/shm.c"

# off — unchanged behavior, no swim symbols, no JSON
gcc -std=c11 -Wall -Wextra -pthread -I include -I cases $SRC -latomic -o /tmp/esl_off

# on — runs, then writes ./swimlane_records.json
gcc -std=c11 -Wall -Wextra -pthread -DESL_SWIMLANE -I include -I cases \
    $SRC src/swimlane.c -latomic -o /tmp/esl_on
./esl_on
```

`-latomic` is required (the 16-byte `task_state` uses libatomic). On the swimlane build `main` runs the orchestration, lets the worker threads drain for ~500 ms, then dumps and exits.

Module self-test (no runtime needed):

```sh
gcc -std=c11 -Wall -Werror -Wextra -pedantic -DESL_SWIMLANE -I include \
    tests/test_swimlane.c src/swimlane.c -o /tmp/test_swimlane && /tmp/test_swimlane
```

## View in Perfetto

```sh
python3 tools/swimlane_converter.py swimlane_records.json -o merged_swimlane.json
# optional nicer task names: --name-map name_map.json   (e.g. {"0":"rmsnorm","1":"q_proj"})
```

Open <https://ui.perfetto.dev> (or `chrome://tracing`) and load `merged_swimlane.json`. Lanes are grouped into processes by role; each thread row is one lane. Task and phase rows are duration bars; dispatch/finish are instant marks; matching task ids draw `dispatch → executor → cutter` flow arrows.

## What populates today

The orchestration→dispatch path is now connected, so root tasks flow end to end:

- **Orchestrator lane** — one `orch_submit` envelope bar around `aicpu_orchestration_entry`, plus one instant mark per task that becomes ready (from `submit()` / `batch_submit()` in [ring_buf.h](../include/ring_buf.h)). Fully populated (~600 marks for the qwen3 decode case).
- **Dispatch lanes** — a dispatch stamp per task handed to an executor slot and a finish stamp per completion ([src/dispatch.c](../src/dispatch.c) `send_task` / `set_completed`). Fully populated (~1200 stamps).
- **Executor lanes** — one bar per task, width = the task's declared `duration`, on the assigned executor slot (`8 + tid*120 + exe_type*60 + idx`). ~600 bars.
- **Cutter / manager lanes** — registered, instrumentation-ready; populate when those workers are wired.

All hooks are work-gated (emit only on real sends/completions), so a hot spin loop never floods the rings.

## Known issues / temporary workarounds

These are stop-gaps to get the pipeline flowing for profiling; revisit as main's runtime matures.

1. **Stand-in executor.** main has no executor threads/kernels yet, so [src/dispatch.c](../src/dispatch.c) `simulate_executor()` marks every busy slot as just-completed each loop, closing assign→complete. Replace with the real 003 executor; the executor-bar duration is the task's declared `duration`, not a measured one.
2. **MIX → AIC routing.** `msg_bitmap` / `task_id_map1` / `task_id_map2` in `ctrl_t` are only `EXE_TYPE_CNT`(2) wide (no MIX column), but `send_task` is driven by `task_type_t` which includes `MIX(=2)`. Indexing those arrays with MIX overruns them, so `exe_type_of()` routes MIX onto the AIC(CUBE) pool for now. A real MIX executor pool is needed.
3. **queue.h `batch_dequeue` read-from-tail bug (fixed).** The original `batch_dequeue` read from `queue->tail` (where `batch_enqueue` writes) and advanced `tail`, returning uninitialized slots. Fixed to read/advance `head`. Worth a proper lock-free ring with wraparound (the `// TODO: RING LOOP` / `// TODO: atomic protect` markers).
4. **Cutter still a stub.** `cutter_worker` does not call `cutter()`, so successor predecessor counts are never decremented — only the initial root tasks drain; the rest of the DAG stays pending. Wire the cutter to drain the full graph.

## How it works

- **Lanes** — `orchestrator=0`, `manager=1`, `dispatch d → 2+d`, `cutter c → 4+c`, `executor → 8+slot`. Helpers `SL_ORCH` / `SL_DISPATCH(d)` / `SL_CUTTER(c)` / `SL_EXEC(tid,type,idx)` live in [conf.h](../include/conf.h). Each lane is written by exactly one thread (single-writer), so collection needs no locks; `swim_dump()` runs after the run window.
- **Timestamps** — aarch64 reads `CNTVCT_EL0` (frequency from `CNTFRQ_EL0`); other arches fall back to `clock_gettime(CLOCK_MONOTONIC)` in ns. The frequency is written to the raw JSON as `cntfrq`; the converter turns ticks into microseconds.
- **Buffers** — one fixed ring per lane (`SWIM_LANE_CAP` records, default 4096, override with `-DSWIM_LANE_CAP=N`), up to `SWIM_MAX_LANES` (256) lanes. Full rings drop and count drops per lane. Stamps on unregistered lanes are safely discarded (counted as drops), never crash.

## Instrumentation API

Use only the macros (from [include/swimlane.h](../include/swimlane.h)); never call `swim_*` directly.

| Macro | Use |
|-------|-----|
| `SWIM_INIT()` | once at startup |
| `SWIM_LANE(lane, kind, name)` | register a lane (kind = `LANE_ORCH/MANAGER/DISPATCH/CUTTER/EXEC`) |
| `SWIM_TASK_BEGIN/END(lane, task_id[, func_id])` | bracket a task's window (single in-flight per lane) |
| `SWIM_TASK_RECORD(lane, task_id, func_id, start, end)` | one-shot full bar with explicit start/end (synthesized spans) |
| `SWIM_STAMP(lane, task_id, SWIM_DISPATCH\|SWIM_FINISH)` | one-shot dispatch/finish mark on the caller's own lane |
| `SWIM_PHASE_BEGIN/END(lane[, phase])` | bracket a phase (`PH_DISPATCH/PH_COMPLETE/PH_ORCH_SUBMIT`) |
| `SWIM_NOW()` | read the raw timer (for synthesized spans) |
| `SWIM_DUMP(path)` / `SWIM_SHUTDOWN()` | snapshot JSON / free buffers |

## Raw JSON schema (`swimlane_records.json`)

```json
{
  "cntfrq": 100000000,
  "lanes":   [{"id": 0, "kind": "orchestrator", "name": "orchestrator", "records": 607, "dropped": 0}],
  "records": [{"lane": 0, "kind": "orchestrator", "rtype": "stamp", "task_id": 2, "func_id": 0,
               "phase": "dispatch", "start_time": 123, "end_time": 123,
               "dispatch_time": 123, "finish_time": 0}]
}
```

`rtype` is `task` / `stamp` / `phase`. Timestamps are raw ticks (convert with `cntfrq`).
