#!/usr/bin/env python3
"""Stage A worker: runs INSIDE the sched-bench-llvm21 container (repo mounted at /work).

For one benchmark: PGO-instrument + profile natively, recompile to riscv64 IR with the
profile, run it through the 4 opt pass configs, llc+llvm-mca each, statically link a
riscv64 binary per config, run each under qemu-riscv64, and record llvm-mca stats +
SCHED_STATS diagnostic counters + correctness data (exit code / output hashes) to
build/results/<name>/stageA.json.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from manifest import BENCHMARKS, CONFIGS, RISCV_LINK_FLAGS, cc_compat_flags, MCPU

REPO = "/work"
PLUGIN = f"{REPO}/build/schedulerPass.so"


def sh(cmd, cwd=None, env=None, stdin_path=None, check=True):
    stdin_fh = open(stdin_path, "rb") if stdin_path else None
    try:
        proc = subprocess.run(
            cmd, cwd=cwd, env=env, stdin=stdin_fh,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    finally:
        if stdin_fh:
            stdin_fh.close()
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n"
            f"stdout: {proc.stdout.decode(errors='replace')}\n"
            f"stderr: {proc.stderr.decode(errors='replace')}"
        )
    return proc


def sha256(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def normalized_stdout(bench, data):
    """Remove explicitly declared nondeterministic benchmark presentation
    fields while retaining semantic result values."""
    mode = bench.get("stdout_normalization")
    if mode is None:
        return data
    if mode == "bitcount_timing":
        text = data.decode(errors="replace")
        text = re.sub(r"Time:\s*[0-9.]+\s*sec\.;", "Time: <elapsed> sec.;", text)
        text = re.sub(r"^(Best|Worst)\s*>.*(?:\n|$)", "", text,
                      flags=re.MULTILINE)
        return text.encode()
    raise ValueError(f"unknown stdout normalization mode: {mode}")


def parse_mca(text):
    out = {}
    for key in ["Instructions", "Total Cycles", "Total uOps", "Dispatch Width",
                "uOps Per Cycle", "IPC", "Block RThroughput"]:
        m = re.search(rf"^{re.escape(key)}:\s*([0-9.]+)", text, re.MULTILINE)
        out[key.lower().replace(" ", "_")] = float(m.group(1)) if m else None
    return out


def parse_sched_stats(text):
    """Parse `SCHED_STATS key=value key2=value2 ...` lines, summing across
    multiple lines (one per function/trace) for scalar counters."""
    totals = {}
    hist_lines = []
    for line in text.splitlines():
        if not line.startswith("SCHED_STATS"):
            continue
        for tok in line.split()[1:]:
            if "=" not in tok:
                continue
            k, v = tok.split("=", 1)
            if k.endswith("_hist"):
                hist_lines.append(f"{k}={v}")
                continue
            try:
                num = float(v)
            except ValueError:
                continue
            totals[k] = totals.get(k, 0) + num
    if hist_lines:
        totals["_hist_raw"] = hist_lines
    return totals


def resolve_args(args, outfile):
    return [a.replace("{outfile}", outfile) if isinstance(a, str) else a for a in args]


def run_one(bench, out_root, extra_env=None):
    name = bench["name"]
    workdir = os.path.join(REPO, bench["dir"])
    build_dir = os.path.join(out_root, name)
    os.makedirs(build_dir, exist_ok=True)

    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)

    sources = bench["sources"]
    libs = bench["libs"]
    compat_flags = cc_compat_flags(bench)

    # 1. PGO-instrument + run NATIVELY (no --target: this container's clang defaults to
    #    the host arch, so no qemu needed here; branch profile counts are arch-independent).
    prof_inst = os.path.join(build_dir, "prof_inst")
    sh(["clang", "-O0"] + compat_flags + ["-fprofile-instr-generate"]
       + sources + libs + ["-o", prof_inst], cwd=workdir)

    profraw = os.path.join(build_dir, "default.profraw")
    run_env = dict(env)
    run_env["LLVM_PROFILE_FILE"] = profraw
    run_args = resolve_args(bench["run_args"], os.path.join(build_dir, "prof_out"))
    stdin_path = None
    if bench["stdin"]:
        stdin_path = os.path.join(workdir, bench["stdin"])
    sh([prof_inst] + run_args, cwd=workdir, env=run_env, stdin_path=stdin_path, check=False)

    profdata = os.path.join(build_dir, "prof.profdata")
    sh(["llvm-profdata", "merge", profraw, "-o", profdata])

    # 2. Recompile each source to riscv64 IR using the collected profile: once
    #    at -O0 (shared input for mem2reg + our scheduler plugin, as before),
    #    and once at -O2 (clang's own full pipeline, for the clang_o2 config --
    #    same profile data, so the comparison isolates "our scheduler+driver"
    #    vs. "clang's standard pipeline", not "PGO helped one and not the other").
    ll_files = []
    ll_files_o2 = []
    for src in sources:
        ll_out = os.path.join(build_dir, os.path.basename(src) + ".ll")
        sh(["clang", "-O0"] + compat_flags
           + [f"-fprofile-instr-use={profdata}", "--target=riscv64-unknown-linux-gnu",
              "-emit-llvm", "-S", src, "-o", ll_out], cwd=workdir)
        ll_files.append(ll_out)

        ll_out_o2 = os.path.join(build_dir, os.path.basename(src) + ".O2.ll")
        sh(["clang", "-O2"] + compat_flags
           + [f"-fprofile-instr-use={profdata}", "--target=riscv64-unknown-linux-gnu",
              "-emit-llvm", "-S", src, "-o", ll_out_o2], cwd=workdir)
        ll_files_o2.append(ll_out_o2)

    linked_ll = os.path.join(build_dir, "linked.ll")
    linked_ll_o2 = os.path.join(build_dir, "linked.O2.ll")
    if len(ll_files) > 1:
        sh(["llvm-link"] + ll_files + ["-S", "-o", linked_ll])
        sh(["llvm-link"] + ll_files_o2 + ["-S", "-o", linked_ll_o2])
    else:
        sh(["cp", ll_files[0], linked_ll])
        sh(["cp", ll_files_o2[0], linked_ll_o2])

    results = {}
    for cfg in CONFIGS:
        key = cfg["key"]
        try:
            results[key] = run_config(bench, cfg, build_dir, workdir, libs, linked_ll,
                                       linked_ll_o2, stdin_path, env)
        except Exception as e:
            results[key] = {"error": str(e)}
            print(f"!!! config {key} failed: {e}", file=sys.stderr)

    return results


def run_config(bench, cfg, build_dir, workdir, libs, linked_ll, linked_ll_o2, stdin_path, env):
    key = cfg["key"]
    cfg_env = dict(env)
    if cfg["sched_stats"]:
        cfg_env["SCHED_STATS"] = "1"

    out_ll = os.path.join(build_dir, f"out_{key}.ll")
    stderr_path = os.path.join(build_dir, f"schedstats_{key}.txt")
    if key == "clang_o2":
        # No scheduler pass involved -- this is clang's own -O2 IR (already
        # fully optimized), just verified and copied through so the rest of
        # the pipeline below (llc/driver, llvm-mca, link, qemu) is identical
        # to every other config from this point on.
        opt_cmd = ["opt", "-verify-each", linked_ll_o2, "-S", "-o", out_ll]
    else:
        opt_cmd = ["opt"]
        if key != "original":
            opt_cmd.append("-verify-each")
        opt_cmd += ["-load-pass-plugin", PLUGIN, f"-passes={cfg['passes']}",
                    linked_ll, "-S", "-o", out_ll]
    proc = sh(opt_cmd, env=cfg_env)
    with open(stderr_path, "wb") as f:
        f.write(proc.stderr)

    out_s = os.path.join(build_dir, f"out_{key}.s")
    out_o = os.path.join(build_dir, f"out_{key}.o")
    sh(["llc", "-march=riscv64", f"-mcpu={MCPU}", out_ll, "-o", out_s])
    sh(["llc", "-march=riscv64", f"-mcpu={MCPU}", out_ll, "-filetype=obj", "-o", out_o])

    mca_txt = os.path.join(build_dir, f"mca_{key}.txt")
    proc = sh(["llvm-mca", "-march=riscv64", f"-mcpu={MCPU}", out_s], check=False)
    with open(mca_txt, "wb") as f:
        f.write(proc.stdout)

    run_bin = os.path.join(build_dir, f"run_{key}")
    sh(["clang"] + RISCV_LINK_FLAGS + [out_o] + libs + ["-o", run_bin])

    outfile_path = os.path.join(build_dir, f"outfile_{key}.bin")
    cfg_run_args = resolve_args(bench["run_args"], outfile_path)
    run_proc = sh(["qemu-riscv64", run_bin] + cfg_run_args, cwd=workdir,
                   stdin_path=stdin_path, check=False)

    with open(out_s) as f:
        spill_count = f.read().count("Spill")

    return {
        "mca": parse_mca(proc.stdout.decode(errors="replace")),
        "spill_count": spill_count,
        "sched_stats": parse_sched_stats(open(stderr_path, errors="replace").read()),
        "exit_code": run_proc.returncode,
        "stdout_sha256": hashlib.sha256(
            normalized_stdout(bench, run_proc.stdout)
        ).hexdigest(),
        "stdout_raw_sha256": hashlib.sha256(run_proc.stdout).hexdigest(),
        "output_sha256": sha256(outfile_path) if bench["output_file"] else None,
    }


def main():
    name = sys.argv[1]
    out_root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "build", "results")
    bench = next(b for b in BENCHMARKS if b["name"] == name)

    extra_env = {}
    for var in ("SCHED_TRACE_THRESHOLD_PCT", "SCHED_PRESSURE_BUDGET"):
        if var in os.environ:
            extra_env[var] = os.environ[var]

    results = run_one(bench, out_root, extra_env=extra_env)

    out_path = os.path.join(out_root, name, "stageA.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
