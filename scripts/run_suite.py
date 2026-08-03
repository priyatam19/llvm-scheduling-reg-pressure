#!/usr/bin/env python3
"""Host-side orchestrator: for each benchmark, shells out to `docker run` for
Stage A (compile/schedule/llvm-mca, in sched-bench-llvm21) and Stage B (gem5
simulation, in rseac/gem5:v24-0), then merges both stages' JSON output into
build/results/results.csv.

Usage:
    python3 scripts/run_suite.py [--only NAME [NAME ...]] [--skip-gem5]
        [--out-root DIR] [--sched-trace-threshold PCT] [--sched-pressure-budget N]

The last two options set SCHED_TRACE_THRESHOLD_PCT / SCHED_PRESSURE_BUDGET for the
Stage A container (used by the hyperparameter sensitivity sweep) -- pass --out-root
to a scratch directory when sweeping so runs don't clobber the main results.csv.
"""
import argparse
import csv
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import BENCHMARKS, CONFIGS

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE_A_IMAGE = "sched-bench-llvm21"
STAGE_B_IMAGE = "rseac/gem5:v24-0"


def docker_run(image, workdir, args, env=None, extra_mounts=None):
    cmd = ["docker", "run", "--rm", "-v", f"{REPO}:/work", "-w", workdir]
    for k, v in (env or {}).items():
        cmd += ["-e", f"{k}={v}"]
    cmd += [image] + args
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode(errors="replace")


def run_stage_a(name, out_root_container, env):
    rc, out = docker_run(STAGE_A_IMAGE, "/work",
                          ["python3", "scripts/bench_worker.py", name, out_root_container],
                          env=env)
    return rc, out


def run_stage_b(name, out_root_container):
    rc, out = docker_run(STAGE_B_IMAGE, "/work",
                          ["python3", "scripts/gem5_worker.py", name, out_root_container])
    return rc, out


def load_json(path):
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def config_matches_original(stage_a, key):
    """Return True only for a completed scheduled config whose observable
    result exactly matches the baseline."""
    original = stage_a.get("original", {})
    candidate = stage_a.get(key, {})
    if not original or "error" in original or not candidate or "error" in candidate:
        return False
    return (
        candidate.get("exit_code") == original.get("exit_code")
        and candidate.get("stdout_sha256") == original.get("stdout_sha256")
        and candidate.get("output_sha256") == original.get("output_sha256")
    )


def stage_a_correct(stage_a):
    return bool(stage_a) and all(
        config_matches_original(stage_a, cfg["key"])
        for cfg in CONFIGS if cfg["key"] != "original"
    )


