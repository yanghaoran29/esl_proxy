#!/usr/bin/env python3
"""Rebuild ``report/swimlane/proxy_baseline/`` under TWO baseline schemes and
compute measured-vs-baseline timing diffs (per-task + whole-flow total).

Both baselines eliminate *scheduling idle* and keep every (sub)task's real
measured execution duration and subtask count unchanged.  They differ only in
how much freedom the schedule has:

  Scheme 1 — fixed-topology zero-gap (``simulate_fixed_topo_zero_gap``)
      Inherits the measured topology completely: every subtask stays on its
      original core, per-core execution order is preserved (sorted by measured
      start), and DAG dependencies are honoured.  The only change is that the
      scheduling gap between "all predecessors finished" and "successor start"
      is set to zero -- the whole right side slides left.
          start = max(all-predecessor-tasks-finished, this-core-free)

  Scheme 2 — dependency-constrained optimal (``simulate_optimal_list_schedule``)
      Only the DAG dependency is a hard constraint.  Independent tasks may be
      freely reordered and migrated to ANY idle core of the SAME core type
      (aic->aic, aiv->aiv).  A greedy event-driven list scheduler (ready-time
      priority + LPT core packing) minimises the makespan.  Subtask count and
      per-subtask duration are preserved; this is a near-optimal theoretical
      lower bound (exact makespan minimisation is NP-hard).

This module is SELF-CONTAINED: it extracts the DAG itself (build + run the CPU
functional sim with WORKER_LOG=1) and implements both schedulers from scratch.
The older ``zero_gap_baseline.py`` / ``baseline_swimlane.py`` are DEPRECATED and
are neither imported nor invoked.  Only ``swimlane_trace.py`` (the independent
Perfetto renderer) is reused to emit ``l2_swimlane_trace.json``.

Time unit: everything internal is in hardware *cycles/ticks* (clock = 50 MHz,
1 tick = 20 ns) so measured durations are preserved exactly with no rounding
drift.  Cycles are converted to microseconds only for the diff report
(us = ticks / 50).

Usage:
    python3 tools/rebuild_baselines.py                 # all 52 proxy configs
    python3 tools/rebuild_baselines.py --only lane2_shared/double_buffer/qwen3_dynamic_manual_scope/tier2
    python3 tools/rebuild_baselines.py --no-trace      # skip Perfetto emit (faster)
    python3 tools/rebuild_baselines.py --refresh-dag   # ignore cached DAGs, re-extract
"""
from __future__ import annotations

import argparse
import glob
import heapq
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # .../esl_proxy
CORE = ROOT / "esl_proxy"                            # build dir (.../esl_proxy/esl_proxy)
SWIM_PROXY = ROOT / "report/swimlane/proxy"
SWIM_BASE = ROOT / "report/swimlane/proxy_baseline"
DAG_CACHE = ROOT / "tools" / "_case_dag_cache.json"

CLOCK_HZ = 50_000_000
TICKS_PER_US = CLOCK_HZ // 1_000_000                # 50

# Baseline schedules start at cycle 0, but swimlane_converter derives its
# render origin as ``base_time = min NON-ZERO timestamp`` (a literal 0 is
# excluded). If the earliest task sits at cycle 0 the converter anchors on the
# 2nd-earliest start instead and subtracts it from every event, collapsing the
# first dependency gap in the Perfetto trace. Offsetting all persisted cycles by
# a positive origin (as real hardware counters naturally are) makes the true
# earliest event the min non-zero timestamp, so the trace renders correctly.
# Relative timing is unchanged (the converter subtracts base_time back out).
BASELINE_CYCLE_ORIGIN = 1_000_000
AIC_CNT = 24
AIV_CNT = 48
WORKER_CNT = AIC_CNT + AIV_CNT

# ``depall`` = the COMPLETE mathematical DAG edge (logged before the runtime
# prune). ``succeed`` is the runtime-pruned subset (misses edges to already-
# completed predecessors) and must NOT be used for the ideal baseline.
DEPALL_RE = re.compile(r"depall,task_id,(\d+),predecessor_id,(\d+)")
SUCC_RE = re.compile(r"succeed,task_id,(\d+),predecessor_id,(\d+)")

