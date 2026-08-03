# benchmark.sh
#-------------------------------------------------------------------

# Compile benchmark with clang instrumentation flag
clang -O0 -fprofile-instr-generate benchmarks/benchmark.c -o build/benchmark_inst

# Run to generate profile data
LLVM_PROFILE_FILE="build/default.profraw" ./build/benchmark_inst

# Convert to profile data
llvm-profdata merge build/default.profraw -o build/benchmark.profdata

# Compile the IR with profile data
clang -O0 -Xclang -disable-O0-optnone -fprofile-instr-use=build/benchmark.profdata --target=riscv64-unknown-linux-gnu benchmarks/benchmark.c -emit-llvm -S -o build/benchmark.ll


# Runs all the passes
#-------------------------------------------------------------------

#local
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,localSchedulerPass" build/benchmark.ll -S -o build/output_local.ll

#global
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,globalSchedulerPass" build/benchmark.ll -S -o build/output_global.ll

#global without reg pressure
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg,globalSchedulerPassNoPressure" build/benchmark.ll -S -o build/output_global_no_pressure.ll

#Original code
opt -load-pass-plugin ./build/schedulerPass.so -passes="mem2reg" build/benchmark.ll -S -o build/original_benchmark.ll


# Evaluate the results
#-------------------------------------------------------------------

# -mcpu=rocket-rv64
# Compile each IR to RISC-V assembly
# Disable all the built-in compiler optimizations
llc -march=riscv64 -mcpu=rocket-rv64 \
    build/output_local.ll -o build/output_local.s

llc -march=riscv64 -mcpu=rocket-rv64 \
    build/output_global.ll -o build/output_global.s

llc -march=riscv64 -mcpu=rocket-rv64 \
    build/output_global_no_pressure.ll -o build/output_global_no_pressure.s

llc -march=riscv64 -mcpu=rocket-rv64 \
    build/original_benchmark.ll -o build/original_benchmark.s

# Run llvm-mca on all these RISC-V assembly code
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/output_local.s > build/mca_local.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/output_global.s > build/mca_global.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/output_global_no_pressure.s > build/mca_global_no_pressure.txt 2>&1
llvm-mca -march=riscv64 -mcpu=rocket-rv64 build/original_benchmark.s > build/original_benchmark.txt 2>&1


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

llc -march=riscv64 build/output_local.ll -filetype=obj -o build/output_local.o
llc -march=riscv64 build/output_global.ll -filetype=obj -o build/output_global.o
llc -march=riscv64 build/output_global_no_pressure.ll -filetype=obj -o build/output_global_no_pressure.o
llc -march=riscv64 build/original_benchmark.ll -filetype=obj -o build/original_benchmark.o


clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/output_local.o -o build/run_local

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/output_global.o -o build/run_global

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/output_global_no_pressure.o -o build/run_global_no_pressure

clang --target=riscv64-linux-gnu \
    -static -O0 \
    build/original_benchmark.o -o build/original_benchmark


# For correctness verification
echo "=== To verify correctness (baseline, local, global, global_no_pressure) ==="
./build/original_benchmark; echo "Exit: $?"
./build/run_local; echo "Exit: $?"
./build/run_global; echo "Exit: $?"
./build/run_global_no_pressure; echo "Exit: $?"
echo ""

echo ""
echo "=== These following outputs are from llvm-mca ==="
echo "=== Please run gem5_benchmark.sh to get results from simulation ==="
echo ""

print_stats "Orginal Code" build/original_benchmark.txt build/original_benchmark.ll build/original_benchmark.s
print_stats "Local Scheduler (Baseline)" build/mca_local.txt build/output_local.ll build/output_local.s
print_stats "Global Scheduler (with Register Pressure)" build/mca_global.txt build/output_global.ll build/output_global.s
print_stats "Global Scheduler (without Register Pressure)" build/mca_global_no_pressure.txt build/output_global_no_pressure.ll build/output_global_no_pressure.s
