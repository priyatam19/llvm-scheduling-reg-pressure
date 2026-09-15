#pragma once

#include "schedulerPass.hpp"
#include "traceIdentification.hpp"

using namespace llvm;

// Peak live-set size (bitcount of currentLive) observed while list
// scheduling. listSchedule() is called once per trace (global scheduler)
// or once per basic block (local scheduler); the caller in
// schedulerPass.cpp accumulates the max across all calls for a function
// and reports one SCHED_STATS line per function via resetPeakLivePressure()
// / emitPeakLivePressure().
static unsigned g_peakLivePressure = 0;

static void resetPeakLivePressure() { g_peakLivePressure = 0; }

static void emitPeakLivePressure() {
    if (!schedStatsEnabled()) return;
    errs() << "SCHED_STATS peak_live_pressure=" << g_peakLivePressure << "\n";
}

// Implements the list scheduling algorithm for a single DDG.
// Produces a linear ordering of instructions that respects
// all data dependences while minimizing schedule length.

// Scheduling Priority:
// Primary   — highest critical path (most urgent first)
// Secondary — lowest register pressure delta (as tie breaker)

// Register Pressure:
// Tracked dynamically via a live set updated after each instruction is scheduled. 
// pressureDelta() estimates the net change in live values when scheduling I:
//     +1 for each new value I produces
//     -1 for each operand that dies at I (not live out of block)

// Returns an ordered list of instructions
// Which is used by applySchedule() to move instructions


// Computes how much scheduling instruction I changes register pressure
// Negative = good (reduces pressure)
// Positive = bad (increases pressure)
// Uses liveness analysis results to determine which values are live at any point
// Called during list scheduling as tie breaker when two instructions have equal critical path
static int pressureDelta(Instruction* I, BitVector& currentLive, DenseMap<Value*, unsigned>& universeIndex,
    DenseMap<const BasicBlock*, BlockStateLiveness>& liveness) {

    int delta = 0;

    // Inst I produces a new value which pressure increases
    // only if the value was not already live
    if (!I->getType()->isVoidTy()) {
        auto it = universeIndex.find((Value*)I);
        if (it != universeIndex.end()) {
            unsigned idx = it->second;
            // New value entering live set
            if (!currentLive.test(idx))
                delta++;
        }
    }

    // Each operand that is last used here --- decreases pressure
    // Check if operand is live out of I's block
    // If not live out --- dies here --- pressure decreases
    BasicBlock* BB = I->getParent();
    for (Use& U : I->operands()) {
        Value* V = U.get();
        auto it = universeIndex.find(V);
        if (it == universeIndex.end()) continue;
        unsigned idx = it->second;

        // If V is in current live set but not live out of block
        // it dies at or before end of block
        if (currentLive.test(idx) && !liveness[BB].out.test(idx))
            delta--;
    }

    return delta;
}


// Updates the current live set after scheduling I
// Called after each instruction is scheduled
// Adds I's result to live set if it produces a value
// Removes operands that die at I
static void updateLiveSet(Instruction* I, BitVector& currentLive,
    DenseMap<Value*, unsigned>& universeIndex, DenseMap<const BasicBlock*,
    BlockStateLiveness>& liveness){

    // Add I's result to live set
    if (!I->getType()->isVoidTy()) {
        auto it = universeIndex.find((Value*)I);
        if (it != universeIndex.end())
            currentLive.set(it->second);
    }

    // Remove operands that die here
    BasicBlock* BB = I->getParent();
    for (Use& U : I->operands()) {
        Value* V = U.get();
        auto it = universeIndex.find(V);
        if (it == universeIndex.end()) continue;
        unsigned idx = it->second;

        // If not live out of block --- remove from live set
        if (!liveness[BB].out.test(idx))
            currentLive.reset(idx);
    }
}


