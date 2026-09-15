# Benchmark + pass-config manifest shared by bench_worker.py, gem5_worker.py, run_suite.py.
# Paths are relative to the repo root (mounted at /work in both containers).

BENCHMARKS = [
    {
        "name": "synthetic_cold",
        "category": "synthetic",
        "dir": "benchmarks",
        "sources": ["benchmark.c"],
        "libs": [],
        "run_args": [],
        "stdin": None,
        "output_file": None,
        "std": "gnu99",
    },
    {
        "name": "synthetic_hot",
        "category": "synthetic",
        "dir": "benchmarks",
        "sources": ["benchmark_hot.c"],
        "libs": [],
        "run_args": [],
        "stdin": None,
        "output_file": None,
        "std": "gnu99",
    },
    {
        "name": "basicmath",
        "category": "automotive",
        "dir": "benchmarks/mibench-master/automotive/basicmath",
        "sources": ["basicmath_small.c", "rad2deg.c", "cubic.c", "isqrt.c"],
        "libs": ["-lm"],
        "run_args": [],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "bitcount",
        "category": "automotive",
        "dir": "benchmarks/mibench-master/automotive/bitcount",
        "sources": [
            "bitcnt_1.c", "bitcnt_2.c", "bitcnt_3.c", "bitcnt_4.c",
            "bitcnts.c", "bitfiles.c", "bitstrng.c", "bstr_i.c",
        ],
        "libs": [],
        "run_args": ["75000"],
        "stdin": None,
        "output_file": None,
        # Timing values and the derived Best/Worst labels vary between runs;
        # correctness is determined by the per-algorithm bit counts.
        "stdout_normalization": "bitcount_timing",
    },
    {
        "name": "qsort",
        "category": "automotive",
        "dir": "benchmarks/mibench-master/automotive/qsort",
        "sources": ["qsort_small.c"],
        "libs": ["-lm"],
        "run_args": ["input_small.dat"],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "susan",
        "category": "automotive",
        "dir": "benchmarks/mibench-master/automotive/susan",
        "sources": ["susan.c"],
        "libs": ["-lm"],
        "run_args": ["input_small.pgm", "{outfile}", "-s"],
        "stdin": None,
        "output_file": "{outfile}",
    },
    {
        "name": "dijkstra",
        "category": "network",
        "dir": "benchmarks/mibench-master/network/dijkstra",
        "sources": ["dijkstra_small.c"],
        "libs": [],
        "run_args": ["input.dat"],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "patricia",
        "category": "network",
        "dir": "benchmarks/mibench-master/network/patricia",
        "sources": ["patricia.c", "patricia_test.c"],
        "libs": [],
        "run_args": ["small.udp"],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "sha",
        "category": "security",
        "dir": "benchmarks/mibench-master/security/sha",
        "sources": ["sha.c", "sha_driver.c"],
        "libs": [],
        "run_args": ["input_small.asc"],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "blowfish",
        "category": "security",
        "dir": "benchmarks/mibench-master/security/blowfish",
        "sources": [
            "bf.c", "bf_skey.c", "bf_ecb.c", "bf_enc.c",
            "bf_cbc.c", "bf_cfb64.c", "bf_ofb64.c",
        ],
        "libs": [],
        "run_args": ["e", "input_small.asc", "{outfile}", "1234567890abcdeffedcba0987654321"],
        "stdin": None,
        "output_file": "{outfile}",
    },
    {
        "name": "rijndael",
        "category": "security",
        "dir": "benchmarks/mibench-master/security/rijndael",
        "sources": ["aes.c", "aesxam.c"],
        "libs": [],
        "run_args": [
            "input_small.asc", "{outfile}", "e",
            "1234567890abcdeffedcba09876543211234567890abcdeffedcba0987654321",
        ],
        "stdin": None,
        "output_file": "{outfile}",
    },
    {
        "name": "CRC32",
        "category": "telecomm",
        "dir": "benchmarks/mibench-master/telecomm/CRC32",
        "sources": ["crc_32.c"],
        "libs": [],
        "run_args": ["../adpcm/data/large.pcm"],
        "stdin": None,
        "output_file": None,
    },
    {
        "name": "adpcm",
        "category": "telecomm",
        "dir": "benchmarks/mibench-master/telecomm/adpcm/src",
        "sources": ["rawcaudio.c", "adpcm.c"],
        "libs": [],
        "run_args": [],
        "stdin": "../data/small.pcm",
        "output_file": None,
    },
    {
        "name": "stringsearch",
        "category": "office",
        "dir": "benchmarks/mibench-master/office/stringsearch",
        "sources": ["bmhasrch.c", "bmhisrch.c", "bmhsrch.c", "pbmsrch_small.c"],
        "libs": [],
        "run_args": [],
        "stdin": None,
        "output_file": None,
    },
]

# opt -passes= string per configuration, and whether SCHED_STATS instrumentation applies
# (only the two global configs go through trace identification / cross-block hoisting).
#
# clang_o2 has passes=None and is handled specially by bench_worker.py: instead of
# mem2reg + our plugin on the shared -O0 profiled IR, it's clang's own full -O2
# pipeline (inlining/GVN/LICM/its own instruction scheduling/greedy regalloc), fed
# the *same* profile data as every other config -- the "what would a user actually
# get off the shelf" comparison point this project previously had no data for.
# Included in CONFIGS (not a separate list) so it's automatically covered by the
# existing exit-code/stdout/output-hash correctness gate in run_suite.py, the same
# way every scheduled config already is.
CONFIGS = [
    {"key": "original", "passes": "mem2reg", "sched_stats": False},
    {"key": "local", "passes": "mem2reg,localSchedulerPass", "sched_stats": False},
    {"key": "global", "passes": "mem2reg,globalSchedulerPass", "sched_stats": True},
    {"key": "global_no_pressure", "passes": "mem2reg,globalSchedulerPassNoPressure", "sched_stats": True},
    {"key": "clang_o2", "passes": None, "sched_stats": False},
]

# clang flags needed to cross-compile/statically link for riscv64 with this toolchain image
# (base sched-bench-llvm21 images are missing a riscv64 sysroot/lld deps by default; this
# repo's Dockerfile at scripts/docker/Dockerfile.llvm21 installs gcc-riscv64-linux-gnu /
# libc6-dev-riscv64-cross / qemu-user / libxml2 to fix that).
RISCV_LINK_FLAGS = [
    "--target=riscv64-unknown-linux-gnu",
    "--sysroot=/usr/riscv64-linux-gnu",
    "-B/usr/lib/gcc-cross/riscv64-linux-gnu/11",
    "-L/usr/lib/gcc-cross/riscv64-linux-gnu/11",
    "-fuse-ld=lld",
    "-static",
]

# Legacy K&R-style MiBench sources need gnu89 dialect (modern clang treats implicit-int /
# implicit-function-declaration as hard errors by default); the synthetic benchmarks use
# modern C99 for-loop scoping so they need gnu99 instead (set via each benchmark's "std").
# -disable-O0-optnone is needed for all of them so our opt passes can run on -O0 IR.
def cc_compat_flags(bench):
    return [f"-std={bench.get('std', 'gnu89')}", "-Xclang", "-disable-O0-optnone"]

MCPU = "rocket-rv64"
