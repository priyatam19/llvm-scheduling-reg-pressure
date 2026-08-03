// Reused this code from Homework with minor changes

#pragma once

#include "schedulerPass.hpp"

using namespace llvm;

// Dominator Computation Function
struct DomInfo {
    std::vector<BasicBlock*> order;
    DenseMap<const BasicBlock*, unsigned> idx;
    DenseMap<const BasicBlock*, BitVector> dom;
    DenseMap<const BasicBlock*, BasicBlock*> idom;
};

// Dominator computation function
// Returns DomInfo struct containing BFS order, block indices, dominator sets, and immediate dominators
static DomInfo computeDominators(Function& F) {
    DomInfo info;

    // BFS traversal of CFG
    // Start from entry block and visit all reachable blocks in BFS order
    info.order.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < info.order.size(); ++i)
    for (BasicBlock* succ : successors(info.order[i]))
    // Only add blocks that are not yet visited
        if (std::find(info.order.begin(), info.order.end(), succ) == info.order.end())
        info.order.push_back(succ);

    // Map each basic block to its index in BFS order
    // Used forblock lookup in a BitVector
    unsigned N = info.order.size();
    for (unsigned i = 0; i < N; ++i) info.idx[info.order[i]] = i;

    // Initializing dominator sets
    // DOM[entry] = {entry} 
    // DOM[others] = T (top element)
    for (unsigned i = 0; i < N; ++i)
    info.dom[info.order[i]] = BitVector(N, true);
    info.dom[info.order[0]].reset();
    info.dom[info.order[0]].set(0);

    // Transfer function: DOM[b] = {b} U (∩ DOM[p] for all predecessors p)
    // Meet operator: intersection (∩)
    // Iterate until no DOM set changes 
    bool changed = true;
    while (changed) {
    changed = false;
    for (unsigned i = 1; i < N; ++i) {
        BasicBlock* BB = info.order[i];
        BitVector newDom(N, true);
        bool hasPred = false;
        for (BasicBlock* pred : predecessors(BB)) {
        auto it = info.idx.find(pred);
        if (it != info.idx.end()) {
            newDom &= info.dom[pred];
            hasPred = true;
        }
        }
        if (!hasPred) newDom.reset();
        newDom.set(i);
        if (newDom != info.dom[BB]) {
        info.dom[BB] = newDom;
        changed = true;
        }
    }
}

    // idom is used in the dominators pass to print the immediate dominator of each block
    // Compute idom
    // idom(b) = the closest strict immediate dominator of b
    // entry has no immediate dominator
    info.idom[info.order[0]] = nullptr;
    for (unsigned i = 1; i < N; ++i) {
    BasicBlock* BB = info.order[i];
    BitVector strictDom = info.dom[BB];
    strictDom.reset(i);
    BasicBlock* immDom = nullptr;
    for (unsigned d : strictDom.set_bits()) {
        // d is the idom if no other strict dominator d2 also dominates d
        // d is the deepest node in the dominator tree that still dominates b
        bool dominated_by_another = false;
        for (unsigned d2 : strictDom.set_bits()) {
        if (d2 != d && info.dom[info.order[d2]].test(d)) {
            // d2 is also a strict dominator and dominates d
            // d2 is closer to b than d then d is not the idom
            dominated_by_another = true;
            break;
        }
        }
        if (!dominated_by_another) {
        immDom = info.order[d];
        break;
        }
    }
    info.idom[BB] = immDom;
    }

    return info;
}