SCHEME_DIR = {1: "scheme1_fixed_topo", 2: "scheme2_optimal"}
SCHEME_MODEL = {
    1: "Scheme1 fixed-topology zero-gap: original core & per-core order & DAG kept, "
       "scheduling gap zeroed (start = max(all-preds-finished, core-free)), real durations",
    2: "Scheme2 dependency-constrained optimal: only DAG deps hard, independent tasks freely "
       "reordered/migrated within same core type, greedy list-schedule min makespan, real durations",
}


# ---------------------------------------------------------------------------
# Source enumeration
# ---------------------------------------------------------------------------

CASE_NAMES = [
    "qwen3_dynamic_manual_scope",
    "qwen3_dynamic_tensormap",
    "paged_attention_unroll_manual_scope",
    "paged_attention_unroll",
]


def parse_source(rec_path: Path) -> tuple[str, str, int]:
    """Return (rel_dir, case, tier) for a proxy records file.

    ``rel_dir`` is relative to SWIM_PROXY (mirrored under proxy_baseline).
    ``tier`` is the SPMD tier parsed from a ``tierN`` path segment, else 0
    (the non-tiered ``basic/<case>`` and ``double_buffer/<case>`` runs are the
    tier-0 configuration, confirmed by matching task-id counts).
    """
    rel = rec_path.parent.relative_to(SWIM_PROXY)
    parts = rel.parts
    case = next((p for p in parts if p in CASE_NAMES), None)
    if case is None:
        raise ValueError(f"cannot identify case in {rel}")
    tier = 0
    for p in parts:
        m = re.fullmatch(r"tier(\d+)", p)
        if m:
            tier = int(m.group(1))
    return str(rel), case, tier


def enumerate_sources() -> list[tuple[Path, str, str, int]]:
    out = []
    for p in sorted(SWIM_PROXY.glob("**/l2_swimlane_records.json")):
        rel, case, tier = parse_source(p)
        out.append((p, rel, case, tier))
    return out


# ---------------------------------------------------------------------------
# DAG extraction (self-contained: build + run CPU functional sim)
# ---------------------------------------------------------------------------

def dag_family(case: str) -> str:
    """tensormap shares the manual_scope DAG (identical task graph)."""
    return case.replace("qwen3_dynamic_tensormap", "qwen3_dynamic_manual_scope")


def _load_dag_cache() -> dict:
    if DAG_CACHE.is_file():
        try:
            raw = json.loads(DAG_CACHE.read_text())
        except json.JSONDecodeError:
            return {}
        # Only keep populated preds entries (old file has empty scaffolding).
        cache = {}
        for k, v in raw.items():
            if isinstance(v, dict) and v.get("preds"):
                cache[k] = {int(t): list(map(int, ps)) for t, ps in v["preds"].items()}
        return cache
    return {}


def _save_dag_cache(cache: dict) -> None:
    payload = {k: {"preds": {str(t): ps for t, ps in preds.items()}}
               for k, preds in cache.items()}
    DAG_CACHE.write_text(json.dumps(payload, indent=1))


