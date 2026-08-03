#pragma once

#include "schedulerPass.hpp"
#include "critical.hpp"
#include "listScheduler.hpp"
#include "liveness.hpp"

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

    // Memory ordering constraints
    std::vector<Instruction*> memOps;
    for (Instruction& I : *BB) {
        if (isa<LoadInst>(&I) || isa<StoreInst>(&I)) {
            for (Instruction* prev : memOps) {
                DDGNode* src = instrToNode.lookup(prev);
                DDGNode* dst = instrToNode.lookup(&I);
                if (src && dst)
                    addEdge(src, dst);
            }
            memOps.push_back(&I);
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
    std::vector<Instruction*> blockOrder;
    for (Instruction* I : schedule) {
        if (I->getParent() == BB &&
            !I->isTerminator() &&
            !isa<PHINode>(I))
            blockOrder.push_back(I);
    }

    if (blockOrder.empty()) return;

    Instruction* insertPoint = &*BB->getFirstNonPHIIt();

    for (Instruction* I : blockOrder) {
        if (I->getParent() != BB) continue;

        // Store is an anchor — advance insertPoint past it
        // so nothing moves before it
        if (isa<StoreInst>(I)) {
            // Walk insertPoint forward until it reaches I
            while (insertPoint != I && insertPoint->getNextNode())
                insertPoint = insertPoint->getNextNode();
            // Then step past it
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

// Main local scheduling function
// Schedules each basic block independently
// No cross block movement
// For each block:
//   1. Build DDG within basic block
//   2. Compute critical path within block
//   3. List schedule with cp + pressure tie breaker
//   4. Apply scheduling within block only
static void runLocalScheduler(Function& F, DenseMap<const BasicBlock*, 
    BlockStateLiveness>& livenessResult, std::vector<Value*>& universe){
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
        auto schedule = listSchedule(ddg, currentLive, universe, livenessResult);

        // Apply scheduling to the block only
        // No cross block movement
        applyLocalSchedule(schedule, &BB);

        delete ddg;
    }
}