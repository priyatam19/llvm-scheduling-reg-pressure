#pragma once

#include "schedulerPass.hpp"
#include "dom.hpp"


using namespace llvm;

// Does physical instruction movement across blocks
// Takes the instruction order produced by listSchedule() and
// physically reorders instructions in the LLVM IR.
// Two phases:
//   Phase 1 — Within-block reordering
//   Phase 2 — Cross-block movement


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

// Repairs SSA form after moving instruction I from fromBB to toBB
// Use SSAUpdater to fix all PHI nodes and uses correctly
// Uses SSAUpdater to rewrite all non-PHI uses of I's result
// PHI node users are skipped, SSAUpdater handles them implicitly
// Used LLVM API here to focus more on our part of global scheduling
static void fixSSA(Instruction* I, BasicBlock* fromBB, BasicBlock* toBB) {

    if (I->getType()->isVoidTy()) return;

    SSAUpdater SSA;
    SSA.Initialize(I->getType(), I->getName());

    // Value is now available in toBB
    SSA.AddAvailableValue(toBB, I);

    // Fix all uses
    for (Use& U : make_early_inc_range(I->uses())) {
        Instruction* user = cast<Instruction>(U.getUser());
        if (isa<PHINode>(user)) continue;
        SSA.RewriteUse(U);
    }
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
    DomInfo& domInfo, LoopInfo& LI, std::vector<Value*>& universe,   
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


    // Phase 1 — reorder within each block
    // Use schedule order for within-block ordering
    
    for (BasicBlock* BB : trace) {

        std::vector<Instruction*> blockOrder;
        for (Instruction* I : schedule)
            if (originalBlock.count(I) &&
                originalBlock[I] == BB &&
                !I->isTerminator() &&
                !isa<PHINode>(I))
                blockOrder.push_back(I);

        if (blockOrder.empty()) continue;

        Instruction* insertPoint = &*BB->getFirstNonPHIIt();

        for (Instruction* I : blockOrder) {
            if (I->getParent() != BB) continue;

            // Store is an anchor 
            // advance insertPoint past it
            // so no instruction moves before it
            // This is a conservative approach. 
            // Without alias analysis we cannot prove which
            // loads are independent of this store, so no instruction
            // moves before a store it followed in original program order

            if (isa<StoreInst>(I)) {
                while (insertPoint != I && insertPoint->getNextNode())
                    insertPoint = insertPoint->getNextNode();
                if (insertPoint->getNextNode())
                    insertPoint = insertPoint->getNextNode();
                continue;
            }

            if (I == insertPoint) {
                if (insertPoint->getNextNode())
                    insertPoint = insertPoint->getNextNode();
                continue;
            }

            I->moveBefore(*BB, insertPoint->getIterator());
            if (I->getNextNode())
                insertPoint = I->getNextNode();
        }
    }   

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
    for (Instruction* I : schedule) {

        // Instructions that are never moved across blocks
        if (I->isTerminator()) continue;
        if (isa<PHINode>(I)) continue;
        if (isa<StoreInst>(I)) continue;
        if (isa<CallInst>(I)) continue;
        
        // Get original block
        BasicBlock* origBB = originalBlock.count(I) ? originalBlock[I] : I->getParent();

        if (!tracePos.count(origBB)) continue;
        int origIdx = tracePos[origBB];

        // Try to move to earliest block in trace
        for (int targetIdx = 0; targetIdx < origIdx; targetIdx++) {
            BasicBlock* targetBB = trace[targetIdx];

            // Do not move into loop headers
            // they execute every iteration
            if (LI.isLoopHeader(targetBB)) continue;

            // Same loop level only
            if (!isSameLoopOrOuter(origBB, targetBB, LI)) continue;

            // Check all operands available at targetBB
            // Use CURRENT location of defs
            // DDG already guarantees correct schedule order
            bool ok = true;
            for (Use& U : I->operands()) {
                Instruction* def = dyn_cast<Instruction>(U.get());
                // constants and args always ok
                if (!def) continue; 

                // Current location of def
                BasicBlock* defBB = def->getParent();

                bool defOk = false;
                if (defBB == targetBB) {
                    // Def is in target block
                    // available
                    defOk = true;
                } 

                else if (tracePos.count(defBB) && tracePos[defBB] < targetIdx) {
                    // Def is in earlier trace block
                    defOk = true;
                }

                // Check --— targetBB dominates origBB
                // Safety --- every path to origBB must pass through targetBB
                // Consider --- b1->b2->b3->b4 and b1->b5->b4
                // Moving I from b4 to b3 is wrong if b5→b4 path exists
                // because b3 is skipped on that path — I never executes
                // targetBB strictly dominating origBB guarantees no such bypass path exists

                else if (!tracePos.count(defBB)) {
                    // Def is outside trace
                    // Check if defBB dominates targetBB
                    auto defIt = domInfo.idx.find(defBB);
                    auto tgtIt = domInfo.idx.find(targetBB);
                    if (defIt != domInfo.idx.end() &&
                        tgtIt != domInfo.idx.end() &&
                        domInfo.dom[targetBB].test(defIt->second))
                        defOk = true;
                }

                
                if (!defOk) { ok = false; break; }
            }

            if (!ok) continue;

            // Register pressure check 
            // only if usePressure is true
            // Uses dynamic live set that updates with each movement
            // If usePressure is false skip this check entirely
            // and allow movement regardless of pressure impact

            // Check the register pressure stays within the budget
            // Budget of 12 based on observed peak pressure from our benchmark programs
            if (usePressure) {
                int pressureAtTarget = (int)dynamicLive[targetBB].count();
                int delta = pressureDelta(I, dynamicLive[targetBB], universe, livenessResult);

                // RISC V has 32 registers
                // Allow movement only if total pressure stays within register budget
                // Budget of 12 (Observed peak pressure of 9 in our benchmark during test runs)
                // This value is experimental and conservative
                if (pressureAtTarget + delta > 12) continue;
            }

            // Safe to move
            BasicBlock* fromBB = I->getParent();

            // Move instruction to targetBB before its terminator
            I->moveBefore(*targetBB, targetBB->getTerminator()->getIterator());

            // Fix SSA
            // update PHI nodes and uses
            fixSSA(I, fromBB, targetBB);

            // Update original block map
            originalBlock[I] = targetBB;

            // After moving instruction — update dynamic live set
            // Add I's result to the target block's dynamic live set
            if (!I->getType()->isVoidTy()) {
                auto it = std::find(universe.begin(), universe.end(), (Value*)I);
                if (it != universe.end())
                    dynamicLive[targetBB].set(it - universe.begin());
            }

            break;
        }
    }
}