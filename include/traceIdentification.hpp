#pragma once

#include "schedulerPass.hpp"

using namespace llvm; 

// Trace Identification using profile data
// Identifies hot execution traces through the CFG using 
// branch probabilities (BPI) and block frequencies (BFI).

// Threshold for choosing best Successor and Predecessor - 60%
static const BranchProbability TRACE_THRESHOLD = BranchProbability(6, 10);

// Grow trace forward from seed
// Returns the best successor of BB to extend the trace forward
// Valid successor must --- not be visited, exceed threshold probability,
// and not be a loop back edge
static BasicBlock* bestSuccessor(BasicBlock* BB, BranchProbabilityInfo& BPI, 
        std::set<BasicBlock*>& visited, std::vector<BasicBlock*>& bfsOrder) {

    BasicBlock* best    = nullptr;
    BranchProbability bestProb = BranchProbability::getZero();
    
    for (BasicBlock* succ : successors(BB)) {

        // Skip if already in a trace
        if (visited.count(succ)) continue;

        // Get probability of edge BB --> succ
        BranchProbability prob = BPI.getEdgeProbability(BB, succ);

        // Stop if probability is below threshold 60%
        if (prob <= TRACE_THRESHOLD) continue;

        // Back edge detection —-- if successor appears before
        // current block in BFS order it is a back edge
        // Back edges close loops so stops trace here
        // Skips back edge
        auto succIt = std::find(bfsOrder.begin(), bfsOrder.end(), succ);
        auto currIt = std::find(bfsOrder.begin(), bfsOrder.end(), BB);
        if (succIt < currIt) continue;  

        // Pick the successor with the highest probability
        if (prob > bestProb) {
            bestProb = prob;
            best     = succ;
        }
    }

    // null if no valid successor found
    return best;  
}


// Grow trace backward from seed
// Returns the best predecessor of BB to extend the trace backward
// Valid predecessor must --- not be visited, exceed threshold probability,
// and not be a loop back edge
static BasicBlock* bestPredecessor(BasicBlock* BB, BranchProbabilityInfo& BPI,
        std::set<BasicBlock*>& visited, std::vector<BasicBlock*>& bfsOrder) {

    BasicBlock* best    = nullptr;
    BranchProbability bestProb = BranchProbability::getZero();

    for (BasicBlock* pred : predecessors(BB)) {

        // Skip if already in a trace
        if (visited.count(pred)) continue;

        // Get probability of edge pred --> BB
        BranchProbability prob = BPI.getEdgeProbability(pred, BB);

        // Stop if probability is below threshold 60%
        if (prob <= TRACE_THRESHOLD) continue;

        // Back edge detection - if predecessor appears after
        // current block in BFS order it is a back edge
        // Skips back edge
        auto predIt = std::find(bfsOrder.begin(), bfsOrder.end(), pred);
        auto currIt = std::find(bfsOrder.begin(), bfsOrder.end(), BB);
        if (predIt > currIt) continue;  

        // Pick predecessor with highest probability
        if (prob > bestProb) {
            bestProb = prob;
            best     = pred;
        }
    }

    // null if no valid predecessor found
    return best;  
}

// Identifies all hot execution traces through function F
// BFI --- Gives execution frequency per block, used to pick seed
// BPI --- Gives probability per edge, used to pick best successor/predecessor
// Returns traces in hotness order, each as a vector of blocks
// in execution order. Every block belongs to exactly one trace.
std::vector<std::vector<BasicBlock*>> identifyTraces(Function& F, 
    BranchProbabilityInfo& BPI, BlockFrequencyInfo& BFI) {
    
    std::vector<std::vector<BasicBlock*>> traces;

    // Visited set 
    // Blocks already assigned to a trace
    std::set<BasicBlock*> visited;

    // BFS order used for back edge detection
    // A successor that appears before current block in BFS order is a back edge 
    std::vector<BasicBlock*> bfsOrder;
    bfsOrder.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < bfsOrder.size(); ++i)
        for (BasicBlock* succ : successors(bfsOrder[i]))
            if (std::find(bfsOrder.begin(), bfsOrder.end(), succ) == bfsOrder.end())
                bfsOrder.push_back(succ);

    // Seed — unvisited block with highest execution frequency
    // Pick seeds until all blocks are assigned to a trace
    while (true) {

        BasicBlock* seed = nullptr;
        uint64_t bestFreq = 0;

        for (BasicBlock* BB : bfsOrder) {
            if (visited.count(BB)) continue;
            uint64_t freq = BFI.getBlockFreq(BB).getFrequency();
            if (freq > bestFreq) {
                bestFreq = freq;
                seed = BB;
            }
        }

        // If no more unvisited blocks
        if (!seed) break;

        // Start a new trace with the seed found
        std::vector<BasicBlock*> trace;
        trace.push_back(seed);
        visited.insert(seed);

        // Grow trace forward
        // Follow best successor until no valid successor found
        // Stops at --- back edges, low probability edges, visited blocks
        BasicBlock* current = seed;
        while (true) {
            BasicBlock* next = bestSuccessor(current, BPI, visited, bfsOrder);
            if (!next) break;
            trace.push_back(next);
            visited.insert(next);
            current = next;
        }

        // Grow trace backward from seed
        // Follow best predecessor until no valid predecessor found
        current = seed;
        while (true) {
            BasicBlock* prev = bestPredecessor(current, BPI, visited, bfsOrder);
            if (!prev) break;
            trace.insert(trace.begin(), prev);
            visited.insert(prev);
            current = prev;
        }

        // Trace complete
        traces.push_back(trace);
    }

    return traces;
}