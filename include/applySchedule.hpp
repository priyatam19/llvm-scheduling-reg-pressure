#pragma once

#include "schedulerPass.hpp"
#include "scheduleLegality.hpp"
#include "traceIdentification.hpp"
#include <cstdlib>


using namespace llvm;

// Does physical instruction movement across blocks
// Takes the instruction order produced by listSchedule() and
// physically reorders instructions in the LLVM IR.
// Two phases:
//   Phase 1 — Within-block reordering
//   Phase 2 — Cross-block movement

// Register pressure budget used by Phase 2 hoisting decisions.
// Default 12, overridable via env var SCHED_PRESSURE_BUDGET (integer),
// read once so a sensitivity sweep can vary it without recompiling.
static int getPressureBudget() {
    static int budget = [] {
        int val = 12;
        if (const char* env = std::getenv("SCHED_PRESSURE_BUDGET")) {
            int parsed = std::atoi(env);
            if (parsed > 0)
                val = parsed;
        }
        return val;
    }();
    return budget;
}

// Per-function Phase 2 hoist stats, only touched when schedStatsEnabled().
struct HoistStats {
    unsigned candidates       = 0;
    unsigned done             = 0;
    unsigned blockedDominance = 0;
    unsigned blockedLoop      = 0;
    unsigned blockedOperand   = 0;
    unsigned blockedUnsafe    = 0;
    unsigned blockedPressure  = 0;
};
static HoistStats g_hoistStats;


// Do not move instruction out of its loop
// If I is in a loop and targetBB is not in the same loop
// skip the movement
// Returns true if fromBB and targetBB are in the same loop
static bool isSameLoopOrOuter(BasicBlock* fromBB, BasicBlock* targetBB, LoopInfo& LI) {
    Loop* fromLoop = LI.getLoopFor(fromBB);
    Loop* targetLoop = LI.getLoopFor(targetBB);
    if (!fromLoop && !targetLoop) return true;
    if (!fromLoop || !targetLoop) return false;
    if (fromLoop != targetLoop) return false;
    return true;
}