def extract_dag(family: str, tier: int) -> dict[int, list[int]]:
    """Build the sim with WORKER_LOG=1, run it, parse predecessor edges.

    Returns ``preds: {task_id: [predecessor_task_ids]}``.  Runs entirely on the
    CPU functional simulator (ESL_PROXY_SIM_INSTANT_AICORE) -- no NPU needed.
    """
    case_h = family + ".h"
    tier_flags = [f"QWEN3_SPMD_TIER={tier}"] if family.startswith("qwen3") else []
    print(f"    [dag] build+run {family} tier={tier} ...", flush=True)
    subprocess.run(["make", f"CASE={case_h}", "WORKER_LOG=1", *tier_flags, "clean"],
                   cwd=CORE, check=True, capture_output=True)
    subprocess.run(["make", f"CASE={case_h}", "WORKER_LOG=1", *tier_flags, "all"],
                   cwd=CORE, check=True, capture_output=True)
    logdir = CORE / "log"
    # The 72-thread CPU sim can intermittently stall under shared-box load;
    # retry the run (no rebuild needed) a few times before giving up.
    last = None
    for attempt in range(1, 4):
        for f in logdir.glob("pto._thread_*.csv"):
            f.unlink()
        with open(logdir / "_sim_stdout.txt", "w") as fout:
            rc = subprocess.run(["timeout", "300", "./bin/esl_proxy"], cwd=CORE,
                                env={**os.environ, "WORKER_LOG": "1", "LOG_OUTPUT_MODE": "0"},
                                stdout=fout, stderr=subprocess.STDOUT).returncode
        last = rc
        if rc == 0:
            break
        print(f"    [dag] run attempt {attempt} rc={rc}, retrying ...", flush=True)
    if last != 0:
        raise RuntimeError(f"sim run failed after retries (rc={last}) for {family} tier={tier}")
    text = "".join(f.read_text() for f in sorted(logdir.glob("pto._thread_*.csv")))
    edges = DEPALL_RE.findall(text)
    if not edges:
        # Fallback: pre-`depall` build. WARN: this DAG is runtime-pruned and will
        # under-constrain the baseline (successors may start too early).
        print("    [dag] WARNING: no 'depall' edges found -- falling back to pruned "
              "'succeed' edges; rebuild with the depall log for an accurate DAG",
              flush=True)
        edges = SUCC_RE.findall(text)
    preds: dict[int, list[int]] = defaultdict(list)
    for tid, pred in edges:
        preds[int(tid)].append(int(pred))
    return {k: sorted(set(v)) for k, v in preds.items()}


# ---------------------------------------------------------------------------
# Records loading
# ---------------------------------------------------------------------------

class Subtask:
    __slots__ = ("core", "core_type", "tid", "dur", "start", "end")

    def __init__(self, core: int, tid: int, dur: int, start: int, end: int):
        self.core = core
        self.core_type = "aic" if core < AIC_CNT else "aiv"
        self.tid = tid
        self.dur = dur          # ticks (end - start), preserved exactly
        self.start = start      # measured start tick
        self.end = end          # measured end tick


def load_records(path: Path) -> tuple[list[Subtask], dict]:
    """Parse ``aicore_tasks`` rows ``[core, tid, reg, start_cyc, end_cyc]``."""
    data = json.loads(path.read_text())
    subs = []
    for row in data["aicore_tasks"]:
        core, tid, _reg, start, end = int(row[0]), int(row[1]), int(row[2]), int(row[3]), int(row[4])
        subs.append(Subtask(core, tid, end - start, start, end))
    return subs, data


# ---------------------------------------------------------------------------
# Scheme 1 — fixed-topology zero-gap
# ---------------------------------------------------------------------------

def simulate_fixed_topo_zero_gap(subs: list[Subtask],
                                 preds: dict[int, list[int]]) -> list[list[int]]:
    """Zero-gap replay: each subtask on its original core, per-core measured
    order preserved, DAG honoured, zero dispatch gap.  Returns records
    ``[core, tid, tid, start_tick, end_tick]``.
    """
    tids = {s.tid for s in subs}
    fpreds = {t: [p for p in preds.get(t, []) if p in tids] for t in tids}

    # Per-core queue in measured-start order (preserves lane order & assignment).
    core_q: dict[int, list[Subtask]] = defaultdict(list)
    for s in subs:
        core_q[s.core].append(s)
    for c in core_q:
        core_q[c].sort(key=lambda s: (s.start, s.tid))

    task_total = defaultdict(int)
    for s in subs:
        task_total[s.tid] += 1

    core_ptr = {c: 0 for c in core_q}
    core_free = {c: 0 for c in core_q}
    task_done = defaultdict(int)
    task_end: dict[int, int] = {}
    task_finish: dict[int, int] = {}

    total = len(subs)
    done = 0
    records: list[list[int]] = []

    while done < total:
        best = None  # (start, core, subtask)
        for c, q in core_q.items():
            ptr = core_ptr[c]
            if ptr >= len(q):
                continue
            s = q[ptr]
            ps = fpreds[s.tid]
            if not all(p in task_finish for p in ps):
                continue
            ready = max((task_finish[p] for p in ps), default=0)
            start = max(ready, core_free[c])
            if best is None or start < best[0] or (start == best[0] and s.start < best[2].start):
                best = (start, c, s)
        if best is None:
            _deadlock(core_q, core_ptr, fpreds, task_finish)
        start, c, s = best
        end = start + s.dur
        core_free[c] = end
        core_ptr[c] += 1
        done += 1
        task_end[s.tid] = max(task_end.get(s.tid, 0), end)
        task_done[s.tid] += 1
        if task_done[s.tid] == task_total[s.tid]:
            task_finish[s.tid] = task_end[s.tid]
        records.append([c, s.tid, s.tid, start, end])
    return records