def build_csv_rows(bench_name, category, stage_a, stage_b):
    rows = []
    original = stage_a.get("original", {})
    for cfg in CONFIGS:
        key = cfg["key"]
        a = stage_a.get(key, {})
        b = stage_b.get(key, {})
        mca = a.get("mca", {})
        sched = a.get("sched_stats", {})

        correct = None
        if key != "original" and original and "error" not in a:
            correct = config_matches_original(stage_a, key)

        rows.append({
            "benchmark": bench_name,
            "category": category,
            "config": key,
            "error": a.get("error"),
            "mca_instructions": mca.get("instructions"),
            "mca_total_cycles": mca.get("total_cycles"),
            "mca_total_uops": mca.get("total_uops"),
            "mca_ipc": mca.get("ipc"),
            "mca_block_rthroughput": mca.get("block_rthroughput"),
            "spill_count": a.get("spill_count"),
            "gem5_num_insts": b.get("numInsts"),
            "gem5_num_cycles": b.get("numCycles"),
            "gem5_ipc": b.get("ipc"),
            "gem5_sim_seconds": b.get("simSeconds"),
            "trace_count": sched.get("trace_count"),
            "edges_above_threshold": sched.get("edges_above_threshold"),
            "edges_below_threshold": sched.get("edges_below_threshold"),
            "backedges_skipped": sched.get("backedges_skipped"),
            "hoist_candidates": sched.get("hoist_candidates"),
            "hoist_done": sched.get("hoist_done"),
            "blocked_dominance": sched.get("blocked_dominance"),
            "blocked_loop": sched.get("blocked_loop"),
            "blocked_operand": sched.get("blocked_operand"),
            "blocked_unsafe_speculation": sched.get("blocked_unsafe_speculation"),
            "blocked_pressure": sched.get("blocked_pressure"),
            "peak_live_pressure": sched.get("peak_live_pressure"),
            "exit_code": a.get("exit_code"),
            "correctness_ok_vs_original": correct,
        })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="+", default=None)
    ap.add_argument("--skip-gem5", action="store_true")
    ap.add_argument("--out-root", default="build/results",
                     help="repo-relative path (used both host- and container-side)")
    ap.add_argument("--sched-trace-threshold", type=int, default=None)
    ap.add_argument("--sched-pressure-budget", type=int, default=None)
    ap.add_argument("--resume", action="store_true",
                     help="skip a benchmark's stage if its JSON output already exists")
    args = ap.parse_args()

    benches = BENCHMARKS
    if args.only:
        benches = [b for b in BENCHMARKS if b["name"] in args.only]
        missing = set(args.only) - {b["name"] for b in benches}
        if missing:
            sys.exit(f"unknown benchmark name(s): {missing}")

    out_root_host = os.path.join(REPO, args.out_root)
    out_root_container = f"/work/{args.out_root}"
    os.makedirs(out_root_host, exist_ok=True)

    env = {}
    if args.sched_trace_threshold is not None:
        env["SCHED_TRACE_THRESHOLD_PCT"] = str(args.sched_trace_threshold)
    if args.sched_pressure_budget is not None:
        env["SCHED_PRESSURE_BUDGET"] = str(args.sched_pressure_budget)

    csv_path = os.path.join(out_root_host, "results.csv")
    csv_fieldnames = None
    csv_rows = {}
    # Fresh run (no --resume): start the CSV clean instead of appending to a stale one.
    if not args.resume and os.path.exists(csv_path):
        os.remove(csv_path)
    elif os.path.exists(csv_path):
        with open(csv_path) as f:
            existing = list(csv.DictReader(f))
        csv_fieldnames = list(existing[0].keys()) if existing else None
        csv_rows = {(r["benchmark"], r["config"]): r for r in existing}
        # Keep --resume compatible with result files created before the new
        # legality diagnostic was added.
        if csv_fieldnames and "blocked_unsafe_speculation" not in csv_fieldnames:
            insert_at = csv_fieldnames.index("blocked_pressure")
            csv_fieldnames.insert(insert_at, "blocked_unsafe_speculation")

    for bench in benches:
        name = bench["name"]
        bench_dir = os.path.join(out_root_host, name)
        stage_a_path = os.path.join(bench_dir, "stageA.json")
        stage_b_path = os.path.join(bench_dir, "stageB.json")

        if args.resume and os.path.exists(stage_a_path):
            print(f"=== {name}: Stage A already done, --resume skipping ===", flush=True)
        else:
            print(f"=== {name}: Stage A (compile/schedule/llvm-mca) ===", flush=True)
            rc, out = run_stage_a(name, out_root_container, env)
            if rc != 0:
                print(out)
                print(f"!!! Stage A FAILED for {name} (rc={rc}) -- skipping Stage B / CSV row")
                continue

        if not args.skip_gem5:
            stage_a_now = load_json(stage_a_path)
            if not stage_a_correct(stage_a_now):
                print(f"!!! {name}: Stage A correctness gate failed -- skipping gem5")
            elif args.resume and os.path.exists(stage_b_path):
                print(f"=== {name}: Stage B already done, --resume skipping ===", flush=True)
            else:
                print(f"=== {name}: Stage B (gem5 simulation) ===", flush=True)
                rc, out = run_stage_b(name, out_root_container)
                if rc != 0:
                    print(out)
                    print(f"!!! Stage B FAILED for {name} (rc={rc}) -- CSV row will have no gem5 data")

        stage_a = load_json(stage_a_path)
        # Never surface stale or newly generated performance data for a
        # correctness-failed benchmark.
        stage_b = (load_json(stage_b_path)
                   if not args.skip_gem5 and stage_a_correct(stage_a) else {})
        rows = build_csv_rows(name, bench["category"], stage_a, stage_b)

        # Upsert and rewrite incrementally.  This preserves resumability while
        # preventing duplicate CSV rows and lets a later gem5 run fill in the
        # previously blank performance columns.
        if rows:
            if csv_fieldnames is None:
                csv_fieldnames = list(rows[0].keys())
            for row in rows:
                csv_rows[(row["benchmark"], row["config"])] = row
            with open(csv_path, "w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=csv_fieldnames)
                writer.writeheader()
                writer.writerows(csv_rows.values())

    if csv_rows:
        print(f"\n{csv_path} has {len(csv_rows)} total rows")
    else:
        print("\nNo rows produced -- all benchmarks failed Stage A.")

    final_rows = list(csv.DictReader(open(csv_path))) if os.path.exists(csv_path) else []
    fails = [r for r in final_rows if r["correctness_ok_vs_original"] == "False"]
    errors = [r for r in final_rows if r.get("error")]
    if errors:
        print(f"\n!!! CONFIG ERRORS ({len(errors)}, e.g. pass/verifier crash -- see error column):")
        for r in errors:
            print(f"  {r['benchmark']} / {r['config']}: {r['error'][:120]}")
    if fails:
        print(f"\n!!! CORRECTNESS FAILURES ({len(fails)}):")
        for r in fails:
            print(f"  {r['benchmark']} / {r['config']}")
    else:
        print("\nAll scheduled variants matched original's output/exit code.")


if __name__ == "__main__":
    main()
