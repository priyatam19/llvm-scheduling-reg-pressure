# Global Instruction Scheduling with Register Pressure — Code Walkthrough

> **Purpose of this document.** The existing `report.pdf` explains the project at a
> conceptual level: *why* global scheduling matters, *what* traces / DDGs / critical
> paths are, and *how* the three configurations compare. This document is the
> companion to that report. It works at the **file and code level**: a map of the
> whole directory, then for every header the exact algorithm, data structures and
> control flow it implements, then a near line-by-line reading of the pass driver
> (`src/schedulerPass.cpp`), and finally what each build/run script actually does.
> Read `report.pdf` first for the big picture; read this to understand the source.

---

## 1. Directory tree map

```
global_instruction_scheduling_reg_pressure/
│
├── README.md                     # How to build & run (Docker, MiBench download, script order)
├── report.pdf                    # High-level design report (problem → approach → evaluation)
├── testResults.md                # Captured llvm-mca + gem5 output for benchmark & MiBench
├── Makefile                      # Builds the pass into build/schedulerPass.so
├── .gitignore
│
├── benchmarks/
│   └── benchmark.c               # Hand-crafted micro-benchmark (a hot if-branch to schedule)
│
├── include/                      # The entire scheduler — header-only implementation
│   ├── schedulerPass.hpp         # Umbrella header: all std + LLVM includes used project-wide
│   ├── reaching.hpp              # Reaching-definitions dataflow (built, but ultimately unused)
│   ├── liveness.hpp              # Backward liveness dataflow → drives register-pressure logic
│   ├── dom.hpp                   # Dominator sets + immediate dominators (safety for hoisting)
│   ├── traceIdentification.hpp   # Profile-guided hot-trace formation (BPI + BFI)
│   ├── ddg.hpp                   # Data Dependence Graph: node/graph structs, latency model, edges
│   ├── critical.hpp              # Critical-path length per DDG node (topo sort + reverse pass)
│   ├── listScheduler.hpp         # List scheduling: ready queue, pressure delta, pickBest
│   ├── applySchedule.hpp         # Physically rewrites IR: within-block + cross-block movement + SSA repair
│   └── localScheduler.hpp        # Single-block variant (no traces, no cross-block movement)
│
├── src/
│   └── schedulerPass.cpp         # Pass plugin: wires all headers together, registers 3 pass names
│
├── build/                        # Generated artifacts (the .so, .ll/.s/.o, mca txt, profile data)
│   ├── schedulerPass.so          # The compiled pass plugin loaded by `opt`
│   ├── benchmark.ll              # Profiled IR fed into the passes
│   ├── output_local.ll/.s/.o     # Local-scheduled outputs
│   ├── output_global*.ll/.s/.o   # Global (with/without pressure) outputs
│   ├── original_benchmark.*      # Baseline (mem2reg only) outputs
│   └── mca_*.txt                 # llvm-mca static analysis reports
│
├── benchmark.sh                  # Full pipeline for benchmark.c: profile → run passes → llc → mca → link
├── mibench.sh                    # Same pipeline for MiBench's SHA program
├── gem5_benchmark.sh             # Runs the benchmark binaries inside the gem5 RISC-V simulator
└── gem5_mibench.sh               # Runs the MiBench binaries inside gem5
```

### How the pieces fit together (data-flow at a glance)

```
                         benchmark.c
                             │  clang -fprofile-instr-generate + run + profdata
                             ▼
                        benchmark.ll  (SSA IR, with profile metadata)
                             │  opt -load-pass-plugin schedulerPass.so
                             ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                       schedulerPass.cpp                           │
   │                                                                   │
   │  computeLiveness ──► computeReaching(unused) ──► identifyTraces   │
   │        │                                             │            │
   │        │                                             ▼            │
   │        │                                         buildDDG         │
   │        │                                             │            │
   │        ▼                                             ▼            │
   │  (pressure model)                            computeCriticalPath  │
   │        │                                             │            │
   │        └──────────────┬──────────────────────────────┘            │
   │                       ▼                                           │
   │                  listSchedule  ──►  applySchedule (+ dom, LoopInfo)│
   └─────────────────────────────────────────────────────────────────┘
                             ▼
                    output_*.ll  ──► llc ──► .s/.o ──► llvm-mca + gem5
```

