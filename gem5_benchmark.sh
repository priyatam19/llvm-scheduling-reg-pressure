# Run this after running benchmark.sh
# That file creates all three executables
# This file runs all these executables in Gem5 RISC simulator
# Prints all the cycle, instruction count details
# You need to have Gem5 in your container
# Else pull the container with Gem5, attach this same directory and then run this script.
# Steps mentioned in Readme file

#!/bin/bash

GEM5=/gem5/build/RISCV/gem5.opt
CONFIG=/gem5/configs/deprecated/example/se.py
OUTDIR=/work/build/m5out
CPU_OPTS="--cpu-type=MinorCPU --caches --l2cache"

BINARIES=("build/original_benchmark" "build/run_local" "build/run_global" "build/run_global_no_pressure")


for bin in "${BINARIES[@]}"; do
    echo ""
    echo "Running: $bin"

    # run gem5
    $GEM5 --outdir=$OUTDIR $CONFIG $CPU_OPTS -c /work/$bin > /dev/null 2>&1

    STATS="$OUTDIR/stats.txt"

    insts=$(awk '/numInsts/ && $2 ~ /^[0-9]+$/ {print $2; exit}' "$STATS")
    cycles=$(awk '/system\.cpu\.numCycles/ {print $2}' $STATS)
    ipc=$(awk '/system\.cpu\.ipc/ {print $2}' $STATS)
    sim_time=$(awk '/simSeconds/ {print $2}' $STATS)

    echo "Cycles (Number of cpu cycles simulated): $cycles"
    echo "Instructions (Number of instructions committed (thread level)): $insts"
    echo "IPC (IPC: instructions per cycle (core level)): $ipc"
    echo "SimSeconds (Number of seconds simulated): $sim_time"
done