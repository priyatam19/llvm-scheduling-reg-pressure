#!/usr/bin/env python3
"""Turn scripts/run_suite.py's results.csv into a pass/fail CI signal.

run_suite.py always exits 0 -- it's built for interactive/resumable local use,
where printing failures and letting you inspect/resume is the right behavior.
CI needs a hard gate instead, so this script re-reads the same CSV and fails
the build if any scheduled configuration errored or didn't match its
`original` baseline.
"""
import csv
import sys


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: ci_gate.py <results.csv>")
    path = sys.argv[1]

    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    if not rows:
        sys.exit(f"{path}: no rows -- every benchmark failed Stage A")

    errors = [r for r in rows if r.get("error")]
    fails = [r for r in rows if r.get("correctness_ok_vs_original") == "False"]

    for r in errors:
        print(f"CONFIG ERROR: {r['benchmark']} / {r['config']}: {r['error'][:200]}")
    for r in fails:
        print(f"CORRECTNESS FAILURE: {r['benchmark']} / {r['config']}")

    if errors or fails:
        sys.exit(f"\n{len(errors)} config error(s), {len(fails)} correctness failure(s)")

    scheduled = [r for r in rows if r["config"] != "original"]
    benchmarks = {r["benchmark"] for r in rows}
    print(f"OK: {len(scheduled)}/{len(scheduled)} scheduled configurations matched "
          f"their baseline across {len(benchmarks)} benchmarks ({len(rows)} rows total)")


if __name__ == "__main__":
    main()
