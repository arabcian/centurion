#!/usr/bin/env python3
"""Compare matched baseline/candidate autotune timing reports (stdlib only).

Inputs: JSONL, one autotune response per line; collect independently using the
same goal, profile, calibration signature and weights, with 10+ repeats each.
Does not claim scientific significance or turn trial data into a benchmark.
"""
import argparse
import json
import math
from pathlib import Path
import statistics

STAGES = ("inputs", "model", "decision", "total")


def read_timings(path):
    rows = []
    for n, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        obj = json.loads(line)
        timings = obj.get("timings_ms")
        if not isinstance(timings, dict):
            raise ValueError(f"{path}:{n}: missing timings_ms")
        if not all(isinstance(timings.get(k), (int, float)) and
                   math.isfinite(timings[k]) and timings[k] >= 0
                   for k in STAGES):
            raise ValueError(f"{path}:{n}: missing/invalid timing fields")
        if obj.get("ok") is False:
            raise ValueError(f"{path}:{n}: failed autotune response")
        rows.append(obj)
    if not rows:
        raise ValueError(f"{path}: empty timing file")
    return rows


def percentile(xs, p):
    xs = sorted(xs)
    pos = (len(xs) - 1) * p / 100.0
    lo, hi = math.floor(pos), math.ceil(pos)
    return xs[lo] * (hi - pos) + xs[hi] * (pos - lo) if hi != lo else float(xs[lo])


def summarize(baseline, candidate):
    for name, rows in (("baseline", baseline), ("candidate", candidate)):
        contexts = {(r.get("goal"), r.get("explore_mode")) for r in rows}
        if len(contexts) > 1:
            raise ValueError(f"{name}: mixed goal/modes; compare the same workload in one set")
    bc = baseline[0].get("goal"), baseline[0].get("explore_mode")
    cc = candidate[0].get("goal"), candidate[0].get("explore_mode")
    # Different exploration modes are intentional only when explicitly comparing
    # fast-first versus old full exploration, so include them in the result.
    if bc[0] != cc[0]:
        raise ValueError(f"different goals: {bc[0]!r} vs {cc[0]!r}")
    metrics = {}
    for key in STAGES:
        a = [r["timings_ms"][key] for r in baseline]
        b = [r["timings_ms"][key] for r in candidate]
        ap50, bp50 = statistics.median(a), statistics.median(b)
        metrics[key] = {
            "baseline_p50_ms": ap50, "baseline_p95_ms": percentile(a, 95),
            "candidate_p50_ms": bp50, "candidate_p95_ms": percentile(b, 95),
            "p50_speedup": round(ap50 / bp50, 3) if bp50 else None,
        }
    return {
        "goal": bc[0], "baseline_mode": bc[1], "candidate_mode": cc[1],
        "baseline_runs": len(baseline), "candidate_runs": len(candidate),
        "warning": "Same machine/state/signature/weights required; no significance or quality equivalence inferred.",
        "stages": metrics,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("baseline", type=Path, help="JSONL from baseline build")
    p.add_argument("candidate", type=Path, help="JSONL from candidate build")
    a = p.parse_args()
    print(json.dumps(summarize(read_timings(a.baseline), read_timings(a.candidate)), indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