Everything except the plugin registration lives in headers. `schedulerPass.cpp`
is deliberately thin — it is the *orchestration layer* that calls the header
functions in the right order.

---

## 2. `benchmarks/benchmark.c` — the workload

The benchmark is not arbitrary. It is engineered to *create global scheduling
opportunities*, i.e. a control-flow shape where instructions in a later block can
legally be hoisted into an earlier block on the hot path.

- **Six global arrays `A..F` of size 64** plus a `volatile int sink`. `volatile`
  forces the final store to actually happen so the optimizer/backend cannot delete
  the whole computation as dead.
- **`benchmark(int cond)`** loads `A[0..5]`, sums them into `base`, then branches on
  `cond`:
  - The `if (cond)` (taken) branch does a *small* amount of work.
  - The `else` branch does a *long dependent chain* `r0 → r1 → … → r9` of
    alternating add/mul plus an independent `s0`. This long chain is the interesting
    material: it has high latency and lots of independent loads that can be moved up.
- `main()` initializes the arrays, then calls `benchmark(1)` **one million times** in
  a loop. Calling with the constant `1` makes the `if` branch the *hot* path — this
  is exactly what the profile run records, and what trace identification will latch
  onto. The million-iteration loop makes the hot block's frequency dominate so the
  scheduler has a clear seed.

The `testResults.md` "Complete Cold Path Scenario" is the same program profiled so
that the *other* branch is hot — used to demonstrate the downside of speculative
hoisting when the hoisted work is not actually needed.

---

## 3. `include/` — the scheduler, header by header

### 3.1 `schedulerPass.hpp` — the umbrella include

Pure plumbing, no logic. It exists so every other header can `#include
"schedulerPass.hpp"` and get a consistent set of dependencies without repeating
include lists.

- **Std headers:** `<algorithm>` (`std::find`, `std::reverse`), `<cstdint>`
  (`uint64_t` for frequencies), `<numeric>`, `<string>`, `<vector>`, `<set>`.
- **LLVM ADT:** `BitVector` (the workhorse set representation for all dataflow),
  `DenseMap` (fast pointer-keyed maps).
- **LLVM IR:** `BasicBlock`, `CFG` (`successors`/`predecessors` iterators),
  `Constants`, `Function`, `Instruction`, `Instructions` (the `isa<LoadInst>` etc.
  subclasses), `PassManager`, `PassBuilder`, `PassPlugin` (new pass-manager plugin
  API), `raw_ostream` (the `outs()` logging).
