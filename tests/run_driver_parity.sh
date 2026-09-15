#!/usr/bin/env bash
# Phase 1 of the register-allocator scope expansion: prove
# build/schedRegAllocDriver (a small llc-equivalent, built because llc has
# no runtime plugin mechanism for out-of-tree register allocators -- see
# src/schedRegAllocDriver.cpp) is a faithful stand-in for stock llc BEFORE
# any new allocator code exists, so later bugs are attributable to the
# allocator, not the harness. Only exercises -regalloc=greedy/fast here --
# -regalloc=chaitin is covered once it exists, by tests/run_regalloc_tests.sh.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
result_dir=$(mktemp -d)
trap 'rm -rf "$result_dir"' EXIT

driver="$repo_dir/build/schedRegAllocDriver"
plugin="$repo_dir/build/schedulerPass.so"
MCPU=rocket-rv64

RISCV_LINK_FLAGS=(
  --target=riscv64-unknown-linux-gnu
  --sysroot=/usr/riscv64-linux-gnu
  -B/usr/lib/gcc-cross/riscv64-linux-gnu/11
  -L/usr/lib/gcc-cross/riscv64-linux-gnu/11
  -fuse-ld=lld
  -static
)

echo "=== compiling benchmarks/benchmark.c to riscv64 IR ==="
clang -O0 -Xclang -disable-O0-optnone --target=riscv64-unknown-linux-gnu \
  -emit-llvm -S "$repo_dir/benchmarks/benchmark.c" -o "$result_dir/benchmark.ll"

echo "=== scheduling it (mem2reg,globalSchedulerPass) ==="
opt -load-pass-plugin "$plugin" -passes="mem2reg,globalSchedulerPass" \
  "$result_dir/benchmark.ll" -S -o "$result_dir/scheduled.ll" 2>/dev/null

fail=0
for alloc in greedy fast; do
  for ft_ext in "asm:s" "obj:o"; do
    ft=${ft_ext%%:*}; ext=${ft_ext##*:}
    "$driver" -mcpu="$MCPU" -regalloc="$alloc" -filetype="$ft" -verify-machineinstrs \
      "$result_dir/scheduled.ll" -o "$result_dir/driver_${alloc}.${ext}"
    llc -march=riscv64 -mcpu="$MCPU" -regalloc="$alloc" -filetype="$ft" -verify-machineinstrs \
      "$result_dir/scheduled.ll" -o "$result_dir/llc_${alloc}.${ext}"
    if cmp -s "$result_dir/driver_${alloc}.${ext}" "$result_dir/llc_${alloc}.${ext}"; then
      echo "PASS: $alloc/$ft byte-identical to stock llc"
    else
      echo "FAIL: $alloc/$ft differs from stock llc"
      fail=1
    fi
  done
done

echo "=== execution parity under qemu-riscv64 (regalloc=greedy) ==="
clang "${RISCV_LINK_FLAGS[@]}" "$result_dir/driver_greedy.o" -o "$result_dir/run_driver"
clang "${RISCV_LINK_FLAGS[@]}" "$result_dir/llc_greedy.o" -o "$result_dir/run_llc"

set +e
qemu-riscv64 "$result_dir/run_driver" >"$result_dir/driver.stdout"; driver_exit=$?
qemu-riscv64 "$result_dir/run_llc" >"$result_dir/llc.stdout"; llc_exit=$?
set -e

if [[ "$driver_exit" == "$llc_exit" ]] && cmp -s "$result_dir/driver.stdout" "$result_dir/llc.stdout"; then
  echo "PASS: identical exit code ($driver_exit) and stdout under qemu-riscv64"
else
  echo "FAIL: exit driver=$driver_exit llc=$llc_exit, or stdout differs"
  fail=1
fi

if [[ "$fail" != 0 ]]; then
  echo "driver parity check FAILED"
  exit 1
fi
echo "driver parity check passed"
