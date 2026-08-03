// Reused this code from Homework with minor changes

#pragma once

#include "schedulerPass.hpp"

using namespace llvm; 

// Block state struct with all the sets for liveness analysis
struct BlockStateLiveness {
    BitVector in, out, use, def;
};

DenseMap<const BasicBlock*, BlockStateLiveness>
computeLiveness(Function& F) {

    // Build the universe for all variables (args + instructions)
    std::vector<Value*> universe;
    for (auto& arg : F.args())
    universe.push_back(&arg);
    for (auto& BB : F)
    for (auto& I : BB)
        if (!I.getType()->isVoidTy()) universe.push_back(&I);
    unsigned N = universe.size();
    
    // Collect all the Basic Blocks
    std::vector<BasicBlock*> allBBs;
    for (auto& BB : F) allBBs.push_back(&BB);

    // From top to bottom, Compute Use and Def sets per block
    DenseMap<const BasicBlock*, BlockStateLiveness> st;
    

    for (BasicBlock* BB : allBBs) {

    // Initialize all sets to empty for this block
    BlockStateLiveness bs;
    bs.in  = BitVector(N, false);
    bs.out = BitVector(N, false);
    bs.use = BitVector(N, false);
    bs.def = BitVector(N, false);

    for (Instruction& I : *BB) {
        for (Use& U : I.operands()) {
        Value* V = U.get();
        auto it = std::find(universe.begin(), universe.end(), V);
        if (it != universe.end()) {
            unsigned idx = it - universe.begin();
            if (!bs.def.test(idx))
            bs.use.set(idx);
        }
        }
        auto it = std::find(universe.begin(), universe.end(), (Value*)&I);
        if (it != universe.end())
        bs.def.set(it - universe.begin());
    }

    //Save the computed use/def sets for this block
    st[BB] = bs;
    }

    // Backward fixed-point iteration
    // OUT[B] = union of IN[sucessors]
    // IN[B] = use[B] union (OUT[B] - DEF[B])
    bool changed = true;
    while (changed) {
    changed = false;
    for (auto it = allBBs.rbegin(); it != allBBs.rend(); ++it) {
        BasicBlock* BB = *it;

        // Meet: union of successor IN sets
        BitVector newOut(N, false);
        for (BasicBlock* succ : successors(BB))
        newOut |= st[succ].in;

        // Transfer function
        BitVector newIn = st[BB].use;
        BitVector prop  = newOut;
        // OUT - def
        prop.reset(st[BB].def);
        newIn |= prop;

        if (newIn != st[BB].in || newOut != st[BB].out) {
        st[BB].in  = newIn;
        st[BB].out = newOut;
        changed = true;
        }
    }
    }
    // Fixed point reached

    return st;  
}