def _deadlock(core_q, core_ptr, fpreds, task_finish):
    stuck = []
    for c, q in core_q.items():
        ptr = core_ptr[c]
        if ptr < len(q):
            tid = q[ptr].tid
            missing = [p for p in fpreds.get(tid, []) if p not in task_finish]
            stuck.append(f"core{c}: task {tid} waiting for {missing}")
    raise RuntimeError("Deadlock (scheme1):\n  " + "\n  ".join(stuck[:8]))


# ---------------------------------------------------------------------------
# Scheme 2 — dependency-constrained optimal list schedule
# ---------------------------------------------------------------------------

def _upward_rank(tids, fpreds, succ, weight) -> dict[int, int]:
    """HEFT-style upward rank: rank(t) = weight(t) + max(rank(s) for s in succ).
    Higher rank = more critical (longer path to a sink) -> higher priority."""
    indeg = {t: len(fpreds[t]) for t in tids}
    order, q = [], [t for t in tids if indeg[t] == 0]
    while q:
        t = q.pop()
        order.append(t)
        for s in succ[t]:
            indeg[s] -= 1
            if indeg[s] == 0:
                q.append(s)
    rank = {}
    for t in reversed(order):                       # successors-first
        rank[t] = weight[t] + max((rank[s] for s in succ[t]), default=0)
    return rank


def simulate_optimal_list_schedule(subs: list[Subtask],
                                   preds: dict[int, list[int]]) -> list[list[int]]:
    """Greedy event-driven list scheduler. Only DAG deps constrain; each
    subtask keeps its core TYPE and duration but may run on any idle core of
    that type.  Among tasks ready at the same time the higher critical-path
    (upward rank) task goes first; a task's subtasks are packed longest-first
    onto the earliest-free matching cores.
    Returns records ``[core, tid, tid, start_tick, end_tick]``.
    """
    tids = {s.tid for s in subs}
    fpreds = {t: [p for p in preds.get(t, []) if p in tids] for t in tids}
    task_subs: dict[int, list[Subtask]] = defaultdict(list)
    for s in subs:
        task_subs[s.tid].append(s)

    succ: dict[int, list[int]] = defaultdict(list)
    pending = {t: 0 for t in tids}
    for t in tids:
        for p in fpreds[t]:
            succ[p].append(t)
        pending[t] = len(fpreds[t])

    weight = {t: max(s.dur for s in task_subs[t]) for t in tids}
    rank = _upward_rank(tids, fpreds, succ, weight)

    aic_heap = [(0, c) for c in range(AIC_CNT)]
    aiv_heap = [(0, c) for c in range(AIC_CNT, WORKER_CNT)]
    heapq.heapify(aic_heap)
    heapq.heapify(aiv_heap)
    heap = {"aic": aic_heap, "aiv": aiv_heap}

    task_finish: dict[int, int] = {}
    records: list[list[int]] = []

    # ready key: (ready_tick, -rank, tid) -> earliest-ready then most-critical.
    ready = [(0, -rank[t], t) for t in tids if pending[t] == 0]
    heapq.heapify(ready)

    while ready:
        ready_tick, _nr, tid = heapq.heappop(ready)
        end_of_task = ready_tick
        for s in sorted(task_subs[tid], key=lambda x: -x.dur):
            h = heap[s.core_type]
            free, core = heapq.heappop(h)
            start = max(ready_tick, free)
            end = start + s.dur
            heapq.heappush(h, (end, core))
            records.append([core, tid, tid, start, end])
            end_of_task = max(end_of_task, end)
        task_finish[tid] = end_of_task
        for nxt in succ[tid]:
            pending[nxt] -= 1
            if pending[nxt] == 0:
                rt = max((task_finish[p] for p in fpreds[nxt]), default=0)
                heapq.heappush(ready, (rt, -rank[nxt], nxt))

    if len(records) != len(subs):
        raise RuntimeError(
            f"Deadlock (scheme2): scheduled {len(records)} of {len(subs)} subtasks "
            f"(cycle in DAG?)")
    return records


