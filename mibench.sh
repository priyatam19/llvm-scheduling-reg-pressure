#!/bin/bash
# sha_benchmark.sh

MIBENCH=benchmarks/mibench-master

# Compile benchmark with clang instrumentation flag
clang -O0 -fprofile-instr-generate -I $MIBENCH/security/sha $MIBENCH/security/sha/sha.c $MIBENCH/security/sha/sha_driver.c -o build/sha_inst

# Run to generate profile data
LLVM_PROFILE_FILE="build/default.profraw" ./build/sha_inst $MIBENCH/security/sha/input_small.asc

# Convert to profile data
llvm-profdata merge build/default.profraw -o build/benchmark.profdata

# Compile the IR with profile data
clang -O0 -Xclang -disable-O0-optnone -fprofile-instr-use=build/benchmark.profdata --target=riscv64-unknown-linux-gnu -I $MIBENCH/security/sha $MIBENCH/security/sha/sha.c -emit-llvm -S -o build/sha_core.ll

clang -O0 -Xclang -disable-O0-optnone -fprofile-instr-use=build/benchmark.profdata --target=riscv64-unknown-linux-gnu -I $MIBENCH/security/sha $MIBENCH/security/sha/sha_driver.c -emit-llvm -S -o build/sha_driver.ll

llvm-link build/sha_core.ll build/sha_driver.ll -S -o build/sha_benchmark.ll


# Runs all the passes
#-------------------------------------------------------------------

#local
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,localSchedulerPass" build/sha_benchmark.ll -S -o build/sha_output_local.ll

#global
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,globalSchedulerPass" build/sha_benchmark.ll -S -o build/sha_output_global.ll

#global without reg pressure
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,globalSchedulerPassNoPressure" build/sha_benchmark.ll -S -o build/sha_output_global_no_pressure.ll

#Original code
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg" build/sha_benchmark.ll -S -o build/sha_original_benchmark.ll


# Evaluate the results
#-------------------------------------------------------------------

# Compile each IR to RISC-V assembly
llc -march=riscv64 -mcpu=rocket-rv64 build/sha_output_local.ll -o build/sha_output_local.s

llc -march=riscv64 -mcpu=rocket-rv64 build/sha_output_global.ll -o build/sha_output_global.s

llc -march=riscv64 -mcpu=rocket-rv64 build/sha_output_global_no_pressure.ll -o build/sha_output_global_no_pressure.s

llc -march=riscv64 -mcpu=rocket-rv64 build/sha_original_benchmark.ll -o build/sha_original_benchmark.s

# Run llvm-mca on all these RISC-V assembly code
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/sha_output_local.s > build/sha_mca_local.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/sha_output_global.s > build/sha_mca_global.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/sha_output_global_no_pressure.s > build/sha_mca_global_no_pressure.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/sha_original_benchmark.s > build/sha_original_benchmark.txt 2>&1


# Print results
#-------------------------------------------------------------------
print_stats() {
    local label=$1
    local mca_file=$2
    local ll_file=$3
    local asm_file=$4

    echo ""
    echo "=== $label ==="
    echo ""
    grep "^Instructions"      $mca_file
    grep "^Total Cycles"      $mca_file
    grep "^Total uOps"        $mca_file
    grep "^Dispatch Width"    $mca_file
    grep "^uOps Per Cycle"    $mca_file
    grep "^IPC"               $mca_file
    grep "^Block RThroughput" $mca_file
    echo ""
    echo "Register Spills:"
    spill_count=$(grep "Spill" $asm_file | wc -l)
    echo "$spill_count"
    echo ""
}

llc -march=riscv64 build/sha_output_local.ll -filetype=obj -o build/sha_output_local.o
llc -march=riscv64 build/sha_output_global.ll -filetype=obj -o build/sha_output_global.o
llc -march=riscv64 build/sha_output_global_no_pressure.ll -filetype=obj -o build/sha_output_global_no_pressure.o
llc -march=riscv64 build/sha_original_benchmark.ll -filetype=obj -o build/sha_original_benchmark.o

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/sha_output_local.o -o build/sha_run_local_mb

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/sha_output_global.o -o build/sha_run_global_mb

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/sha_output_global_no_pressure.o -o build/sha_run_global_no_pressure_mb

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/sha_original_benchmark.o -o build/sha_original_benchmark_mb


echo ""
echo "=== These following outputs are from llvm-mca ==="
echo "=== Please run gem5_benchmark.sh to get results from simulation ==="
echo ""

print_stats "Local Scheduler (Baseline)" build/sha_mca_local.txt build/sha_output_local.ll build/sha_output_local.s
print_stats "Global Scheduler (with Register Pressure)" build/sha_mca_global.txt build/sha_output_global.ll build/sha_output_global.s
print_stats "Global Scheduler (without Register Pressure)" build/sha_mca_global_no_pressure.txt build/sha_output_global_no_pressure.ll build/sha_output_global_no_pressure.s