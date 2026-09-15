#!/usr/bin/env python3
"""Turn scripts/run_suite.py's results.csv into a pass/fail CI signal.

run_suite.py always exits 0 -- it's built for interactive/resumable local use,
where printing failures and letting you inspect/resume is the right behavior.
CI needs a hard gate instead, so this script re-reads the same CSV and fails
the build if any scheduled configuration errored, or if a config with
gates_correctness=True (manifest.py) didn't match its `original` baseline.

A config error (crash/exception) always fails the build regardless of which
config produced it -- that's a harness bug. A correctness mismatch only fails
the build for gating configs: a non-gating config like clang_o2 can
legitimately diverge from the -O0 baseline (e.g. FMA/vectorization changing
FP results on an FP-heavy benchmark) without that being a scheduler
regression, so it's reported but doesn't fail CI.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import CONFIGS

GATING_KEYS = {cfg["key"] for cfg in CONFIGS if cfg.get("gates_correctness", True)}


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ci_gate.py <results.csv>")
    path = sys.argv[1]

    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    if not rows:
        sys.exit(f"{path}: no rows -- every benchmark failed Stage A")

    errors = [r for r in rows if r.get("error")]
    mismatches = [r for r in rows if r.get("correctness_ok_vs_original") == "False"]
    fails = [r for r in mismatches if r["config"] in GATING_KEYS]
    informational = [r for r in mismatches if r["config"] not in GATING_KEYS]

    for r in errors:
        print(f"CONFIG ERROR: {r['benchmark']} / {r['config']}: {r['error'][:200]}")
    for r in fails:
        print(f"CORRECTNESS FAILURE: {r['benchmark']} / {r['config']}")
    for r in informational:
        print(f"non-gating divergence (not a CI failure): {r['benchmark']} / {r['config']}")

    if errors or fails:
        sys.exit(f"\n{len(errors)} config error(s), {len(fails)} correctness failure(s)")

    scheduled = [r for r in rows if r["config"] != "original" and r["config"] in GATING_KEYS]
    benchmarks = {r["benchmark"] for r in rows}
    print(f"OK: {len(scheduled)}/{len(scheduled)} gating configurations matched "
          f"their baseline across {len(benchmarks)} benchmarks ({len(rows)} rows total)")


if __name__ == "__main__":
    main()
