// Reused this code from Homework with minor changes

#pragma once

#include "schedulerPass.hpp"

using namespace llvm; 

//Block state struct with all the sets for reaching definitions
struct BlockStateReaching {
    BitVector in, out, gen, kill;
};

DenseMap<const BasicBlock*, BlockStateReaching>
computeReaching(Function& F) {

    // Meet operator --- Union of all predecessor OUT sets
    // Return: BitVector
    auto meetUnion = [](const std::vector<BitVector>& ins) -> BitVector {
        if (ins.empty()) return {};
        BitVector out = ins[0];
        for (size_t i = 1; i < ins.size(); ++i) out |= ins[i];
        return out;
    };

    // Build universe --- Each bit is a definiton that produces a value (non-void)
    std::vector<Instruction*> universe;
    for (auto& BB : F)
        for (auto& I : BB)
        // only definiton that produces a value (non-void)
        if (!I.getType()->isVoidTy())   
            universe.push_back(&I);

    // BFS traversal of CFG
    // Gets the processing order for the worklist loop
    std::vector<BasicBlock*> order;
    order.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < order.size(); ++i)
        for (BasicBlock* succ : successors(order[i]))
        if (std::find(order.begin(), order.end(), succ) == order.end())
            order.push_back(succ);

    // Initialize states and compute gen, kill per block
    DenseMap<const BasicBlock*, BlockStateReaching> st;
    for (BasicBlock* BB : order) {
        BlockStateReaching bs;
        
        // Initialize --- OUT[B] = empty set (all zeros)
        // Everything starts as empty (all zeros)
        bs.in   = BitVector(universe.size(), false);
        bs.out  = BitVector(universe.size(), false);
        bs.gen  = BitVector(universe.size(), false);
        bs.kill = BitVector(universe.size(), false);

        for (Instruction& I : *BB) {
        // skips void instructions
        if (I.getType()->isVoidTy()) continue;

        // Find this definition's index in the universe
        auto it = std::find(universe.begin(), universe.end(), &I);
        if (it == universe.end()) continue;
        unsigned defIdx = static_cast<unsigned>(it - universe.begin());
        
        // GEN --- this block produces this definition
        // This bit is set
        bs.gen.set(defIdx);

        // KILL --- find all other definitions of the same variable and kill them
        for (size_t i = 0; i < universe.size(); ++i) {
            // for not killing the same definition
            if (i == defIdx) continue;                  
            if (!I.getName().empty() && universe[i]->getName() == I.getName())
            bs.kill.set(static_cast<unsigned>(i));
        }
        }

        st[BB] = bs;
    }

    // Running the analysis until the fixed point is reached
    // Keep iterating until no block's OUT changes
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock* BB : order) {

        // IN[B] = Union of OUT[P] for all predecessors
        // OUT[ENTRY] = empty set
        std::vector<BitVector> predOuts;
        for (BasicBlock* pred : predecessors(BB))
            predOuts.push_back(st[pred].out);
        if (predOuts.empty())      
            // entry block                 
            // empty set        
            predOuts.push_back(BitVector(universe.size(), false)); 
        
        // Calling the meet operator to calculate IN[B]
        st[BB].in = meetUnion(predOuts);

        // Transfer Function ---- OUT[B] = gen_B U (IN[B] - kill_B)
        // IN[B] - kill_B
        BitVector inMinusKill = st[BB].in;
        BitVector killCopy    = st[BB].kill;
        inMinusKill &= killCopy.flip();     

        // gen_B U (IN - kill_B)
        BitVector newOut = st[BB].gen;
        newOut |= inMinusKill;

        // Check for fixed point
        if (newOut != st[BB].out) {
            st[BB].out = newOut;
            changed = true;
        }
        }
    }

    return st;  
}