// Physically moves instructions to their earliest safe position in the trace
// Algorithm:
//   Phase 1 — reorder within each block using global schedule order
//   Phase 2 — cross block movement
//              for each instruction in global schedule order
//              try to move to earliest block in trace where:
//                  same loop level
//                  not a loop header
//                  all operands available (using current location)
//                  fix PHI nodes after movement
static void applySchedule(std::vector<Instruction*>& schedule, std::vector<BasicBlock*>& trace,
    DominatorTree& DT, AssumptionCache& AC, TargetLibraryInfo& TLI,
    LoopInfo& LI, DenseMap<Value*, unsigned>& universeIndex,
    DenseMap<const BasicBlock*, BlockStateLiveness>& livenessResult, bool usePressure) {

    // Map block --- position in trace
    DenseMap<BasicBlock*, int> tracePos;
    for (int i = 0; i < (int)trace.size(); i++)
        tracePos[trace[i]] = i;

    // Track original block of ALL instructions in trace
    // before any movement happens
    DenseMap<Instruction*, BasicBlock*> originalBlock;
    for (BasicBlock* BB : trace)
        for (Instruction& I : *BB)
            originalBlock[&I] = BB;


    // Phase 1 — reorder pure instructions only within anchor-bounded regions.
    for (BasicBlock* BB : trace)
        reorderBlockWithinRegions(schedule, BB);

    // Initialize dynamic live sets
    // Updated as instructions are moved
    // Gives accurate pressure at each target block during Phase 2
    DenseMap<BasicBlock*, BitVector> dynamicLive;
    for (BasicBlock* BB : trace)
        dynamicLive[BB] = livenessResult[BB].in;

    // Phase 2 —-- cross block movement
    // For each instruction in schedule order, find the earliest
    // block in the trace where it can safely be moved.
    // Stores, calls and PHI nodes are never moved across blocks.

    // Algorithm Strategy: greedy earliest-position
    // For each instruction in schedule order, find the earliest
    // block in the trace where it can safely be placed.
    // Tries blocks from trace start toward I's original block —
    // takes the first block that passes all safety checks.
    // If no earlier block is safe — I stays in its original block.

    // Safety checks:
    // 1. Not a loop header — as it executes every iteration
    // 2. Same loop level — never hoist out of a loop (should be taken care by LCM and LICM)
    //      We did not do that in this pass because we will not be able to 
    //      see the real impact that is caused purely due to scheduling
    // 3. All operands available at target block (additional safety check)
    //      Def in target block or earlier trace block
    //        If not do a dom check (conservative move)
    // 5. Register pressure budget
    bool statsOn = schedStatsEnabled();

    for (Instruction* I : schedule) {

        // Get original block
        BasicBlock* origBB = originalBlock.count(I) ? originalBlock[I] : I->getParent();

        if (!tracePos.count(origBB)) continue;
        int origIdx = tracePos[origBB];

        // Only instructions with at least one earlier trace block to try
        // are real hoist candidates for Phase 2 (targetIdx loop below would
        // otherwise never execute).
        bool isCandidate = statsOn && origIdx > 0;
        if (isCandidate) g_hoistStats.candidates++;

        // Rejection-reason ranking used to pick the single most-informative
        // blocking reason for this candidate across all attempted target
        // positions (mutually exclusive per candidate — whichever check the
        // candidate got furthest past before failing). Rank order follows
        // the order checks run in a single attempt: loop < dominance <
        // operand-unavailable < pressure.
        enum class BlockReason { None, Loop, Dominance, Operand, Unsafe, Pressure };
        BlockReason bestReason = BlockReason::None;
        auto noteReason = [&](BlockReason r) {
            if (!statsOn) return;
            if (static_cast<int>(r) > static_cast<int>(bestReason))
                bestReason = r;
        };

        bool moved = false;

        // Anchors are deliberately present in the DDG/list schedule so their
        // dependencies can release other nodes, but they are never legal
        // cross-block moves.
        if (isSchedulingAnchor(I)) {
            if (isCandidate) g_hoistStats.blockedUnsafe++;
            continue;
        }

        // Try to move to earliest block in trace
        for (int targetIdx = 0; targetIdx < origIdx; targetIdx++) {
            BasicBlock* targetBB = trace[targetIdx];

            // Do not move into loop headers
            // they execute every iteration
            if (LI.isLoopHeader(targetBB)) { noteReason(BlockReason::Loop); continue; }

            // Same loop level only
            if (!isSameLoopOrOuter(origBB, targetBB, LI)) { noteReason(BlockReason::Loop); continue; }

            // Moving a single SSA definition upward is legal only when the
            // destination dominates its original block.  A hot trace order
            // alone does not establish this property.
            if (!DT.dominates(targetBB, origBB)) {
                noteReason(BlockReason::Dominance);
                continue;
            }

            Instruction* insertPoint = targetBB->getTerminator();
            if (!insertPoint) {
                noteReason(BlockReason::Unsafe);
                continue;
            }

            // Check every instruction operand at the exact insertion point.
            // This correctly handles definitions inside/outside the trace and
            // definitions moved by an earlier scheduling decision.
            bool ok = true;
            for (Use& U : I->operands()) {
                Instruction* def = dyn_cast<Instruction>(U.get());
                if (!def) continue;
                if (!DT.dominates(def, insertPoint)) { ok = false; break; }
            }

            if (!ok) { noteReason(BlockReason::Operand); continue; }

            // Even a side-effect-free instruction may be unsafe to execute on
            // paths that reach targetBB but not origBB (for example, a
            // potentially trapping operation).  ValueTracking provides the
            // target-context legality test.  Memory-reading instructions were
            // already rejected as anchors above.
            if (!isSafeToSpeculativelyExecute(I, insertPoint, &AC, &DT, &TLI)) {
                noteReason(BlockReason::Unsafe);
                continue;
            }

            // Register pressure check
            // only if usePressure is true
            // Uses dynamic live set that updates with each movement
            // If usePressure is false skip this check entirely
            // and allow movement regardless of pressure impact

            // Check the register pressure stays within the budget
            // Budget defaults to 12 (overridable via SCHED_PRESSURE_BUDGET),
            // based on observed peak pressure from our benchmark programs
            if (usePressure) {
                int pressureAtTarget = (int)dynamicLive[targetBB].count();
                int delta = pressureDelta(I, dynamicLive[targetBB], universeIndex, livenessResult);

                // RISC V has 32 registers
                // Allow movement only if total pressure stays within register budget
                // Default budget of 12 (Observed peak pressure of 9 in our benchmark during test runs)
                // This value is experimental and conservative; overridable via SCHED_PRESSURE_BUDGET
                if (pressureAtTarget + delta > getPressureBudget()) { noteReason(BlockReason::Pressure); continue; }
            }

            // Safe to move
            // Move instruction to targetBB before its terminator
            I->moveBefore(*targetBB, targetBB->getTerminator()->getIterator());

            // After moving instruction — update dynamic live set
            // Add I's result to the target block's dynamic live set
            if (!I->getType()->isVoidTy()) {
                auto it = universeIndex.find((Value*)I);
                if (it != universeIndex.end())
                    dynamicLive[targetBB].set(it->second);
            }

            moved = true;
            if (isCandidate) g_hoistStats.done++;
            break;
        }

        if (isCandidate && !moved) {
            switch (bestReason) {
                case BlockReason::Loop:      g_hoistStats.blockedLoop++;      break;
                case BlockReason::Dominance: g_hoistStats.blockedDominance++; break;
                case BlockReason::Operand:   g_hoistStats.blockedOperand++;   break;
                case BlockReason::Unsafe:    g_hoistStats.blockedUnsafe++;    break;
                case BlockReason::Pressure:  g_hoistStats.blockedPressure++;  break;
                case BlockReason::None:      break; // unreachable: origIdx>0 guarantees >=1 attempt
            }
        }
    }
}

// applySchedule() is called once per trace, so hoist stats must be
// accumulated by the caller (schedulerPass.cpp) across all traces of a
// function and reported once per function. Call resetHoistStats() before
// the per-trace loop and emitHoistStats() after it.
static void resetHoistStats() { g_hoistStats = HoistStats(); }

static void emitHoistStats() {
    if (!schedStatsEnabled()) return;
    errs() << "SCHED_STATS hoist_candidates=" << g_hoistStats.candidates
           << " hoist_done=" << g_hoistStats.done
           << " blocked_dominance=" << g_hoistStats.blockedDominance
           << " blocked_loop=" << g_hoistStats.blockedLoop
           << " blocked_operand=" << g_hoistStats.blockedOperand
           << " blocked_unsafe_speculation=" << g_hoistStats.blockedUnsafe
           << " blocked_pressure=" << g_hoistStats.blockedPressure << "\n";
}
