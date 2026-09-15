#pragma once

#include "schedulerPass.hpp"
#include "critical.hpp"
#include "listScheduler.hpp"
#include "liveness.hpp"
#include "scheduleLegality.hpp"

using namespace llvm;

// The localScheduler reuses most of the code written for global scheduler
// It does not find traces
// Builds DDG for a single basic block
// No cross block edges
// Only difference lies in constructing DDG and applying scheduling that avoids cross block movements
// Others remain same

static DDG* buildLocalDDG(BasicBlock* BB) {

    DDG* ddg = new DDG();

    // Map from instruction to its DDG node
    DenseMap<Instruction*, DDGNode*> instrToNode;

    // Create one node per instruction
    for (Instruction& I : *BB) {
        DDGNode* node      = new DDGNode();
        node->instr        = &I;
        node->delay        = getDelay(&I);
        node->criticalPath = 0;
        node->predCount    = 0;
        ddg->nodes.push_back(node);
        instrToNode[&I]    = node;
    }

    // True dependence edges
    for (Instruction& I : *BB) {
        DDGNode* dst = instrToNode[&I];
        for (Use& U : I.operands()) {
            Instruction* def = dyn_cast<Instruction>(U.get());
            if (!def) continue;
            DDGNode* src = instrToNode.lookup(def);
            if (!src) continue;
            addEdge(src, dst);
        }
    }   

    // Conservatively preserve the original order of every operation that may
    // observe or change memory/program state, including calls.
    std::vector<Instruction*> orderedOps;
    for (Instruction& I : *BB) {
        if (isa<CallBase>(&I) || I.mayReadOrWriteMemory() || I.mayHaveSideEffects()) {
            for (Instruction* prev : orderedOps) {
                DDGNode* src = instrToNode.lookup(prev);
                DDGNode* dst = instrToNode.lookup(&I);
                if (src && dst)
                    addEdge(src, dst);
            }
            orderedOps.push_back(&I);
        }
    }

    return ddg;
}


// Reorders instructions within a single block
// according to the scheduled order
// No cross block movement at all
static void applyLocalSchedule(
    std::vector<Instruction*>& schedule,
    BasicBlock* BB)
{
    reorderBlockWithinRegions(schedule, BB);
}

// Main local scheduling function
// Schedules each basic block independently
// No cross block movement
// For each block:
//   1. Build DDG within basic block
//   2. Compute critical path within block
//   3. List schedule with cp + pressure tie breaker
//   4. Apply scheduling within block only
static void runLocalScheduler(Function& F, DenseMap<const BasicBlock*,
    BlockStateLiveness>& livenessResult, DenseMap<Value*, unsigned>& universeIndex){
    // Schedule each block independently
    // Iterates each basic block
    for (auto& BB : F) {

        // Build DDG for this basic block
        DDG* ddg = buildLocalDDG(&BB);

        // Compute critical path within block
        computeCriticalPath(ddg);

        // Initialize live set from block entry
        BitVector currentLive = livenessResult[&BB].in;

        // List schedule uses same algorithm as global
        // critical path priority + pressure tie breaker
        auto schedule = listSchedule(ddg, currentLive, universeIndex, livenessResult);

        // Apply scheduling to the block only
        // No cross block movement
        applyLocalSchedule(schedule, &BB);

        delete ddg;
    }
}
