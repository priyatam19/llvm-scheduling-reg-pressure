#pragma once

#include "schedulerPass.hpp"

using namespace llvm;

// Computes the critical path length for each node in the DDG.
// The critical path of a node is the length of the longest
// latency-weighted path from that node to any sink in the DDG.

// This value determines scheduling urgency, nodes with higher
// critical path must be scheduled earlier to avoid delaying
// the entire computation. Used as the primary priority in
// the list scheduler.

// Standard DFS based topological sort
// Process nodes in reverse topological order
// so that when we compute critical path of node N
// all of N's successors already have their cp computed
static void topoSortDFS(DDGNode* node, std::set<DDGNode*>& visited,
        std::vector<DDGNode*>& result) {
    
    visited.insert(node);

    // Visit all successors first
    for (DDGNode* succ : node->successors) {
        if (!visited.count(succ))
            topoSortDFS(succ, visited, result);
    }

    // Add to result after all successors processed
    result.push_back(node);
}

// Returns nodes in topological order
// Uses DFS post-order then reverses to get correct ordering
static std::vector<DDGNode*> topologicalSort(DDG* ddg) {

    std::set<DDGNode*> visited;
    std::vector<DDGNode*> result;

    // Visit all nodes
    for (DDGNode* node : ddg->nodes) {
        if (!visited.count(node))
            topoSortDFS(node, visited, result);
    }

    // result is now in reverse topological order
    // Want sourcs first for critical path
    // so reverse it
    std::reverse(result.begin(), result.end());

    return result;
}



// Critical Path Calculation
// The length of the longest path from a node to
// the end of the graph determines how urgently
// the node must be scheduled
//
// Algorithm:
//   Process nodes in REVERSE topological order
//   for each node N:
//       if N has no successors:
//           N.criticalPath = N.delay
//       else:
//           N.criticalPath = N.delay + max(S.criticalPath)
//                            for all successors S
static void computeCriticalPath(DDG* ddg) {

    // Topological order
    // Reverse topological order for critical path
    // so process from sinks to sources
    std::vector<DDGNode*> topoOrder = topologicalSort(ddg);

    // Process in reverse topological order
    for (auto it = topoOrder.rbegin(); it != topoOrder.rend(); ++it) {
        
        DDGNode* N = *it;

        // Sink node --- critical path is just its own latency
        if (N->successors.empty()) {
            N->criticalPath = N->delay;
            continue;
        }

        // Non-sink --- critical path is own latency plus longest successor path
        int maxSuccCP = 0;
        for (DDGNode* succ : N->successors) {
            if (succ->criticalPath > maxSuccCP)
                maxSuccCP = succ->criticalPath;
        }
        N->criticalPath = N->delay + maxSuccCP;
    }
}