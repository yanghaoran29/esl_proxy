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

main's runtime is still being wired up, so the trace reflects what actually runs:

- **Orchestrator lane** — one `orch_submit` envelope bar around `aicpu_orchestration_entry`, plus one instant mark per task that becomes ready (from `submit()` / `batch_submit()` in [ring_buf.h](../include/ring_buf.h)). This is fully populated (e.g. ~600 submit marks for the qwen3 decode case).
- **Dispatch lanes** — work-gated dispatch/finish stamps in [src/dispatch.c](../src/dispatch.c) (`send_task` / `set_completed`). These fire once the orchestration→dispatch ready path and executor completion signalling are connected; until then they stay empty.
- **Cutter / manager lanes** — registered, instrumentation-ready; populate when those workers are wired.

All hooks are work-gated (emit only on real sends/completions), so a hot spin loop never floods the rings.

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
