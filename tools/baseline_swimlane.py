#!/usr/bin/env python3
"""Resource-constrained zero-dispatch-delay baseline swimlane generator.

For **basic** (and double_buffer): preserves the per-core task order **and**
per-task kernel duration from the matching `report/swimlane/proxy/<mode>/<case>/`
onboard swimlane. Each of 24 AIC + 48 AIV workers runs at most one kernel at a
time; start time = max(predecessors done, same-core previous task done) with
zero extra AICPU dispatch delay.

DAG predecessors from orchestration WORKER_LOG CSV (mathematical deps).

Outputs under report/swimlane/proxy_baseline/<mode>/<case>/ — never overwrites
report/swimlane/proxy/.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "esl_proxy"
SWIM_PROXY = ROOT / "report/swimlane/proxy"
SWIM_BASE = ROOT / "report/swimlane/proxy_baseline"

AIC_CNT = 24
AIV_CNT = 48
WORKER_CNT = AIC_CNT + AIV_CNT
CLOCK_HZ = 50_000_000

CASES = [
    "qwen3_dynamic_manual_scope",
    "qwen3_dynamic_tensormap",
    "paged_attention_unroll",
    "paged_attention_unroll_manual_scope",
]
MODES = ["basic", "double_buffer"]

NEW_RE = re.compile(r"new,task_id,(\d+),type,(\d+),subtask_cnt,(\d+),dur,(\d+)")
SUCC_RE = re.compile(r"succeed,task_id,(\d+),predecessor_id,(\d+)")
TID_RE = re.compile(r"\(t(\d+)\)$")


def ns_to_ticks(ns: int) -> int:
    return ns * CLOCK_HZ // 1_000_000_000


def extract_dag(case_h: str) -> tuple[dict[int, int], dict[int, int], dict[int, list[int]]]:
    tier = ["QWEN3_SPMD_TIER=0"] if case_h.startswith("qwen3") else []
    subprocess.run(
        ["make", f"CASE={case_h}", "WORKER_LOG=1", *tier, "clean"],
        cwd=CORE,
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["make", f"CASE={case_h}", "WORKER_LOG=1", *tier, "all"],
        cwd=CORE,
        check=True,
        capture_output=True,
    )
    logdir = CORE / "log"
    for f in logdir.glob("pto._thread_*.csv"):
        f.unlink()
    subprocess.run(
        ["stdbuf", "-oL", "timeout", "25", "./bin/esl_proxy"],
        cwd=CORE,
        env={**os.environ, "WORKER_LOG": "1", "LOG_OUTPUT_MODE": "0"},
        check=True,
        capture_output=True,
    )
    text = "".join(f.read_text() for f in sorted(logdir.glob("pto._thread_*.csv")))
    durations: dict[int, int] = {}
    types: dict[int, int] = {}
    preds: dict[int, list[int]] = defaultdict(list)
    for tid, typ, _cnt, dur in NEW_RE.findall(text):
        t = int(tid)
        durations[t] = int(dur)
        types[t] = int(typ)
    for tid, pred in SUCC_RE.findall(text):
        preds[int(tid)].append(int(pred))
    preds = {k: sorted(set(v)) for k, v in preds.items()}
    return durations, types, preds


def parse_dag_from_log() -> tuple[dict[int, int], dict[int, int], dict[int, list[int]]]:
    text = "".join(f.read_text() for f in sorted((CORE / "log").glob("pto._thread_*.csv")))
    durations: dict[int, int] = {}
    types: dict[int, int] = {}
    preds: dict[int, list[int]] = defaultdict(list)
    for tid, typ, _c, dur in NEW_RE.findall(text):
        t = int(tid)
        durations[t] = int(dur)
        types[t] = int(typ)
    for tid, pred in SUCC_RE.findall(text):
        preds[int(tid)].append(int(pred))
    return durations, types, {k: sorted(set(v)) for k, v in preds.items()}


def perfetto_tid_to_core(tid: int) -> int:
    """Inverse of swimlane_converter core_to_tid = 10000 + core_id * 10."""
    return (tid - 10000) // 10


def load_actual_swimlane(case: str, mode: str) -> tuple[list[tuple[int, int, int]], dict[int, int]] | None:
    """Return (assignments, task_duration_ns) from proxy onboard swimlane.

    assignments: [(core_id, task_id, actual_start_ticks), ...]
    task_duration_ns: task_id -> kernel execution time (end - start) from proxy.
    """
    rec = SWIM_PROXY / mode / case / "l2_swimlane_records.json"
    tr = SWIM_PROXY / mode / case / "l2_swimlane_trace.json"
    if rec.is_file():
        data = json.loads(rec.read_text())
        freq = int(data["metadata"]["clock_freq_hz"])
        assignments = []
        durations: dict[int, int] = {}
        for row in data["aicore_tasks"]:
            core, tid, _reg, st, en = int(row[0]), int(row[1]), int(row[2]), int(row[3]), int(row[4])
            assignments.append((core, tid, st))
            durations[tid] = (en - st) * 1_000_000_000 // freq
        return assignments, durations

    if not tr.is_file():
        return None

    events = json.loads(tr.read_text())["traceEvents"]
    assignments = []
    durations: dict[int, int] = {}
    for e in events:
        if e.get("ph") != "X":
            continue
        m = TID_RE.search(e.get("name", ""))
        if not m:
            continue
        tid_task = int(m.group(1))
        core = perfetto_tid_to_core(int(e["tid"]))
        start_ticks = int(e["ts"]) * (CLOCK_HZ // 1_000_000)
        assignments.append((core, tid_task, start_ticks))
        durations[tid_task] = round(float(e["dur"]) * 1000)  # perfetto us (float) -> ns
    return assignments, durations


def load_actual_assignments(case: str, mode: str) -> list[tuple[int, int, int]] | None:
    got = load_actual_swimlane(case, mode)
    return got[0] if got else None


def per_core_order(assignments: list[tuple[int, int, int]]) -> dict[int, list[int]]:
    """Sort by actual start time within each core → fixed execution order."""
    by_core: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for core, tid, start in assignments:
        by_core[core].append((start, tid))
    queues: dict[int, list[int]] = {}
    for core in range(WORKER_CNT):
        items = sorted(by_core.get(core, []))
        queues[core] = [tid for _st, tid in items]
    return queues


def simulate_order_preserving(
    durations: dict[int, int],
    preds: dict[int, list[int]],
    core_queues: dict[int, list[int]],
) -> tuple[list[list[int]], dict]:
    """Reschedule with fixed per-core order, 72 workers, zero dispatch gap."""
    core_idx = [0] * WORKER_CNT
    core_free = [0] * WORKER_CNT
    finish: dict[int, int] = {}
    records: list[list[int]] = []
    scheduled: set[int] = set()
    total = sum(len(q) for q in core_queues.values())

    while len(scheduled) < total:
        progress = False
        for w in range(WORKER_CNT):
            q = core_queues.get(w, [])
            if core_idx[w] >= len(q):
                continue
            tid = q[core_idx[w]]
            if tid in scheduled:
                core_idx[w] += 1
                continue
            ps = preds.get(tid, [])
            if any(p not in finish for p in ps):
                continue
            ready_at = max((finish[p] for p in ps), default=0)
            start = max(core_free[w], ready_at)
            end = start + durations[tid]
            core_free[w] = end
            finish[tid] = end
            records.append([w, tid, tid, ns_to_ticks(start), ns_to_ticks(end)])
            scheduled.add(tid)
            core_idx[w] += 1
            progress = True
        if not progress:
            missing = total - len(scheduled)
            raise RuntimeError(f"order-preserving baseline stuck: {missing} tasks unsscheduled")

    span_ns = max(finish.values()) if finish else 0
    meta = {
        "span_ns": span_ns,
        "span_ms": span_ns / 1e6,
        "tasks": len(scheduled),
        "model": "24AIC+48AIV order+duration from proxy, zero-dispatch-gap",
    }
    return records, meta


def core_types_metadata() -> list[str]:
    return ["aic"] * AIC_CNT + ["aiv"] * AIV_CNT


def write_records(path: Path, records: list[list[int]], mode: str, meta: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "l2_swimlane_level": 1,
        "metadata": {
            "clock_freq_hz": CLOCK_HZ,
            "num_cores": WORKER_CNT,
            "core_types": core_types_metadata(),
            "baseline": True,
            "dispatch_mode": mode,
            "model": meta.get("model", ""),
        },
        "aicore_tasks": records,
    }
    path.write_text(json.dumps(payload, indent=2))


def span_from_records(data: dict, freq: int) -> float:
    rows = data["aicore_tasks"]
    starts = [r[3] for r in rows]
    ends = [r[4] for r in rows]
    return (max(ends) - min(starts)) / freq * 1000


def emit_perfetto(records_path: Path, case: str) -> None:
    trace_out = records_path.parent / "l2_swimlane_trace.json"
    fn_json = ROOT / "tools" / (
        "qwen3_func_names.json" if case.startswith("qwen3") else "paged_attention_func_names.json"
    )
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "swimlane_trace.py"),
        str(records_path),
        "-o",
        str(trace_out),
        "--case",
        case,
        "--spmd-tier",
        "0",
        "--func-names",
        str(fn_json),
    ]
    subprocess.run(cmd, check=True, capture_output=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--case", action="append", help="case name without .h (repeatable)")
    ap.add_argument("--skip-sim", action="store_true", help="reuse last sim CSV in esl_proxy/log")
    ap.add_argument("--mode", choices=MODES, action="append", help="dispatch mode (default: both)")
    args = ap.parse_args()
    cases = args.case or CASES
    modes = args.mode or MODES

    summary = []
    for case in cases:
        print(f"=== {case} ===", flush=True)
        if not args.skip_sim:
            _nominal, _types, preds = extract_dag(case + ".h")
        else:
            _nominal, _types, preds = parse_dag_from_log()

        for mode in modes:
            loaded = load_actual_swimlane(case, mode)
            if loaded is None:
                print(f"  skip {mode}: no actual swimlane", flush=True)
                continue
            assignments, durations = loaded

            core_queues = per_core_order(assignments)
            records, meta = simulate_order_preserving(durations, preds, core_queues)
            out_dir = SWIM_BASE / mode / case
            rec_path = out_dir / "l2_swimlane_records.json"
            write_records(rec_path, records, mode, meta)
            emit_perfetto(rec_path, case)

            actual_data = json.loads(
                (SWIM_PROXY / mode / case / "l2_swimlane_records.json").read_text()
            ) if (SWIM_PROXY / mode / case / "l2_swimlane_records.json").is_file() else {
                "aicore_tasks": [[a[0], a[1], a[1], a[2], a[2]] for a in assignments],
                "metadata": {"clock_freq_hz": CLOCK_HZ},
            }
            freq = int(actual_data.get("metadata", {}).get("clock_freq_hz", CLOCK_HZ))
            if (SWIM_PROXY / mode / case / "l2_swimlane_records.json").is_file():
                actual_ms = span_from_records(actual_data, freq)
            else:
                tr = SWIM_PROXY / mode / case / "l2_swimlane_trace.json"
                ev = json.loads(tr.read_text())["traceEvents"]
                xs = [e for e in ev if e.get("ph") == "X"]
                actual_ms = (max(e["ts"] + e["dur"] for e in xs) - min(e["ts"] for e in xs)) / 1000

            overhead_ms = actual_ms - meta["span_ms"]
            overhead_pct = (actual_ms / meta["span_ms"] - 1) * 100 if meta["span_ms"] else 0
            summary.append(
                {
                    "case": case,
                    "mode": mode,
                    "baseline_ms": meta["span_ms"],
                    "actual_ms": actual_ms,
                    "sched_overhead_ms": overhead_ms,
                    "sched_overhead_pct": overhead_pct,
                    "order_preserving": True,
                }
            )
            print(
                f"  [{mode}] baseline={meta['span_ms']:.2f}ms actual={actual_ms:.2f}ms "
                f"overhead=+{overhead_ms:.2f}ms ({overhead_pct:+.1f}%) -> {rec_path}",
                flush=True,
            )

    report_path = ROOT / "report" / "baseline_sched_analysis.json"
    report_path.write_text(json.dumps(summary, indent=2))
    print(f"\nWrote {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
