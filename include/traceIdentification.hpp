#pragma once

#include "schedulerPass.hpp"
#include <cstdlib>
#include <map>

using namespace llvm;

// Trace Identification using profile data
// Identifies hot execution traces through the CFG using
// branch probabilities (BPI) and block frequencies (BFI).

// Threshold for choosing best Successor and Predecessor - default 60%
// Overridable via env var SCHED_TRACE_THRESHOLD_PCT (integer percent, e.g. 60)
// Read once via a function-local static so a sensitivity sweep can vary it
// without recompiling the pass shared object.
static BranchProbability getTraceThreshold() {
    static BranchProbability threshold = [] {
        int pct = 60;
        if (const char* env = std::getenv("SCHED_TRACE_THRESHOLD_PCT")) {
            int parsed = std::atoi(env);
            if (parsed > 0 && parsed <= 100)
                pct = parsed;
        }
        return BranchProbability(pct, 100);
    }();
    return threshold;
}

// SCHED_STATS=1 opt-in instrumentation toggle, read once.
static bool schedStatsEnabled() {
    static bool enabled = [] {
        const char* env = std::getenv("SCHED_STATS");
        return env && std::string(env) == "1";
    }();
    return enabled;
}

// Per-function counters for trace identification, only touched when
// schedStatsEnabled() is true.
struct TraceIdStats {
    unsigned edgesAboveThreshold = 0;
    unsigned edgesBelowThreshold = 0;
    unsigned backedgesSkipped    = 0;
};

// Global (translation-unit local) stats instance used across the
// bestSuccessor/bestPredecessor/identifyTraces calls for the function
// currently being processed. Reset at the start of identifyTraces().
static TraceIdStats g_traceIdStats;

// BFS order position of BB, or bfsIndex.size() ("after everything") if BB
// was never reached by the forward BFS from the entry block -- e.g. an
// otherwise-unreachable block that still has an edge into a reachable one.
// Mirrors std::find(bfsOrder, BB) == bfsOrder.end() exactly, so swapping the
// old O(n) std::find-based back-edge check for this DenseMap lookup can't
// change which edges are classified as back edges.
static unsigned bfsPos(DenseMap<BasicBlock*, unsigned>& bfsIndex, BasicBlock* BB) {
    auto it = bfsIndex.find(BB);
    return it != bfsIndex.end() ? it->second : (unsigned)bfsIndex.size();
}