def _makespan(records: list[list[int]]) -> int:
    return max(r[4] for r in records) - min(r[3] for r in records)


def build_scheme2(subs: list[Subtask], preds: dict[int, list[int]],
                  scheme1_records: list[list[int]]) -> tuple[list[list[int]], bool]:
    """Scheme-2 output = better of {greedy list schedule, scheme-1 schedule}.

    Scheme-1's assignment (each subtask on a core of its own type, deps
    respected) is itself feasible under scheme-2's constraints, so the scheme-2
    optimum is <= scheme-1.  Returning the min guarantees that and keeps the
    'optimal' baseline honest when the greedy heuristic is beaten by the
    measured order.  Returns (records, fell_back_to_scheme1)."""
    greedy = simulate_optimal_list_schedule(subs, preds)
    if _makespan(greedy) <= _makespan(scheme1_records):
        return greedy, False
    return list(scheme1_records), True


# ---------------------------------------------------------------------------
# Output writers
# ---------------------------------------------------------------------------

def core_types_metadata() -> list[str]:
    return ["aic"] * AIC_CNT + ["aiv"] * AIV_CNT


def write_records(path: Path, records: list[list[int]], scheme: int, src_data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    meta = dict(src_data.get("metadata", {}))
    meta.setdefault("clock_freq_hz", CLOCK_HZ)
    meta.setdefault("num_cores", WORKER_CNT)
    meta.setdefault("core_types", core_types_metadata())
    meta["baseline"] = True
    meta["scheme"] = scheme
    meta["model"] = SCHEME_MODEL[scheme]
    meta["cycle_origin"] = BASELINE_CYCLE_ORIGIN
    offset_rows = [[r[0], r[1], r[2], r[3] + BASELINE_CYCLE_ORIGIN, r[4] + BASELINE_CYCLE_ORIGIN]
                   for r in records]
    payload = {
        "l2_swimlane_level": src_data.get("l2_swimlane_level", 1),
        "metadata": meta,
        "aicore_tasks": offset_rows,
    }
    path.write_text(json.dumps(payload, indent=2))


def emit_perfetto(rec_path: Path, case: str, tier: int) -> None:
    trace_out = rec_path.parent / "l2_swimlane_trace.json"
    fn_json = ROOT / "tools" / (
        "qwen3_func_names.json" if case.startswith("qwen3") else "paged_attention_func_names.json")
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "swimlane_trace.py"), str(rec_path),
         "-o", str(trace_out), "--case", case, "--spmd-tier", str(tier),
         "--func-names", str(fn_json), "--no-summary"],
        check=True, capture_output=True)


# ---------------------------------------------------------------------------
# Func labels (for readable per-task diffs)
# ---------------------------------------------------------------------------

_FUNC_MAPS = None
_FUNC_NAMES: dict[str, dict] = {}


def func_label(case: str, tier: int, tid: int) -> str:
    global _FUNC_MAPS
    if _FUNC_MAPS is None:
        p = ROOT / "tools" / "case_task_func_maps.json"
        _FUNC_MAPS = json.loads(p.read_text()) if p.is_file() else {}
    family = "qwen3" if case.startswith("qwen3") else "paged_attention"
    if family not in _FUNC_NAMES:
        fn = ROOT / "tools" / f"{family}_func_names.json"
        _FUNC_NAMES[family] = json.loads(fn.read_text()) if fn.is_file() else {}
    entry = _FUNC_MAPS.get(case) or {}
    if entry and isinstance(next(iter(entry.values())), dict):
        entry = entry.get(str(tier)) or entry.get("0") or {}
    fid = entry.get(str(tid))
    if fid is None:
        return "unknown"
    return _FUNC_NAMES[family].get(str(fid), f"func{fid}")


