#!/usr/bin/env python3
"""swimlane_converter.py - raw esl_proxy swimlane JSON -> Perfetto Chrome-Trace.

Reads the raw record dump produced by swim_dump() (swimlane_records.json) and
emits a Chrome Trace Event Format file (merged_swimlane.json) that loads directly
in Perfetto (https://ui.perfetto.dev) or chrome://tracing.

Lanes are grouped into processes by kind (orchestrator / dispatch / cutter /
executor); each lane is a thread (tid = lane id). Task and phase rows become
duration ("X") events; dispatch/finish stamps become instant ("i") marks and,
when a task id appears in more than one role, flow ("s"/"f") arrows
dispatch -> executor -> cutter.

Usage:
  python3 tools/swimlane_converter.py [INPUT] [-o OUTPUT] [--name-map NAME_MAP]

  INPUT      raw swimlane JSON (default: ./swimlane_records.json)
  -o         output trace (default: alongside INPUT as merged_swimlane.json)
  --name-map optional JSON mapping {"<func_id>": "name"} for nicer task labels
"""

import argparse
import json
import os
import sys

# kind string -> (pid, human process name). Lower pid sorts higher in Perfetto.
KIND_PID = {
    "orchestrator": (1, "Orchestrator"),
    "manager":      (2, "Manager"),
    "dispatch":     (3, "Dispatch"),
    "cutter":       (4, "Cutter"),
    "executor":     (5, "Executor"),
}


def load_json(path):
    with open(path, "r") as f:
        return json.load(f)


def base_time(records):
    """Minimum non-zero timestamp across all records (the trace origin)."""
    lo = None
    for r in records:
        for key in ("start_time", "dispatch_time", "finish_time"):
            t = r.get(key, 0)
            if t > 0 and (lo is None or t < lo):
                lo = t
    return lo if lo is not None else 0


def make_to_us(base, cntfrq):
    scale = 1.0e6 / float(cntfrq if cntfrq else 1)

    def to_us(ticks):
        return (ticks - base) * scale

    return to_us


def func_label(func_id, name_map):
    key = str(func_id)
    if name_map and key in name_map:
        return name_map[key]
    # default: CUBE/VECTOR/MIX guess from the small task-type ints we emit
    return {0: "CUBE", 1: "VECTOR", 2: "MIX"}.get(func_id, "func_%d" % func_id)


def convert(raw, name_map=None):
    cntfrq = raw.get("cntfrq", 1)
    lanes = raw.get("lanes", [])
    records = raw.get("records", [])
    to_us = make_to_us(base_time(records), cntfrq)

    events = []

    # --- process & thread metadata -------------------------------------------
    seen_pids = {}
    for lane in lanes:
        pid, pname = KIND_PID.get(lane["kind"], (9, "Other"))
        if pid not in seen_pids:
            events.append({"ph": "M", "name": "process_name", "pid": pid, "tid": 0,
                           "args": {"name": pname}})
            seen_pids[pid] = pname
        tname = "%s-%d" % (lane["kind"], lane["id"])
        events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": lane["id"],
                       "args": {"name": tname}})

    # --- duration bars + instant marks ---------------------------------------
    # task_id -> role timestamps, for flow arrows
    flow = {}

    for r in records:
        pid, _ = KIND_PID.get(r["kind"], (9, "Other"))
        tid = r["lane"]
        rtype = r["rtype"]

        if rtype == "task":
            ts = to_us(r["start_time"])
            dur = max(to_us(r["end_time"]) - ts, 0.0)
            events.append({
                "ph": "X", "pid": pid, "tid": tid,
                "name": func_label(r["func_id"], name_map),
                "ts": ts, "dur": dur,
                "args": {"task_id": r["task_id"], "func_id": r["func_id"]},
            })
            flow.setdefault(r["task_id"], {})["exec_start"] = ts
            flow[r["task_id"]]["exec_end"] = ts + dur

        elif rtype == "phase":
            ts = to_us(r["start_time"])
            dur = max(to_us(r["end_time"]) - ts, 0.0)
            events.append({
                "ph": "X", "pid": pid, "tid": tid,
                "name": r["phase"],
                "ts": ts, "dur": dur,
                "args": {"task_id": r["task_id"]},
            })

        elif rtype == "stamp":
            if r["finish_time"] > 0:
                ts = to_us(r["finish_time"])
                label, role = "finish", "finish"
            else:
                ts = to_us(r["dispatch_time"])
                label, role = "dispatch", "dispatch"
            events.append({
                "ph": "i", "pid": pid, "tid": tid, "name": label,
                "ts": ts, "s": "t", "args": {"task_id": r["task_id"]},
            })
            flow.setdefault(r["task_id"], {})[role] = ts

    # --- flow arrows: dispatch -> exec -> finish, when ids line up ------------
    fid = 0
    for task_id, m in flow.items():
        if "dispatch" in m and "exec_start" in m:
            events.append({"ph": "s", "id": fid, "pid": KIND_PID["dispatch"][0],
                           "tid": -1, "name": "dispatch", "cat": "dep",
                           "ts": m["dispatch"], "args": {"task_id": task_id}})
            events.append({"ph": "f", "id": fid, "bp": "e", "pid": KIND_PID["executor"][0],
                           "tid": -1, "name": "dispatch", "cat": "dep",
                           "ts": m["exec_start"], "args": {"task_id": task_id}})
            fid += 1
        if "exec_end" in m and "finish" in m:
            events.append({"ph": "s", "id": fid, "pid": KIND_PID["executor"][0],
                           "tid": -1, "name": "finish", "cat": "dep",
                           "ts": m["exec_end"], "args": {"task_id": task_id}})
            events.append({"ph": "f", "id": fid, "bp": "e", "pid": KIND_PID["cutter"][0],
                           "tid": -1, "name": "finish", "cat": "dep",
                           "ts": m["finish"], "args": {"task_id": task_id}})
            fid += 1

    return {"traceEvents": events, "displayTimeUnit": "ns"}


def main(argv):
    ap = argparse.ArgumentParser(description="esl_proxy swimlane -> Perfetto trace")
    ap.add_argument("input", nargs="?", default="swimlane_records.json")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--name-map", default=None)
    args = ap.parse_args(argv)

    if not os.path.exists(args.input):
        sys.stderr.write("input not found: %s\n" % args.input)
        return 1

    raw = load_json(args.input)
    name_map = load_json(args.name_map) if args.name_map else None
    trace = convert(raw, name_map)

    out = args.output or os.path.join(os.path.dirname(os.path.abspath(args.input)),
                                      "merged_swimlane.json")
    with open(out, "w") as f:
        json.dump(trace, f, indent=1)

    n_x = sum(1 for e in trace["traceEvents"] if e["ph"] == "X")
    n_flow = sum(1 for e in trace["traceEvents"] if e["ph"] == "s")
    print("wrote %s: %d events (%d bars, %d flows) from %d lanes"
          % (out, len(trace["traceEvents"]), n_x, n_flow, len(raw.get("lanes", []))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