// Picks the best instruction from ready queue
// Priority 1 — highest critical path
// Priority 2 — lowest pressure delta (tie breaker)
static DDGNode* pickBest(std::vector<DDGNode*>& readyQueue, BitVector& currentLive,
    DenseMap<Value*, unsigned>& universeIndex, DenseMap<const BasicBlock*,
    BlockStateLiveness>& liveness, bool useRegPressure = true){

    DDGNode* best = nullptr;
    int bestCP    = -1;
    int bestDelta = INT_MAX;

    for (DDGNode* node : readyQueue) {

        int cp    = node->criticalPath;

        // Only compute pressure delta if useRegPressure is true
        // Since we do global scheduling with and without register pressure analysis
        // This flag is used to run globalScheduler with and without reg pressure analysis
        // If False then always 0
        int delta = useRegPressure ? pressureDelta(node->instr, currentLive, universeIndex, liveness): 0;

        // Priority 1
        if (cp > bestCP) {
            bestCP    = cp;
            bestDelta = delta;
            best      = node;
        }
        // Priority 2
        // tie on critical path
        // lower pressure delta wins
        else if (cp == bestCP && delta < bestDelta) {
            bestDelta = delta;
            best      = node;
        }
    }

    return best;
}


// listSchedule
// Algorithm:
//   Initialize ready queue with all leaves (nodes with no predecessors in DDG)
//   while ready queue not empty:
//       pick best instruction (highest cp, lowest pressure delta)
//       add to schedule
//       update live set
//       for each successor:
//           decrement predCount
//           if predCount == 0 then add to ready queue

// Returns ordered list of instructions
// This order is used by applySchedule() to
// physically reorder instructions in IR
// -------------------------------------------------------
static std::vector<Instruction*> listSchedule(DDG* ddg, BitVector& currentLive, DenseMap<Value*, unsigned>& universeIndex,
    DenseMap<const BasicBlock*, BlockStateLiveness>& liveness, bool useRegPressure = true){
    
    std::vector<Instruction*> schedule;

    bool statsOn = schedStatsEnabled();
    if (statsOn) {
        unsigned live0 = currentLive.count();
        if (live0 > g_peakLivePressure) g_peakLivePressure = live0;
    }

    // Ready queue —-- instructions with no unscheduled predecessors
    // Initially contains all leaves (nodes with no predecessors)
    std::vector<DDGNode*> readyQueue;

    // Reset predCount for all nodes
    // predCount tracks how many predecessors are not yet scheduled
    for (DDGNode* node : ddg->nodes)
        node->predCount = node->predecessors.size();

    // Initialize ready queue with leaves
    // Leaves have no predecessors can be scheduled immediately
    for (DDGNode* node : ddg->nodes)
        if (node->predCount == 0)
            if (!node->instr->isTerminator() && !isa<PHINode>(node->instr))
                    readyQueue.push_back(node);

    // Main scheduling loop
    while (!readyQueue.empty()) {

        // Pick best instruction from ready queue
        // Priority 1 — highest critical path
        // Priority 2 — lowest pressure delta (tie breaker)
        DDGNode* best = pickBest(readyQueue, currentLive, universeIndex, liveness, useRegPressure);

        // Remove from ready queue. readyQueue is bounded by instruction-level
        // parallelism (how many nodes are simultaneously ready), not function
        // size, so this std::find isn't the quadratic-over-the-function
        // pattern the universe lookups above were -- left as is.
        readyQueue.erase(std::find(readyQueue.begin(), readyQueue.end(), best));

        // Add to schedule
        schedule.push_back(best->instr);

        // Update live set after scheduling this instruction
        updateLiveSet(best->instr, currentLive, universeIndex, liveness);

        if (statsOn) {
            unsigned live = currentLive.count();
            if (live > g_peakLivePressure) g_peakLivePressure = live;
        }

        // For each successor S of best:
        //     decrement S.predCount
        //     if S.predCount == 0 --- all predecessors scheduled, S is now ready
        for (DDGNode* succ : best->successors) {
            succ->predCount--;
            if (succ->predCount == 0)
                if (!succ->instr->isTerminator() && !isa<PHINode>(succ->instr))
                    readyQueue.push_back(succ);
        }
    }

    return schedule;
}