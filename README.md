# LLVM Scheduling with Register-Pressure Awareness

This project implements three LLVM IR scheduling configurations and evaluates
them against an unscheduled `mem2reg` baseline:

- `localSchedulerPass`: reorders pure instructions inside basic blocks.
- `globalSchedulerPass`: adds profile-guided, cross-block scheduling with a
  register-pressure budget.
- `globalSchedulerPassNoPressure`: uses the same global scheduler without the
  pressure rejection check.

The scheduler is correctness-first. Calls, memory operations, allocas, EH
instructions, and other side effects are fixed scheduling anchors. Cross-block
hoists are accepted only when LLVM dominance, operand availability, loop, and
speculative-execution checks all succeed.

## Reproducible environment

The reference toolchain is LLVM 21 with a RISC-V sysroot and QEMU:

```bash
docker build -t sched-bench-llvm21 \
  -f scripts/docker/Dockerfile.llvm21 .
```

MiBench is downloaded separately and is intentionally not committed:

```bash
curl -L https://github.com/embecosm/mibench/archive/refs/heads/master.zip \
  -o /tmp/mibench.zip
unzip /tmp/mibench.zip -d /tmp
cp -r /tmp/mibench-master benchmarks/
```

Build the pass and run the focused legality regressions inside the LLVM image:

```bash
docker run --rm -v "$PWD":/work -w /work sched-bench-llvm21 make
docker run --rm -v "$PWD":/work -w /work sched-bench-llvm21 make test
```

## Benchmark suite

The reusable driver covers two synthetic programs and 12 MiBench programs:

| Category | Programs |
| --- | --- |
| Synthetic | `synthetic_cold`, `synthetic_hot` |
| Automotive | `basicmath`, `bitcount`, `qsort`, `susan` |
| Network | `dijkstra`, `patricia` |
| Security | `sha`, `blowfish`, `rijndael` |
| Telecomm | `CRC32`, `adpcm` |
| Office | `stringsearch` |

Each program is compiled through the baseline and all three scheduler
configurations, checked with `opt -verify-each`, analyzed with `llvm-mca`, run
under QEMU for output comparison, and then simulated with the same gem5
`MinorCPU` plus L1/L2 cache model. Stage results are written incrementally and
can be resumed without duplicating CSV rows.

Run the correctness and static-analysis stage first:

```bash
python3 scripts/run_suite.py \
  --skip-gem5 \
  --out-root build/correctness_full
```

Only after every scheduled binary matches its baseline, add gem5 results:

```bash
python3 scripts/run_suite.py \
  --resume \
  --out-root build/correctness_full

python3 scripts/analyze.py \
  --csv build/correctness_full/results.csv
```

The correctness comparison includes exit status, normalized stdout, and any
declared output file. Bitcount's elapsed-time text and timing-derived
`Best`/`Worst` labels are normalized, while every computed bit count remains in
the comparison.

## Correctness results

The initial broad run exposed two independent scheduler defects: calls and
loads could be reordered without modeling call memory effects, and a hot trace
was incorrectly treated as proof of dominance during cross-block hoisting. The
raw run reported only 8 of 42 scheduled configurations matching the baseline
and 14 verifier failures. It also revealed that bitcount's raw stdout hash was
nondeterministic because it prints measured execution times.

After adding scheduling anchors, LLVM dominance/speculation checks, focused IR
regressions, and the bitcount normalization:

- **42/42 scheduled configurations match their baselines.**
- **0 verifier failures** occur under `opt -verify-each`.
- **14/14 benchmarks** complete gem5 simulation in all four configurations.
- **56/56 result rows** contain gem5 cycle data with a zero simulator return
  code.

## Correctness-verified gem5 results

The following results use `build/correctness_full/results.csv`. Speedup is
`local cycles / global-with-pressure cycles`; values above 1 mean global
scheduling is faster.

| Benchmark | Local cycles | Global cycles | Speedup |
| --- | ---: | ---: | ---: |
| adpcm | 37,774,037 | 35,965,605 | 1.050 |
| sha | 12,517,311 | 12,459,127 | 1.005 |
| stringsearch | 358,873 | 357,897 | 1.003 |
| susan | 22,966,659 | 22,938,441 | 1.001 |
| blowfish | 53,309,541 | 53,284,097 | 1.000 |
| synthetic_cold | 31,077,536 | 31,077,536 | 1.000 |
| bitcount | 59,197,187 | 59,197,187 | 1.000 |
| qsort | 29,921,623 | 29,921,623 | 1.000 |
| patricia | 162,987,301 | 162,987,301 | 1.000 |
| CRC32 | 985,458,431 | 985,459,023 | 1.000 |
| dijkstra | 67,604,368 | 67,607,314 | 1.000 |
| rijndael | 38,399,373 | 38,456,797 | 0.999 |
| basicmath | 184,353,705 | 187,296,001 | 0.984 |
| synthetic_hot | 83,177,727 | 89,377,377 | 0.931 |

At the 2% threshold, `adpcm` is the only global-over-local win and
`synthetic_hot` is the only regression. Performance numbers from the earlier
incorrect schedules should not be used.

## Diagnostics and tuning

Set `SCHED_STATS=1` to report trace counts and lengths, profile edge decisions,
hoist candidates and rejection reasons, and peak live pressure. Two experimental
constants can be changed without rebuilding the pass:

```bash
SCHED_TRACE_THRESHOLD_PCT=60
SCHED_PRESSURE_BUDGET=12
```

The pressure budget is a profitability heuristic, not a correctness check.
Correctness is enforced independently by the anchor, dominance, operand, loop,
and speculation gates. Precise memory motion using AliasAnalysis or MemorySSA
is intentionally left for future work.
