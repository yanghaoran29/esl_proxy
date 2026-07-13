#!/usr/bin/env python3
"""DAG-aware zero-gap baseline swimlane generator.

Reconstructs ``report/swimlane/proxy_baseline/`` from
``report/swimlane/proxy/`` by DAG-aware timeline compression:

  - Each subtask stays on its original core.
  - The per-core subtask order is preserved (sorted by actual start time).
  - DAG dependencies are respected: a task starts only after ALL its
    predecessors have *finished* (all their subtasks done).
  - The scheduling gap is zero: start = max(predecessor finishes, core free),
    with no additional dispatch delay.

DAG predecessors are extracted by running the esl_proxy simulation with
WORKER_LOG=1, which produces CSV logs containing ``new,task_id,...`` and
``succeed,task_id,predecessor_id,...`` entries.

Source mapping:
  - basic mode         -> proxy/basic/<case>/                     (non-SPMD)
  - double_buffer mode -> proxy/lane2_shared/double_buffer/<case>/tier0/  (SPMD tier 0)
  - SPMD tiers 0-4     -> proxy/lane2_shared/<mode>/<case>/tier<N>/

Outputs under report/swimlane/proxy_baseline/<mode>/<case>/ and writes
the comparison analysis to report/zero_gap_analysis.json (standard) and
report/zero_gap_spmd_analysis.json (SPMD tiers).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]       # .../esl_proxy
CORE = ROOT / "esl_proxy"                         # .../esl_proxy/esl_proxy (build dir)
SWIM_PROXY = ROOT / "report/swimlane/proxy"
SWIM_BASE = ROOT / "report/swimlane/proxy_baseline"
CLOCK_HZ = 50_000_000

AIC_CNT = 24
AIV_CNT = 48
WORKER_CNT = AIC_CNT + AIV_CNT

CASES = [
    "qwen3_dynamic_manual_scope",
    "qwen3_dynamic_tensormap",
    "paged_attention_unroll",
    "paged_attention_unroll_manual_scope",
]
QWEN3_CASES = ["qwen3_dynamic_manual_scope", "qwen3_dynamic_tensormap"]
MODES = ["basic", "double_buffer"]
SPMD_TIERS = [0, 1, 2, 3, 4]

NEW_RE = re.compile(r"new,task_id,(\d+),type,(\d+),subtask_cnt,(\d+),dur,(\d+)")
SUCC_RE = re.compile(r"succeed,task_id,(\d+),predecessor_id,(\d+)")


# ---------------------------------------------------------------------------
# DAG extraction (simulation-based)
# ---------------------------------------------------------------------------

def extract_dag(case: str, spmd_tier: int = 0) -> dict[int, list[int]]:
    """Build and run simulation to extract the task DAG.

    Returns ``preds: {task_id: [predecessor_task_ids]}``.
    """
    case_h = case + ".h"
    tier_flags = [f"QWEN3_SPMD_TIER={spmd_tier}"] if case.startswith("qwen3") else []

    # Clean and build
    subprocess.run(
        ["make", f"CASE={case_h}", "WORKER_LOG=1", *tier_flags, "clean"],
        cwd=CORE, check=True, capture_output=True,
    )
    subprocess.run(
        ["make", f"CASE={case_h}", "WORKER_LOG=1", *tier_flags, "all"],
        cwd=CORE, check=True, capture_output=True,
    )

    # Remove old logs and run simulation
    for f in (CORE / "log").glob("pto._thread_*.csv"):
        f.unlink()
    # Redirect stdout/stderr to files to avoid pipe buffer deadlock
    # (WORKER_LOG=1 produces large output that can fill pipe buffers)
    log_out = CORE / "log" / "_sim_stdout.txt"
    with open(log_out, "w") as fout:
        subprocess.run(
            ["timeout", "120", "./bin/esl_proxy"],
            cwd=CORE,
            env={**os.environ, "WORKER_LOG": "1", "LOG_OUTPUT_MODE": "0"},
            check=True, stdout=fout, stderr=subprocess.STDOUT,
        )

    # Parse logs
    text = "".join(f.read_text() for f in sorted((CORE / "log").glob("pto._thread_*.csv")))
    preds: dict[int, list[int]] = defaultdict(list)
    for tid, pred in SUCC_RE.findall(text):
        preds[int(tid)].append(int(pred))
    return {k: sorted(set(v)) for k, v in preds.items()}


# ---------------------------------------------------------------------------
# Swimlane data loading
# ---------------------------------------------------------------------------

def load_swimlane(records_path: Path) -> tuple[
    dict[int, list[tuple[int, int, int]]],  # task_id -> [(core, dur_ns, start_ticks), ...]
    dict[int, int],                         # task_id -> min_start_ticks (for ordering)
]:
    """Load actual swimlane records and compute per-subtask info.

    Each row in the swimlane is a **subtask** — an independent execution
    on one core. A task may have multiple subtasks on different cores
    (SPMD) or even multiple subtasks on the same core.

    Returns per-task subtask lists (core, duration_ns, actual_start_ticks)
    and per-task min start (for tie-breaking in scheduling).
    """
    data = json.loads(records_path.read_text())
    freq = int(data["metadata"]["clock_freq_hz"])
    tasks = data["aicore_tasks"]

    by_task: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    for row in tasks:
        tid = int(row[1])
        core = int(row[0])
        start = int(row[3])
        end = int(row[4])
        dur_ns = (end - start) * 1_000_000_000 // freq
        by_task[tid].append((core, dur_ns, start))

    # Sort subtasks by (core, actual_start) for deterministic same-core ordering
    task_subtasks: dict[int, list[tuple[int, int, int]]] = {}
    task_start: dict[int, int] = {}
    for tid, entries in by_task.items():
        task_subtasks[tid] = sorted(entries, key=lambda x: (x[0], x[2]))
        task_start[tid] = min(s for _, _, s in entries)

    return task_subtasks, task_start


# ---------------------------------------------------------------------------
# Per-core-order-preserving DAG-aware simulation
# ---------------------------------------------------------------------------

def simulate_dag_aware(
    task_subtasks: dict[int, list[tuple[int, int, int]]],
    task_start: dict[int, int],
    preds: dict[int, list[int]],
) -> list[list[int]]:
    """DAG-aware simulation with zero dispatch delay, preserving per-core order.

    Each subtask stays on its original core.  Within each core, subtasks
    execute in the same order as the actual swimlane (sorted by actual
    start time).  A subtask starts at::

        start = max(task_ready_time, core_free[core])

    where ``task_ready_time`` is the finish time of the task's latest DAG
    predecessor (0 if none).  There is no additional dispatch delay.

    A task is *finished* when ALL its subtasks have finished; only then
    can successor tasks become ready.

    Because per-core order is preserved and the gap is zero, every
    subtask's baseline start ≤ its actual start (by induction on the
    topological order), guaranteeing ``baseline_makespan ≤ actual_makespan``.
    """
    swimlane_tids = set(task_subtasks.keys())
    filtered_preds: dict[int, list[int]] = {
        tid: [p for p in preds.get(tid, []) if p in swimlane_tids]
        for tid in swimlane_tids
    }

    # Build per-core subtask queues (sorted by actual start time)
    core_queues: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    # core -> [(task_id, dur_ns, actual_start), ...]
    for tid, entries in task_subtasks.items():
        for core, dur_ns, start in entries:
            core_queues[core].append((tid, dur_ns, start))
    for core in core_queues:
        core_queues[core].sort(key=lambda x: (x[2], x[0]))  # by actual_start, then task_id

    # Task subtask counts and finish tracking
    task_total: dict[int, int] = {tid: len(e) for tid, e in task_subtasks.items()}
    task_done: dict[int, int] = defaultdict(int)   # subtasks scheduled so far
    task_end: dict[int, int] = {}                   # max end of scheduled subtasks
    task_finish: dict[int, int] = {}                # set when ALL subtasks done

    core_ptr: dict[int, int] = {c: 0 for c in core_queues}
    core_free = [0] * WORKER_CNT

    total_subtasks = sum(len(q) for q in core_queues.values())
    scheduled_count = 0
    records: list[list[int]] = []

    while scheduled_count < total_subtasks:
        # Find the front-of-queue subtask with the earliest possible start
        # whose task is DAG-ready (all predecessors finished).
        best = None          # (start, core, tid, dur_ns)
        for core, queue in core_queues.items():
            ptr = core_ptr[core]
            if ptr >= len(queue):
                continue
            tid, dur_ns, _actual_start = queue[ptr]
            ps = filtered_preds.get(tid, [])
            if not all(p in task_finish for p in ps):
                continue
            ready_at = max((task_finish[p] for p in ps), default=0)
            start = max(ready_at, core_free[core])
            if best is None or start < best[0] or \
               (start == best[0] and _actual_start < best[4]):
                best = (start, core, tid, dur_ns, _actual_start)

        if best is None:
            # Deadlock — collect diagnostics
            stuck = []
            for core, queue in core_queues.items():
                ptr = core_ptr[core]
                if ptr < len(queue):
                    tid = queue[ptr][0]
                    missing = [p for p in filtered_preds.get(tid, []) if p not in task_finish]
                    stuck.append(f"core{core}: task {tid} waiting for {missing}")
            raise RuntimeError(
                f"Deadlock: {len(stuck)} cores blocked.\n  " +
                "\n  ".join(stuck[:5]))

        start, core, tid, dur_ns, _ = best
        end = start + dur_ns
        core_free[core] = end
        task_end[tid] = max(task_end.get(tid, 0), end)
        task_done[tid] += 1
        if task_done[tid] == task_total[tid]:
            task_finish[tid] = task_end[tid]

        core_ptr[core] += 1
        scheduled_count += 1
        records.append([
            core, tid, tid,
            start * CLOCK_HZ // 1_000_000_000,
            end * CLOCK_HZ // 1_000_000_000,
        ])

    return records


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def core_types_metadata() -> list[str]:
    return ["aic"] * AIC_CNT + ["aiv"] * AIV_CNT


def write_records(path: Path, records: list[list[int]], mode: str, source_data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    meta = dict(source_data.get("metadata", {}))
    meta["baseline"] = True
    meta["dispatch_mode"] = mode
    meta["model"] = "DAG-aware zero-gap, per-core order preserved (predecessor finish -> successor start, no dispatch delay)"
    payload = {
        "l2_swimlane_level": source_data.get("l2_swimlane_level", 1),
        "metadata": meta,
        "aicore_tasks": records,
    }
    path.write_text(json.dumps(payload, indent=2))


def emit_perfetto(records_path: Path, case: str, spmd_tier: int = 0) -> None:
    trace_out = records_path.parent / "l2_swimlane_trace.json"
    fn_json = ROOT / "tools" / (
        "qwen3_func_names.json" if case.startswith("qwen3") else "paged_attention_func_names.json"
    )
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "swimlane_trace.py"),
        str(records_path),
        "-o", str(trace_out),
        "--case", case,
        "--spmd-tier", str(spmd_tier),
        "--func-names", str(fn_json),
    ]
    subprocess.run(cmd, check=True, capture_output=True)


def span_ms_from_records(data: dict) -> float:
    freq = int(data["metadata"]["clock_freq_hz"])
    tasks = data["aicore_tasks"]
    starts = [r[3] for r in tasks]
    ends = [r[4] for r in tasks]
    return (max(ends) - min(starts)) / freq * 1000


# ---------------------------------------------------------------------------
# Source path mapping
# ---------------------------------------------------------------------------

def source_records(mode: str, case: str, tier: int | None = None) -> Path:
    """Return the proxy source records path."""
    if tier is not None:
        return SWIM_PROXY / "lane2_shared" / mode / case / f"tier{tier}" / "l2_swimlane_records.json"
    if mode == "basic":
        return SWIM_PROXY / "basic" / case / "l2_swimlane_records.json"
    # double_buffer standard uses lane2_shared tier0
    return SWIM_PROXY / "lane2_shared" / "double_buffer" / case / "tier0" / "l2_swimlane_records.json"


def baseline_dir(mode: str, case: str, tier: int | None = None) -> Path:
    if tier is not None:
        return SWIM_BASE / "lane2_shared" / mode / case / f"tier{tier}"
    return SWIM_BASE / mode / case


# ---------------------------------------------------------------------------
# Main processing
# ---------------------------------------------------------------------------

def process_one(
    case: str, mode: str, spmd_tier: int, preds: dict[int, list[int]],
    src_path: Path, out_dir: Path, no_trace: bool = False,
) -> dict:
    """Process a single case/mode/tier combination."""
    src_data = json.loads(src_path.read_text())
    actual_ms = span_ms_from_records(src_data)

    task_subtasks, task_start = load_swimlane(src_path)
    records = simulate_dag_aware(task_subtasks, task_start, preds)

    rec_path = out_dir / "l2_swimlane_records.json"
    write_records(rec_path, records, mode, src_data)
    if not no_trace:
        emit_perfetto(rec_path, case, spmd_tier=spmd_tier)

    baseline_data = json.loads(rec_path.read_text())
    baseline_ms = span_ms_from_records(baseline_data)

    saved_ms = actual_ms - baseline_ms
    speedup_pct = (saved_ms / actual_ms * 100) if actual_ms > 0 else 0

    cores_used = len(set(c for r in records for c in [r[0]]))
    entry = {
        "case": case,
        "mode": mode,
        "actual_ms": round(actual_ms, 6),
        "baseline_ms": round(baseline_ms, 6),
        "saved_ms": round(saved_ms, 6),
        "speedup_pct": round(speedup_pct, 4),
        "tasks": len(records),
        "cores_used": cores_used,
        "source": str(src_path.relative_to(ROOT)),
        "model": "DAG-aware zero-gap, per-core order preserved",
    }
    label = f"[{mode}/{case}" + (f"/tier{spmd_tier}" if case.startswith("qwen3") else "") + "]"
    print(
        f"  {label} actual={actual_ms:.3f}ms baseline={baseline_ms:.3f}ms "
        f"saved={saved_ms:.3f}ms ({speedup_pct:.1f}% faster) -> {rec_path}",
        flush=True,
    )
    return entry


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--case", action="append", help="case name (repeatable, default: all)")
    ap.add_argument("--mode", choices=MODES, action="append", help="dispatch mode (default: both)")
    ap.add_argument("--no-trace", action="store_true", help="skip Perfetto trace generation")
    ap.add_argument("--spmd", action="store_true",
                    help="process qwen3 SPMD tiers 0-4 from lane2_shared directory")
    args = ap.parse_args()
    cases = args.case or CASES
    modes = args.mode or MODES

    # --- DAG cache ---
    dag_cache: dict[tuple[str, int], dict[int, list[int]]] = {}

    def get_dag(case: str, tier: int) -> dict[int, list[int]]:
        key = (case, tier)
        if key not in dag_cache:
            # qwen3 tensormap shares DAG with manual_scope
            dag_case = case.replace("qwen3_dynamic_tensormap", "qwen3_dynamic_manual_scope")
            dag_key = (dag_case, tier)
            if dag_key not in dag_cache:
                print(f"  Extracting DAG: {dag_case} tier={tier} ...", flush=True)
                dag_cache[dag_key] = extract_dag(dag_case, tier)
            dag_cache[key] = dag_cache[dag_key]
        return dag_cache[key]

    # --- Standard (non-SPMD) processing ---
    if not args.spmd:
        summary = []
        for case in cases:
            tier = 0 if case.startswith("qwen3") else 0
            # For paged_attention, no SPMD tier
            if not case.startswith("qwen3"):
                # Extract DAG once (no tier)
                if (case, 0) not in dag_cache:
                    print(f"  Extracting DAG: {case} ...", flush=True)
                    dag_cache[(case, 0)] = extract_dag(case, 0)
            else:
                get_dag(case, 0)

            for mode in modes:
                src = source_records(mode, case)
                if not src.is_file():
                    print(f"  skip {mode}/{case}: source not found at {src}", flush=True)
                    continue
                entry = process_one(case, mode, 0 if case.startswith("qwen3") else 0,
                                    dag_cache[(case, 0)], src, baseline_dir(mode, case),
                                    args.no_trace)
                summary.append(entry)

        report_path = ROOT / "report" / "zero_gap_analysis.json"
        report_path.write_text(json.dumps(summary, indent=2))
        print(f"\nWrote {report_path}")

    # --- SPMD tier processing (qwen3 only) ---
    if args.spmd:
        spmd_summary = []
        for case in QWEN3_CASES:
            for tier in SPMD_TIERS:
                get_dag(case, tier)
                for mode in modes:
                    src = source_records(mode, case, tier=tier)
                    if not src.is_file():
                        print(f"  skip {mode}/{case}/tier{tier}: source not found", flush=True)
                        continue
                    entry = process_one(case, mode, tier,
                                        dag_cache[(case, tier)], src,
                                        baseline_dir(mode, case, tier=tier),
                                        args.no_trace)
                    entry["spmd_tier"] = tier
                    spmd_summary.append(entry)

        spmd_report_path = ROOT / "report" / "zero_gap_spmd_analysis.json"
        spmd_report_path.write_text(json.dumps(spmd_summary, indent=2))
        print(f"\nWrote {spmd_report_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