- **LLVM Analysis:** `BranchProbabilityInfo`, `BlockFrequencyInfo` (profile data for
  traces), `ValueTracking`, `LoopInfo` (don't-hoist-out-of-loops checks).
- **LLVM Transforms/Utils:** `BasicBlockUtils`, `Verifier` (sanity checking),
  `SSAUpdater` (repairs SSA after cross-block moves).

`#pragma once` guards it. Key idea: **`BitVector` is the central data structure** —
liveness, reaching, and dominators all represent their sets as fixed-width bit
vectors indexed by a "universe" of values or blocks.

---

### 3.2 `reaching.hpp` — reaching definitions (built but not used for scheduling)

**What it computes.** Classic forward "reaching definitions" dataflow: for each
block, which definitions (value-producing instructions) may reach the block's
entry/exit.

**Data structure.** `BlockStateReaching { BitVector in, out, gen, kill; }` — one per
block, stored in a `DenseMap<const BasicBlock*, BlockStateReaching>`.

**Universe.** A `std::vector<Instruction*>` of every non-void instruction. Each bit
position corresponds to one definition.

**Algorithm (iterative fixed-point, forward, *union* meet):**
1. Build the universe (skip void instructions — they produce no value).
2. BFS the CFG from the entry block to get a processing order.
3. Per block, compute `gen` (the definitions it creates) and `kill` (other
   definitions of the *same name* — a name-based approximation of "same variable").
4. Iterate until nothing changes:
   - `IN[B] = ⋃ OUT[P]` over predecessors (entry uses the empty set).
   - `OUT[B] = gen[B] ∪ (IN[B] − kill[B])` — implemented with `&= killCopy.flip()`
     for the set-minus, then `|= gen`.

**Why it's here but unused.** The header comment and the call site in
`schedulerPass.cpp` both say so: on SSA form (after `mem2reg`) every operand already
points directly at its single defining instruction, so *true* dependences can be read
straight off the operands — no reaching analysis needed. It was an early plan, kept
for completeness. `buildDDG` takes it as a parameter but never reads it.

---

### 3.3 `liveness.hpp` — backward liveness (this one *is* the pressure engine)

**What it computes.** For each block, the set of values that are *live* on entry
(`in`) and on exit (`out`). "Live" = will be used again before being redefined. This
is what the register-pressure heuristics rest on.

**Data structure.** `BlockStateLiveness { BitVector in, out, use, def; }` in a
`DenseMap<const BasicBlock*, BlockStateLiveness>`.

**Universe.** `std::vector<Value*>` = **function arguments first, then every non-void
instruction**. Note this includes arguments (reaching's universe did not) because
arguments are live values that occupy registers. This exact universe ordering is
reused verbatim by the schedulers, so bit indices line up across analyses.

**Algorithm (iterative fixed-point, backward, *union* meet):**
1. Per block, compute `use` and `def`. Walking top-to-bottom: an operand found in the
   universe is a `use` **only if not already defined earlier in the block**
   (`if (!bs.def.test(idx)) bs.use.set(idx)` — this captures "used before defined").
   The instruction itself sets its `def` bit.
2. Iterate over blocks **in reverse** until stable:
   - `OUT[B] = ⋃ IN[S]` over successors.
   - `IN[B] = use[B] ∪ (OUT[B] − def[B])` (the `prop.reset(def)` does the set-minus).

**How it's consumed.** Register pressure at a point ≈ number of live values. The
schedulers use `liveness[BB].out` to decide whether an operand "dies" at an
instruction (not live out of the block ⇒ its register is freed). This is a
*block-granularity approximation* — liveness is not tracked per-instruction — which
the report explicitly acknowledges as good enough for a tiebreaker.

---

### 3.4 `dom.hpp` — dominators and immediate dominators

**What it computes.** For each block, the set of blocks that dominate it (every path
from entry to `B` passes through them), and each block's immediate dominator.

**Data structure.** 
```
DomInfo {
  std::vector<BasicBlock*> order;              // BFS order of the CFG
  DenseMap<BasicBlock*, unsigned> idx;         // block → its bit index
  DenseMap<BasicBlock*, BitVector> dom;        // block → set of dominators (by index)
  DenseMap<BasicBlock*, BasicBlock*> idom;     // block → immediate dominator
}
```

**Algorithm (iterative fixed-point, *intersection* meet):**
1. BFS the CFG; assign each block an index (the bit position in the `dom` vectors).
2. Initialize `DOM[entry] = {entry}`, `DOM[everything else] = all-ones` (the "top"
   element for an intersection lattice — start pessimistic, shrink down).
3. Iterate: `DOM[B] = {B} ∪ (⋂ DOM[P] over predecessors P)`. `newDom &= dom[pred]`
   is the intersection; `newDom.set(i)` adds B itself.
4. Compute `idom`: for each block take its *strict* dominators, and the immediate
   dominator is the one strict dominator that is itself dominated by all the others
   (i.e. the deepest / closest one in the dominator tree). The double loop checks
   "is `d` dominated by another strict dominator `d2`?" — if never, `d` is the idom.

**How it's consumed.** `applySchedule` uses `domInfo.dom` for the hoisting safety
check: an instruction may only be hoisted to a target block if the target's
dominance relationship guarantees the instruction still executes on every path that
reaches its original location. `idom` is computed but is more of a completeness /
debug artifact here.

---

### 3.5 `traceIdentification.hpp` — profile-guided hot traces

**What it computes.** Partitions the function's blocks into **traces** — linear
sequences of blocks that tend to execute together on the hot path. Each block ends up
in exactly one trace. Traces become the scheduling regions for global scheduling.

**Inputs.** `BranchProbabilityInfo& BPI` (per-edge probability) and
`BlockFrequencyInfo& BFI` (per-block execution frequency), both derived from the
profile data baked into `benchmark.ll`.

**Key constant.** `TRACE_THRESHOLD = BranchProbability(6, 10)` = 60%. An edge is only
followed to extend a trace if its probability strictly exceeds 60%.

**Two helper functions:**
- `bestSuccessor(BB, ...)` — among `BB`'s successors, pick the highest-probability one
  that is (a) not already in a trace, (b) above the 60% threshold, and (c) **not a
  back edge**. Back edges are detected via BFS order: if the successor appears
  *before* the current block in BFS order, the edge closes a loop, so it's skipped.
  This is what keeps traces from crossing loop boundaries.
- `bestPredecessor(BB, ...)` — mirror image, growing backward; a predecessor that
  appears *after* the current block in BFS order is a back edge and skipped.

**Main algorithm `identifyTraces`:**
1. Compute BFS order of the CFG (reused for back-edge detection).
2. Repeat until every block is visited:
   - **Seed:** pick the unvisited block with the highest `BFI` frequency (the hottest
     remaining block).
   - **Grow forward** from the seed following `bestSuccessor` until none qualifies.
   - **Grow backward** from the seed following `bestPredecessor`, inserting at the
     front of the trace.
   - Push the completed trace.
3. Cold blocks that can't extend in either direction become **single-block traces**,
   which makes the global scheduler degenerate gracefully into local scheduling for
   those blocks.

**Data structures.** `std::vector<std::vector<BasicBlock*>>` (traces, each a block
list in execution order), a `std::set<BasicBlock*> visited`, and the BFS-order
vector. Traces come out in hotness order.

---

### 3.6 `ddg.hpp` — the Data Dependence Graph

This is the structural heart. It defines the graph, the latency model, and how edges
are built.

**Node — `DDGNode`:**
```
Instruction* instr;                 // the instruction this node wraps
int delay;                          // latency in cycles (from getDelay)
int criticalPath;                   // filled in later by critical.hpp
int predCount;                      // # unscheduled predecessors (list-scheduling counter)
std::vector<DDGNode*> successors;   // nodes that depend on this one
std::vector<DDGNode*> predecessors; // nodes this one depends on
```

**Graph — `DDG`:** owns `std::vector<DDGNode*> nodes` and provides:
- `leaves()` — nodes with no predecessors (initial ready set for scheduling).
- `roots()` — nodes with no successors (the sinks; where critical path bottoms out).
- a destructor that `delete`s all nodes (manual memory management — the nodes are
  raw `new`'d).

**Latency model — `getDelay(Instruction*)`:** a simplified RISC-V-ish cost table:
`Load = 3`, `Store = 1`, `Call = 5`, `Mul = 2`, `UDiv/SDiv = 10`, everything else
`= 1`. (The header comment mentions "LOAD = 3…DIV = 10"; the code matches.)

**Edge insertion — `addEdge(src, dst)`:** appends `dst` to `src->successors` and
`src` to `dst->predecessors`, increments `dst->predCount`, and dedups (won't add the
same edge twice). Semantics: **`src` must execute before `dst`**.

**`buildDDG(traces, liveness, reaching)` — builds one DDG per trace:**
1. **Create nodes.** One node per instruction across all blocks of the trace, *except
   PHI nodes* (which are never scheduled). Record `instr → node` in a `DenseMap`.
2. **True (RAW) dependence edges.** For each instruction `B` and each of its operands
   `V`, if `V` is an instruction `A` in the same trace, add `A → B`. Because the IR
   is SSA, the operand *is* the definition — no reaching analysis needed. PHI nodes
   and terminators are skipped as both sources and destinations.
3. **Memory-ordering edges.** Within each block, chain every load/store to all
   previous loads/stores in original program order. This single conservative rule
   covers store→load (RAW), load→store (WAR), and store→store (WAW) memory
   dependences **without needing alias analysis** — the price is that memory ops can
   never be reordered relative to each other.

Note the `reaching` argument is accepted but unused (see §3.2). Anti/output
dependences on *registers* aren't added as edges — SSA makes them impossible for
value definitions, and memory is handled by the ordering chain.

---

### 3.7 `critical.hpp` — critical-path length

**What it computes.** For each `DDGNode`, `criticalPath` = the longest
latency-weighted path from that node to any sink. This is the scheduling *urgency*
metric: a node with a long critical path is holding up a long dependent chain, so it
should be scheduled early.

**Algorithm:**
1. `topologicalSort(ddg)` — DFS post-order over successors, then reverse. Standard
   topo sort; produces sources-first ordering. Uses `std::set<DDGNode*> visited` to
   avoid revisits.
2. `computeCriticalPath(ddg)` — walk the topo order **in reverse** (sinks first) so
   that when a node is processed all its successors are already done:
   - Sink (no successors): `criticalPath = delay`.
   - Otherwise: `criticalPath = delay + max(successor.criticalPath)`.

Because the DDG is a DAG (SSA + acyclic memory chains within a block), the topo sort
is well-defined and the recurrence terminates in one pass.

---

### 3.8 `listScheduler.hpp` — list scheduling with a pressure tiebreaker

Turns a DDG into a **linear instruction order** that respects all dependences. This
is the core scheduling decision; `applySchedule` only executes the plan it produces.

**Priority policy (two levels):**
- **Primary:** highest `criticalPath` first (most urgent).
- **Secondary (tiebreaker):** lowest `pressureDelta` — prefer the instruction that
  grows the live set least (or shrinks it most).

**`pressureDelta(I, currentLive, universe, liveness)`** — estimates the net change in
number of live values if `I` is scheduled now:
- `+1` if `I` produces a value not already live (a new value enters the live set).
- `−1` for each operand that is live now but **not live-out of `I`'s block**
  (`currentLive.test(idx) && !liveness[BB].out.test(idx)`) — meaning it dies here and
  frees a register.
- Net delta returned; negative is good (relieves pressure).

**`updateLiveSet(I, currentLive, …)`** — applied *after* each scheduling choice:
sets `I`'s result bit, and clears operand bits that are not live-out of the block.
Keeps `currentLive` an accurate running picture as scheduling proceeds.

**`pickBest(readyQueue, …, useRegPressure)`** — scans the ready queue, tracking
`bestCP` and `bestDelta`. Higher CP wins outright; on a CP tie, lower delta wins. When
`useRegPressure` is false, delta is forced to `0` — this is the switch that produces
the "global without register pressure" configuration.

**`listSchedule(ddg, currentLive, universe, liveness, useRegPressure)`** — the loop:
1. Reset every node's `predCount` to its real predecessor count.
2. Seed the ready queue with all leaves (`predCount == 0`) that are neither
   terminators nor PHIs.
3. While the ready queue is non-empty:
   - `pickBest` → append its instruction to the `schedule`.
   - Remove it from the queue; `updateLiveSet`.
   - For each successor, decrement `predCount`; when it hits `0` (and it's not a
     terminator/PHI), it becomes ready.
4. Return the `std::vector<Instruction*> schedule`.

The `currentLive` bitvector is passed by reference and threaded through the whole
process, so the pressure estimate reflects the order chosen so far.

---

### 3.9 `applySchedule.hpp` — rewriting the IR (the risky part)

The list scheduler only *decides* an order; this file *physically mutates* the LLVM
IR to realize it, in two phases, and then repairs SSA.

**Helpers:**
- `isSameLoopOrOuter(fromBB, targetBB, LI)` — returns true only if both blocks are in
  the same loop (or both loop-free). Prevents hoisting an instruction out of / across
  a loop, which the project deliberately leaves to LICM/LCM.
- `fixSSA(I, fromBB, toBB)` — after a move, uses LLVM's `SSAUpdater` to rewrite all
  non-PHI uses of `I` so SSA stays valid. PHI users are left to the updater's
  implicit handling. This is delegated to the LLVM API on purpose, to keep the focus
  on the scheduling logic rather than reimplementing SSA reconstruction.

**`applySchedule(schedule, trace, domInfo, LI, universe, liveness, usePressure)`:**

*Setup.* Build `tracePos` (block → index within trace) and `originalBlock`
(instruction → its block *before any movement*). Capturing original blocks up front
matters because Phase 2 moves instructions and later checks must reason about where
things started.

*Phase 1 — within-block reordering.* For each block, filter `schedule` down to the
instructions that originally lived in that block (excluding terminators/PHIs), then
re-lay them out in scheduled order starting at the first non-PHI insert point.
**Stores act as hard anchors:** the insert point is advanced *past* each store so
nothing is reordered across it — a conservative substitute for alias analysis (we
can't prove which loads are independent of a store, so we never move anything before
one it originally followed).

*Phase 2 — cross-block hoisting.* Initialize `dynamicLive[BB] = liveness[BB].in` for
each trace block; this is updated as instructions move so pressure reflects reality.
Then for each instruction in schedule order (skipping terminators, PHIs, **stores,
and calls** — never moved across blocks):
1. Find its original block index `origIdx` in the trace.
2. Try target blocks from the **start of the trace up to** `origIdx` (greedy
   earliest-safe placement). For each candidate `targetBB`:
   - **Not a loop header** (headers execute every iteration).
   - **Same loop level** (`isSameLoopOrOuter`).
   - **All operands available at the target.** For each operand defined by some
     instruction `def`: OK if `def` is in `targetBB`, or in an *earlier* trace block,
     or — if `def` is outside the trace — if `def`'s block **dominates** the target
     (the `domInfo.dom` check). This dominance test is the correctness guard against
     hoisting onto a path where an operand isn't yet defined.
   - **Register-pressure budget** (only when `usePressure`): if
     `dynamicLive[targetBB].count() + pressureDelta(...) > 12`, reject this target.
     The budget of 12 is empirical (observed peak pressure ≈ 9 in the benchmark; 12
     is a conservative headroom under RISC-V's 32 registers).
3. On the first target that passes, `moveBefore` the target's terminator, call
   `fixSSA`, update `originalBlock[I] = targetBB`, and add `I`'s result bit to
   `dynamicLive[targetBB]`. Then `break` (earliest safe position found).
4. If no earlier block qualifies, the instruction stays put.

The subtle safety argument (spelled out in the code comments): for a diamond
`b1→b2→b3→b4` and `b1→b5→b4`, moving an instruction from `b4` up to `b3` is wrong
because the `b5→b4` path skips `b3`. Requiring the target to strictly dominate the
origin rules this out.

---

### 3.10 `localScheduler.hpp` — the single-block baseline

Deliberately reuses everything above but confines it to one basic block, giving the
"local scheduling" configuration the project compares against.

- `buildLocalDDG(BB)` — same as `buildDDG` but for one block: node per instruction
  (PHIs included here since there's no cross-block issue, though they still won't be
  reordered by apply), true-dependence edges from SSA operands, and the same
  in-block memory-ordering chain. **No cross-block edges.**
- `applyLocalSchedule(schedule, BB)` — the in-block reordering only, byte-for-byte the
  same store-anchor logic as `applySchedule` Phase 1. **No Phase 2, no SSA repair**
  (nothing leaves the block, so SSA can't break).
- `runLocalScheduler(F, liveness, universe)` — for each block: build local DDG →
  `computeCriticalPath` → `listSchedule` (with pressure tiebreaker on) →
  `applyLocalSchedule` → free the DDG.

The point of keeping it structurally identical is a fair comparison: local vs. global
differ *only* in the scheduling region (one block vs. a trace) and whether cross-block
movement is allowed — not in the underlying priority function.

---

## 4. `src/schedulerPass.cpp` — the pass driver, walked through

This file is the orchestration layer. It defines two passes and registers three pass
names with LLVM's new pass manager. Below, the logic block by block.

**Includes (lines 1–10).** Pulls in every header in dependency order. Because the
headers are function-definition-in-header style, this single translation unit
compiles the whole scheduler.

**`struct globalSchedulerPass : PassInfoMixin<globalSchedulerPass>` (line 15).**
- **`bool usePressure` + constructor (17–21).** The one piece of pass state: whether
  register-pressure analysis is active. Set at registration time, this is what
  distinguishes the "with pressure" and "without pressure" global configurations from
  the *same* pass class.
- **`run(Function& F, FunctionAnalysisManager& FAM)` (23).** Signature required by
  the new PM for a function pass.
  - **Logging (24–27).** Prints which function it's running on and whether pressure
    is enabled — this is the banner you see in `testResults.md`.
  - **`computeLiveness(F)` (30).** Runs the backward liveness dataflow; result feeds
    the DDG build and every pressure computation.
  - **`computeReaching(F)` (32–33).** Computed but, per the inline comment, *not
    actually used* — SSA makes it unnecessary. Kept from an earlier design.
  - **Profile analyses (42–43).** `FAM.getResult<BranchProbabilityAnalysis>(F)` and
    `BlockFrequencyAnalysis` pull the profile-derived BPI/BFI out of the analysis
    manager. These read the branch weights embedded in `benchmark.ll`.
  - **`identifyTraces(F, BPI, BFI)` (46).** Partitions the function into hot traces.
  - **`buildDDG(traces, livenessResult, reachingResult)` (49).** One DDG per trace.
  - **`computeDominators(F)` (52).** Needed by `applySchedule`'s hoisting safety
    check.
  - **Critical paths (55–57).** Loops over the DDGs, filling each node's
    `criticalPath`.
  - **Building the `universe` (61–70).** Args first, then non-void instructions —
    **the same universe ordering as `computeLiveness`**, so BitVector indices are
    consistent between the liveness result and the pressure logic. This is important:
    a mismatch here would silently corrupt every pressure estimate.
  - **`LoopAnalysis` (74).** `LoopInfo` for the loop-header / same-loop guards.
  - **Per-trace scheduling loop (80–93).** For each trace:
    - **`currentLive` (83).** Seeded from `livenessResult[firstBB].in` — the values
      live entering the trace.
    - **`listSchedule(...)` (87).** Produces the globally-ordered instruction list for
      this trace, honoring `usePressure`.
    - **`applySchedule(...)` (92).** Physically reorders within blocks (Phase 1) and
      hoists across blocks (Phase 2).
  - **`return PreservedAnalyses::all()` (95).** Claims all analyses are preserved.
    (This is optimistic — the pass mutates the IR — but acceptable here because it's
    the last transform in the tested pipelines; nothing downstream relies on stale
    analyses.)

**`struct localSchedulerPass` (line 100).** The stripped-down baseline:
- `run` logs, calls `computeLiveness`, builds the *same* universe (args + non-void
  instructions), and calls `runLocalScheduler`. No traces, no dominators, no loop
  info, no cross-block movement. Returns `PreservedAnalyses::all()`.

**Plugin registration (128–151).** `llvmGetPassPluginInfo()` is the entry point
`opt` looks for in the `.so`.
- Registers a `PipelineParsingCallback` that maps textual pass names (from the
  `-passes=` string) to pass objects:
  - `"globalSchedulerPass"` → `globalSchedulerPass(true)` (pressure on).
  - `"localSchedulerPass"` → `localSchedulerPass()`.
  - `"globalSchedulerPassNoPressure"` → `globalSchedulerPass(false)` (pressure off).
- Returning `true` tells the parser the name was recognized. This is exactly why the
  scripts can say `-passes="mem2reg,globalSchedulerPass"` etc.

**Design takeaway.** The "with vs. without pressure" comparison is achieved with a
single boolean threaded from registration → pass field → `listSchedule` →
`pickBest`/`applySchedule`. There is no code duplication between the two global
modes; they are the same pass instantiated differently. That's what makes the
isolation experiment (how much does pressure awareness actually matter) clean.

---

## 5. Build & run scripts

### 5.1 `Makefile`

Compiles `src/schedulerPass.cpp` into `build/schedulerPass.so`:
- `CXXFLAGS`/`LDFLAGS`/`LIBS` come from `llvm-config` so it links against the
  installed LLVM.
- Builds `-shared -fPIC` with `-Iinclude`, producing the loadable plugin.
- `make clean` removes `build/`.

### 5.2 `benchmark.sh` — the full micro-benchmark pipeline

Runs end to end and prints the llvm-mca comparison table. Stages:
1. **Profile collection.** `clang -O0 -fprofile-instr-generate` builds an
   instrumented binary, runs it to emit `default.profraw`, then `llvm-profdata merge`
   → `benchmark.profdata`.
2. **Profiled IR.** Recompile `benchmark.c` to `benchmark.ll` with
   `-fprofile-instr-use=…profdata`, targeting `riscv64`, and
   `-disable-O0-optnone` so `opt` passes are actually allowed to run on the `-O0` IR.
3. **Run the four configurations** with `opt -load-pass-plugin schedulerPass.so`:
   - `mem2reg,localSchedulerPass` → `output_local.ll`
   - `mem2reg,globalSchedulerPass` → `output_global.ll`
   - `mem2reg,globalSchedulerPassNoPressure` → `output_global_no_pressure.ll`
   - `mem2reg` only → `original_benchmark.ll` (baseline). **`mem2reg` runs first in
     every case** — it's what promotes memory to SSA registers so the schedulers see
     proper def-use chains.
4. **Lower to RISC-V** with `llc -march=riscv64 -mcpu=rocket-rv64` for each `.ll`.
5. **Static analysis.** `llvm-mca` on each `.s`, output to `mca_*.txt`.
6. **Real binaries.** `llc -filetype=obj` then `clang --target=riscv64-linux-gnu
   -static -O0` link into runnable ELF binaries (`run_local`, `run_global`,
   `run_global_no_pressure`, `original_benchmark`).
7. **Correctness check.** Runs all four binaries and prints their exit codes — they
   must match (all `Exit: 64` in the results), proving the transforms preserved
   semantics.
8. **`print_stats`** greps Instructions / Total Cycles / uOps / IPC / Block
   RThroughput out of the mca files, and counts `Spill` occurrences in the `.s` files
   for the "Register Spills" line.

### 5.3 `mibench.sh` — same pipeline for MiBench SHA

Structurally identical to `benchmark.sh`, but the input is MiBench's `security/sha`
program:
- Compiles `sha.c` + `sha_driver.c` instrumented, runs on `input_small.asc` to
  profile.
- Recompiles `sha.c` and `sha_driver.c` separately to IR, then **`llvm-link`s them**
  into one `sha_benchmark.ll` before running the passes (so the whole program is one
  module the scheduler can see).
- Same four-configuration `opt` runs, `llc`, `llvm-mca`, object/link, and
  `print_stats` reporting. (It links `..._mb` binaries for the gem5 MiBench script.)

### 5.4 `gem5_benchmark.sh` — dynamic simulation of the micro-benchmark

Runs the *already-built* benchmark binaries inside the gem5 RISC-V simulator to get
real execution numbers (llvm-mca is only static estimate).
- `GEM5=/gem5/build/RISCV/gem5.opt`, config `se.py` (syscall-emulation mode).
- **`CPU_OPTS="--cpu-type=MinorCPU --caches --l2cache"`** — MinorCPU is an
  **in-order** pipeline model; this is the whole point, because compiler scheduling
  only matters when the hardware doesn't reorder for you. Caches/L2 enabled.
- Loops over `original_benchmark`, `run_local`, `run_global`,
  `run_global_no_pressure`; runs each, then `awk`s `stats.txt` for `numInsts`,
  `numCycles`, `ipc`, `simSeconds` and prints them.

### 5.5 `gem5_mibench.sh` — dynamic simulation of MiBench

Same as above but:
- Binaries are the `..._mb` set (`run_local_mb`, `run_global_mb`,
  `run_global_no_pressure_mb`).
- **`CPU_OPTS="--cpu-type=AtomicSimpleCPU"`** — a simpler atomic CPU model (faster,
  coarser) used for the heavier MiBench workload.

---

## 6. Cross-references to the results (why the code produces what `testResults.md` shows)

- **Local beats original** because Phase-1 within-block reordering already spreads
  dependent ops apart (fewer stalls, and `mem2reg` + reordering trims a few
  instructions/spills).
- **Global-with-pressure is best on the hot path** because Phase-2 hoists
  independent loads from the later block up into the entry block on the trace, hiding
  latency — while the budget-12 check keeps spills equal to local (8 in the
  benchmark).
- **Global-without-pressure regresses** (spills jump back to ~11–12, instruction
  count worse) because the same hoisting runs *without* the `pressureDelta` budget,
  so it over-hoists, blows past the register file, and the backend spills. This is the
  single-boolean `usePressure` difference from §4 made visible.
- **The "cold path" experiment regresses global** because the speculatively hoisted
  instructions land on a path that isn't taken — pure wasted work, the known downside
  of trace scheduling on a mispredicted profile.
- **MiBench shows global ≈ local** because the SHA hot blocks have tight in-block
  dependences and hit the pressure budget early, so Phase-2 finds few legal moves and
  the global scheduler gracefully degrades to local behavior.
```