# ---------------------------------------------------------------------------
# Diff analysis (per-task + total)
# ---------------------------------------------------------------------------

def _task_spans(rows) -> dict[int, tuple[int, int]]:
    """task_id -> (min_start_tick, max_end_tick).  Accepts Subtask list or record rows."""
    span: dict[int, list[int]] = {}
    for r in rows:
        if isinstance(r, Subtask):
            tid, st, en = r.tid, r.start, r.end
        else:
            tid, st, en = r[1], r[3], r[4]
        if tid not in span:
            span[tid] = [st, en]
        else:
            span[tid][0] = min(span[tid][0], st)
            span[tid][1] = max(span[tid][1], en)
    return {t: (a, b) for t, (a, b) in span.items()}


def us(ticks: int) -> float:
    return round(ticks / TICKS_PER_US, 4)


def compute_diff(measured: list[Subtask], baseline_records: list[list[int]],
                 case: str, tier: int) -> dict:
    m_span = _task_spans(measured)
    b_span = _task_spans(baseline_records)
    m0 = min(a for a, _ in m_span.values())
    b0 = min(a for a, _ in b_span.values())
    m_make = max(b for _, b in m_span.values()) - m0
    b_make = max(b for _, b in b_span.values()) - b0

    per_task = []
    for tid in sorted(m_span):
        ms, me = m_span[tid]
        bs, be = b_span[tid]
        per_task.append({
            "task_id": tid,
            "func": func_label(case, tier, tid),
            "measured_start_us": us(ms - m0),
            "measured_end_us": us(me - m0),
            "measured_span_us": us(me - ms),
            "baseline_start_us": us(bs - b0),
            "baseline_end_us": us(be - b0),
            "baseline_span_us": us(be - bs),
            "start_delay_us": us((ms - m0) - (bs - b0)),
        })
    total_diff = m_make - b_make
    return {
        "measured_makespan_us": us(m_make),
        "baseline_makespan_us": us(b_make),
        "total_diff_us": us(total_diff),
        "speedup_pct": round(total_diff / m_make * 100, 3) if m_make else 0.0,
        "tasks": len(per_task),
        "subtasks": len(measured),
        "per_task": per_task,
    }


# ---------------------------------------------------------------------------
# Invariant checks
# ---------------------------------------------------------------------------

