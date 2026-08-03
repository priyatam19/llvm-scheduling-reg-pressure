#!/usr/bin/env python3
"""Stage B worker: runs INSIDE the rseac/gem5:v24-0 container (repo mounted at /work,
container -w set to the benchmark's source directory so relative input filenames in
run_args resolve the same way they did for the qemu correctness run in Stage A).

Simulates each of the 4 riscv64 binaries built by bench_worker.py (build/results/<name>/
run_<key>) with a single, fixed in-order CPU model (MinorCPU + L1/L2 caches) so all
benchmarks/configs are comparable -- this also fixes the inconsistency in the original
repo's gem5_benchmark.sh (MinorCPU+caches) vs gem5_mibench.sh (AtomicSimpleCPU, no caches).
"""
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import BENCHMARKS, CONFIGS

REPO = "/work"
GEM5 = "/gem5/build/RISCV/gem5.opt"
GEM5_CONFIG = "/gem5/configs/deprecated/example/se.py"
CPU_OPTS = ["--cpu-type=MinorCPU", "--caches", "--l2cache"]


def resolve_args(args, outfile):
    return [a.replace("{outfile}", outfile) if isinstance(a, str) else a for a in args]


def parse_stats(path):
    if not os.path.exists(path):
        return {"numInsts": None, "numCycles": None, "ipc": None, "simSeconds": None}
    text = open(path).read()

    def grab(pattern):
        m = re.search(pattern, text)
        return float(m.group(1)) if m else None

    insts = None
    m = re.search(r"^\S*numInsts\s+(\d+)", text, re.MULTILINE)
    if m:
        insts = float(m.group(1))
    return {
        "numInsts": insts,
        "numCycles": grab(r"system\.cpu\.numCycles\s+([0-9.]+)"),
        "ipc": grab(r"system\.cpu\.ipc\s+([0-9.]+)"),
        "simSeconds": grab(r"simSeconds\s+([0-9.]+)"),
    }


def run_one(bench, out_root):
    name = bench["name"]
    build_dir = os.path.join(out_root, name)
    workdir = os.path.join(REPO, bench["dir"])

    def simulate(cfg):
        key = cfg["key"]
        run_bin = os.path.join(build_dir, f"run_{key}")
        if not os.path.exists(run_bin):
            return key, {"error": "binary missing, run Stage A first"}

        outfile = os.path.join(build_dir, f"gem5_outfile_{key}.bin")
        args = resolve_args(bench["run_args"], outfile)
        outdir = os.path.join(build_dir, f"m5out_{key}")
        os.makedirs(outdir, exist_ok=True)

        cmd = [GEM5, f"--outdir={outdir}", GEM5_CONFIG] + CPU_OPTS + ["-c", run_bin]
        if args:
            cmd += ["-o", " ".join(args)]
        if bench["stdin"]:
            cmd += ["-i", os.path.join(workdir, bench["stdin"])]

        proc = subprocess.run(cmd, cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stats = parse_stats(os.path.join(outdir, "stats.txt"))
        stats["gem5_returncode"] = proc.returncode
        return key, stats

    # Configurations use separate binaries and output directories, so they can
    # be simulated independently without changing the measured CPU model.
    with ThreadPoolExecutor(max_workers=len(CONFIGS)) as pool:
        return dict(pool.map(simulate, CONFIGS))


def main():
    name = sys.argv[1]
    out_root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build", "results")
    bench = next(b for b in BENCHMARKS if b["name"] == name)

    results = run_one(bench, out_root)

    out_path = os.path.join(out_root, name, "stageB.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
