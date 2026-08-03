#!/usr/bin/env python3
"""Read build/results/results.csv and produce:
  1. A per-benchmark speedup table (global+pressure vs local/original/no-pressure)
     on both llvm-mca and gem5 cycle counts.
  2. A feature table (trace length, mem-op fraction proxy via hoist candidates,
     pressure headroom, etc.) alongside the speedup, sorted so wins/regressions
     are easy to eyeball and correlate by inspection (N~13, no formal stats needed).

Usage: python3 scripts/analyze.py [--csv build/results/results.csv]
"""
import argparse
import csv
import os


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "build", "results", "results.csv"))
    args = ap.parse_args()

    rows = load(args.csv)
    by_bench = {}
    for r in rows:
        by_bench.setdefault(r["benchmark"], {})[r["config"]] = r

    def is_valid(cfg_row):
        """original is trusted by construction; any other config must have compiled
        (no 'error') AND matched original's output/exit code to be trustworthy."""
        if not cfg_row:
            return False
        if cfg_row.get("config") == "original":
            return True
        if cfg_row.get("error"):
            return False
        return cfg_row.get("correctness_ok_vs_original") == "True"

    print(f"{'benchmark':<16} {'category':<10} {'status (L/G/GNP)':<22} "
          f"{'gem5_cyc_orig':>13} {'gem5_cyc_local':>14} {'gem5_cyc_global':>15} {'gem5_cyc_gnp':>12} "
          f"{'spd_g_vs_l':>10} {'spd_g_vs_o':>10} {'spd_g_vs_gnp':>12} "
          f"{'traces':>6} {'hoist_done':>10} {'blk_press':>9} {'blk_loop':>8} {'peak_pr':>7}")

    summary = []
    for name, cfgs in by_bench.items():
        o, l, g, gnp = cfgs.get("original", {}), cfgs.get("local", {}), cfgs.get("global", {}), cfgs.get("global_no_pressure", {})
        # Only trust cycle counts from configs that (a) compiled and (b) matched
        # original's output -- a "speedup" from a config computing the wrong answer
        # (or from a compiler crash) is not a real result.
        cyc_o = to_float(o.get("gem5_num_cycles")) if is_valid(o) else None
        cyc_l = to_float(l.get("gem5_num_cycles")) if is_valid(l) else None
        cyc_g = to_float(g.get("gem5_num_cycles")) if is_valid(g) else None
        cyc_gnp = to_float(gnp.get("gem5_num_cycles")) if is_valid(gnp) else None

        def ratio(a, b):
            return (a / b) if (a and b) else None

        spd_g_vs_l = ratio(cyc_l, cyc_g)      # >1 means global is faster than local
        spd_g_vs_o = ratio(cyc_o, cyc_g)      # >1 means global is faster than original
        spd_g_vs_gnp = ratio(cyc_gnp, cyc_g)  # >1 means pressure-awareness helped

        traces = to_float(g.get("trace_count"))
        hoist_done = to_float(g.get("hoist_done"))
        blk_press = to_float(g.get("blocked_pressure"))
        blk_loop = to_float(g.get("blocked_loop"))
        peak_pr = to_float(g.get("peak_live_pressure"))

        def status(cfg_row):
            if not cfg_row:
                return "?"
            if cfg_row.get("error"):
                return "CRASH"
            return "ok" if is_valid(cfg_row) else "WRONG"

        status_str = f"L={status(l)} G={status(g)} GNP={status(gnp)}"

        summary.append(dict(name=name, category=o.get("category"), cyc_o=cyc_o, cyc_l=cyc_l,
                             cyc_g=cyc_g, cyc_gnp=cyc_gnp, spd_g_vs_l=spd_g_vs_l,
                             spd_g_vs_o=spd_g_vs_o, spd_g_vs_gnp=spd_g_vs_gnp,
                             traces=traces, hoist_done=hoist_done, blk_press=blk_press,
                             blk_loop=blk_loop, peak_pr=peak_pr, status=status_str))

    summary.sort(key=lambda r: (r["spd_g_vs_l"] is None, -(r["spd_g_vs_l"] or 0)))

    def fmt(v, nd=3):
        return f"{v:.{nd}f}" if isinstance(v, float) else "n/a"

    for r in summary:
        print(f"{r['name']:<16} {str(r['category']):<10} {r['status']:<22} "
              f"{fmt(r['cyc_o'], 0):>13} {fmt(r['cyc_l'], 0):>14} {fmt(r['cyc_g'], 0):>15} {fmt(r['cyc_gnp'], 0):>12} "
              f"{fmt(r['spd_g_vs_l']):>10} {fmt(r['spd_g_vs_o']):>10} {fmt(r['spd_g_vs_gnp']):>12} "
              f"{fmt(r['traces'], 0):>6} {fmt(r['hoist_done'], 0):>10} {fmt(r['blk_press'], 0):>9} {fmt(r['blk_loop'], 0):>8} {fmt(r['peak_pr'], 0):>7}")

    wins = [r for r in summary if r["spd_g_vs_l"] and r["spd_g_vs_l"] > 1.02]
    regressions = [r for r in summary if r["spd_g_vs_l"] and r["spd_g_vs_l"] < 0.98]
    print(f"\nWins (global+pressure >2% faster than local, BOTH configs correctness-verified): {[r['name'] for r in wins]}")
    print(f"Regressions (global+pressure >2% slower than local, BOTH configs correctness-verified): {[r['name'] for r in regressions]}")
    no_valid_comparison = [r["name"] for r in summary if r["spd_g_vs_l"] is None]
    print(f"No valid comparison possible (global and/or local crashed or produced wrong output): {no_valid_comparison}")

    correctness_fails = [r for r in rows if r["correctness_ok_vs_original"] == "False"]
    if correctness_fails:
        print(f"\n!!! {len(correctness_fails)} correctness mismatches found: "
              f"{[(r['benchmark'], r['config']) for r in correctness_fails]}")


if __name__ == "__main__":
    main()