// Grow trace forward from seed
// Returns the best successor of BB to extend the trace forward
// Valid successor must --- not be visited, exceed threshold probability,
// and not be a loop back edge
static BasicBlock* bestSuccessor(BasicBlock* BB, BranchProbabilityInfo& BPI,
        std::set<BasicBlock*>& visited, DenseMap<BasicBlock*, unsigned>& bfsIndex) {

    BasicBlock* best    = nullptr;
    BranchProbability bestProb = BranchProbability::getZero();

    for (BasicBlock* succ : successors(BB)) {

        // Skip if already in a trace
        if (visited.count(succ)) continue;

        // Get probability of edge BB --> succ
        BranchProbability prob = BPI.getEdgeProbability(BB, succ);

        // Stop if probability is below threshold (default 60%)
        if (prob <= getTraceThreshold()) {
            if (schedStatsEnabled()) g_traceIdStats.edgesBelowThreshold++;
            continue;
        }
        if (schedStatsEnabled()) g_traceIdStats.edgesAboveThreshold++;

        // Back edge detection —-- if successor appears before
        // current block in BFS order it is a back edge
        // Back edges close loops so stops trace here
        // Skips back edge
        if (bfsPos(bfsIndex, succ) < bfsPos(bfsIndex, BB)) {
            if (schedStatsEnabled()) g_traceIdStats.backedgesSkipped++;
            continue;
        }

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
        std::set<BasicBlock*>& visited, DenseMap<BasicBlock*, unsigned>& bfsIndex) {

    BasicBlock* best    = nullptr;
    BranchProbability bestProb = BranchProbability::getZero();

    for (BasicBlock* pred : predecessors(BB)) {

        // Skip if already in a trace
        if (visited.count(pred)) continue;

        // Get probability of edge pred --> BB
        BranchProbability prob = BPI.getEdgeProbability(pred, BB);

        // Stop if probability is below threshold (default 60%)
        if (prob <= getTraceThreshold()) {
            if (schedStatsEnabled()) g_traceIdStats.edgesBelowThreshold++;
            continue;
        }
        if (schedStatsEnabled()) g_traceIdStats.edgesAboveThreshold++;

        // Back edge detection - if predecessor appears after
        // current block in BFS order it is a back edge
        // Skips back edge
        if (bfsPos(bfsIndex, pred) > bfsPos(bfsIndex, BB)) {
            if (schedStatsEnabled()) g_traceIdStats.backedgesSkipped++;
            continue;
        }

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

    // Reset per-function stats before extending any traces for this function
    if (schedStatsEnabled()) g_traceIdStats = TraceIdStats();

    // Visited set
    // Blocks already assigned to a trace
    std::set<BasicBlock*> visited;

    // BFS order used for back edge detection
    // A successor that appears before current block in BFS order is a back edge
    // bfsIndex mirrors bfsOrder's positions in O(1)-lookup form: bestSuccessor/
    // bestPredecessor used to std::find() into bfsOrder per edge examined,
    // which is O(n) per edge (O(n^2) or worse per function); membership below
    // (`bfsIndex.count(succ)`) replaces the same std::find pattern used during
    // BFS construction itself.
    std::vector<BasicBlock*> bfsOrder;
    DenseMap<BasicBlock*, unsigned> bfsIndex;
    bfsOrder.push_back(&F.getEntryBlock());
    bfsIndex[&F.getEntryBlock()] = 0;
    for (size_t i = 0; i < bfsOrder.size(); ++i)
        for (BasicBlock* succ : successors(bfsOrder[i]))
            if (!bfsIndex.count(succ)) {
                bfsIndex[succ] = bfsOrder.size();
                bfsOrder.push_back(succ);
            }

    // Seed selection picks the unvisited block with the highest execution
    // frequency, once per trace formed. The original rescanned all of
    // bfsOrder from scratch for every trace (O(n) per trace, so O(n^2) over
    // a function with many small traces). Block frequencies don't change as
    // traces are grown, so sorting once up front and walking a
    // monotonically-advancing cursor past already-visited entries visits
    // each block at most once in total across every trace, while picking
    // the exact same block every time: stable_sort keeps entries tied on
    // frequency in their original bfsOrder order, which is exactly the
    // "first encountered in bfsOrder wins ties" behavior the original
    // strict `freq > bestFreq` scan had.
    std::vector<BasicBlock*> byFreqDesc = bfsOrder;
    std::stable_sort(byFreqDesc.begin(), byFreqDesc.end(), [&](BasicBlock* a, BasicBlock* b) {
        return BFI.getBlockFreq(a).getFrequency() > BFI.getBlockFreq(b).getFrequency();
    });
    size_t seedCursor = 0;

    // Pick seeds until all blocks are assigned to a trace
    while (true) {

        while (seedCursor < byFreqDesc.size() && visited.count(byFreqDesc[seedCursor]))
            ++seedCursor;
        BasicBlock* seed = seedCursor < byFreqDesc.size() ? byFreqDesc[seedCursor] : nullptr;

        // A zero-frequency block is never picked as a seed, matching the
        // original's `freq > bestFreq` with bestFreq starting at 0.
        if (seed && BFI.getBlockFreq(seed).getFrequency() == 0)
            seed = nullptr;

        // If no more unvisited (nonzero-frequency) blocks
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
            BasicBlock* next = bestSuccessor(current, BPI, visited, bfsIndex);
            if (!next) break;
            trace.push_back(next);
            visited.insert(next);
            current = next;
        }

        // Grow trace backward from seed
        // Follow best predecessor until no valid predecessor found
        current = seed;
        while (true) {
            BasicBlock* prev = bestPredecessor(current, BPI, visited, bfsIndex);
            if (!prev) break;
            trace.insert(trace.begin(), prev);
            visited.insert(prev);
            current = prev;
        }

        // Trace complete
        traces.push_back(trace);
    }

    // Emit end-of-function trace identification stats
    if (schedStatsEnabled()) {
        std::map<size_t, unsigned> lenHist;
        for (auto& trace : traces)
            lenHist[trace.size()]++;

        errs() << "SCHED_STATS trace_count=" << traces.size() << "\n";

        errs() << "SCHED_STATS trace_len_hist=";
        bool first = true;
        for (auto& kv : lenHist) {
            if (!first) errs() << ",";
            errs() << kv.first << ":" << kv.second;
            first = false;
        }
        errs() << "\n";

        errs() << "SCHED_STATS edges_above_threshold=" << g_traceIdStats.edgesAboveThreshold
               << " edges_below_threshold=" << g_traceIdStats.edgesBelowThreshold
               << " backedges_skipped=" << g_traceIdStats.backedgesSkipped << "\n";
    }

    return traces;
}