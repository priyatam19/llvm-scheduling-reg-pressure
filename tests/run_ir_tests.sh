#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
plugin="$repo_dir/build/schedulerPass.so"
test_dir="$repo_dir/tests"
result_dir=$(mktemp -d)
trap 'rm -rf "$result_dir"' EXIT

configs=(
  "local:mem2reg,localSchedulerPass"
  "global:mem2reg,globalSchedulerPass"
  "global_no_pressure:mem2reg,globalSchedulerPassNoPressure"
)

for config in "${configs[@]}"; do
  name=${config%%:*}
  pipeline=${config#*:}

  SCHED_STATS=1 opt -verify-each -load-pass-plugin "$plugin" \
    -passes="$pipeline" "$test_dir/memory_anchors.ll" -S \
    -o "$result_dir/memory_anchors_$name.ll" \
    2>"$result_dir/memory_anchors_$name.stats"
  python3 "$test_dir/check_ir.py" memory "$result_dir/memory_anchors_$name.ll"
  lli "$result_dir/memory_anchors_$name.ll"

  SCHED_STATS=1 opt -verify-each -load-pass-plugin "$plugin" \
    -passes="$pipeline" "$test_dir/loop_phi_dominance.ll" -S \
    -o "$result_dir/loop_phi_dominance_$name.ll" \
    2>"$result_dir/loop_phi_dominance_$name.stats"
  python3 "$test_dir/check_ir.py" loop "$result_dir/loop_phi_dominance_$name.ll"
  if [[ "$name" != "local" ]]; then
    grep -Eq 'blocked_dominance=[1-9][0-9]*' \
      "$result_dir/loop_phi_dominance_$name.stats"
  fi
done

for config in "${configs[@]:1}"; do
  name=${config%%:*}
  pipeline=${config#*:}

  SCHED_STATS=1 opt -verify-each -load-pass-plugin "$plugin" \
    -passes="$pipeline" "$test_dir/global_hoist_legality.ll" -S \
    -o "$result_dir/global_hoist_legality_$name.ll" \
    2>"$result_dir/global_hoist_legality_$name.stats"
  python3 "$test_dir/check_ir.py" hoist "$result_dir/global_hoist_legality_$name.ll"
  grep -Eq 'hoist_done=[1-9][0-9]*' "$result_dir/global_hoist_legality_$name.stats"
  grep -Eq 'blocked_unsafe_speculation=[1-9][0-9]*' \
    "$result_dir/global_hoist_legality_$name.stats"
done

echo "IR scheduler regressions passed"