def check_invariants(scheme: int, measured: list[Subtask], records: list[list[int]]) -> None:
    assert len(records) == len(measured), \
        f"scheme{scheme}: subtask count {len(records)} != measured {len(measured)}"
    # Duration multiset preserved exactly.
    m_dur = sorted(s.dur for s in measured)
    b_dur = sorted(r[4] - r[3] for r in records)
    assert m_dur == b_dur, f"scheme{scheme}: subtask durations altered"
    # Per (tid, core_type) subtask count preserved.
    def keyed(items, is_rec):
        c = defaultdict(int)
        for r in items:
            if is_rec:
                tid, core = r[1], r[0]
            else:
                tid, core = r.tid, r.core
            ctype = "aic" if core < AIC_CNT else "aiv"
            c[(tid, ctype)] += 1
        return c
    assert keyed(measured, False) == keyed(records, True), \
        f"scheme{scheme}: per-task core-type subtask counts changed"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="process only sources whose rel path contains this substring")
    ap.add_argument("--no-trace", action="store_true", help="skip Perfetto trace emit")
    ap.add_argument("--refresh-dag", action="store_true", help="ignore cached DAGs, re-extract")
    args = ap.parse_args()

    sources = enumerate_sources()
    if args.only:
        sources = [s for s in sources if args.only in s[1]]
    if not sources:
        print("no matching sources", file=sys.stderr)
        return 1

    dag_cache = {} if args.refresh_dag else _load_dag_cache()

    def get_dag(case: str, tier: int) -> dict[int, list[int]]:
        fam = dag_family(case)
        key = f"{fam}:{tier}"
        if key not in dag_cache:
            dag_cache[key] = extract_dag(fam, tier)
            _save_dag_cache(dag_cache)
        return dag_cache[key]

    summaries = {1: [], 2: []}
    print(f"Processing {len(sources)} source config(s)\n")
    for src_path, rel, case, tier in sources:
        preds = get_dag(case, tier)
        measured, src_data = load_records(src_path)
        m_tids = {s.tid for s in measured}
        dag_tids = set(preds) | {p for ps in preds.values() for p in ps}
        missing = m_tids - dag_tids - {0}          # root task 0 has no succeed line
        if missing:
            print(f"  ! {rel}: {len(missing)} swimlane tids not in DAG (tier mismatch?) "
                  f"sample {sorted(missing)[:5]}", flush=True)

        s1_records = simulate_fixed_topo_zero_gap(measured, preds)
        s2_records, s2_fallback = build_scheme2(measured, preds, s1_records)
        scheme_records = {1: s1_records, 2: s2_records}
        for scheme, records in scheme_records.items():
            check_invariants(scheme, measured, records)
            out_dir = SWIM_BASE / SCHEME_DIR[scheme] / rel
            write_records(out_dir / "l2_swimlane_records.json", records, scheme, src_data)
            if not args.no_trace:
                emit_perfetto(out_dir / "l2_swimlane_records.json", case, tier)
            diff = compute_diff(measured, records, case, tier)
            diff.update({"config": rel, "case": case, "tier": tier})
            if scheme == 2:
                diff["fell_back_to_scheme1"] = s2_fallback
            summaries[scheme].append(diff)

        d1 = summaries[1][-1]
        d2 = summaries[2][-1]
        # Cross-scheme sanity: optimal makespan <= fixed-topo makespan <= measured.
        assert d1["baseline_makespan_us"] <= d1["measured_makespan_us"] + 1e-3, rel
        assert d2["baseline_makespan_us"] <= d1["baseline_makespan_us"] + 1e-3, rel
        print(f"  {rel}\n"
              f"      measured={d1['measured_makespan_us']:.1f}us  "
              f"scheme1={d1['baseline_makespan_us']:.1f}us (+{d1['total_diff_us']:.1f}, "
              f"{d1['speedup_pct']:.1f}%)  "
              f"scheme2={d2['baseline_makespan_us']:.1f}us (+{d2['total_diff_us']:.1f}, "
              f"{d2['speedup_pct']:.1f}%)", flush=True)

    for scheme in (1, 2):
        rep = SWIM_BASE / f"scheme{scheme}_diff_analysis.json"
        rep.parent.mkdir(parents=True, exist_ok=True)
        # Strip per_task from the flat list header but keep it in each entry.
        rep.write_text(json.dumps(summaries[scheme], indent=2))
        print(f"Wrote {rep.relative_to(ROOT)}")

    write_summary_md(summaries)
    return 0


def write_summary_md(summaries: dict) -> None:
    lines = ["# proxy_baseline 实测-基线耗时差值汇总", "",
             "单位 us。scheme1 = 固定拓扑零调度间隔；scheme2 = 依赖约束最优拓扑。",
             "`Δ` = 实测 makespan − 基线 makespan（实测相对基线的延迟）。", "",
             "| config | measured | scheme1 | Δ1 | Δ1% | scheme2 | Δ2 | Δ2% |",
             "|---|--:|--:|--:|--:|--:|--:|--:|"]
    by_cfg = {d["config"]: d for d in summaries[1]}
    by_cfg2 = {d["config"]: d for d in summaries[2]}
    for cfg in sorted(by_cfg):
        d1, d2 = by_cfg[cfg], by_cfg2[cfg]
        lines.append(
            f"| {cfg} | {d1['measured_makespan_us']:.1f} | {d1['baseline_makespan_us']:.1f} "
            f"| {d1['total_diff_us']:.1f} | {d1['speedup_pct']:.1f}% "
            f"| {d2['baseline_makespan_us']:.1f} | {d2['total_diff_us']:.1f} "
            f"| {d2['speedup_pct']:.1f}% |")
    out = SWIM_BASE / "DIFF_SUMMARY.md"
    out.write_text("\n".join(lines) + "\n")
    print(f"Wrote {out.relative_to(ROOT)}")


if __name__ == "__main__":
    raise SystemExit(main())
