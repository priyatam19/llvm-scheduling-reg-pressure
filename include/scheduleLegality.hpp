#pragma once

#include "schedulerPass.hpp"
#include <llvm/ADT/SmallPtrSet.h>

using namespace llvm;

// Instructions in this category are fixed scheduling boundaries.  Keeping
// them in their original positions makes the conservative scheduler correct
// without requiring AliasAnalysis or MemorySSA.
static bool isSchedulingAnchor(const Instruction *I) {
    return isa<PHINode>(I) || I->isTerminator() || isa<AllocaInst>(I) ||
           isa<CallBase>(I) || I->isEHPad() || I->getType()->isTokenTy() ||
           I->mayReadOrWriteMemory() || I->mayHaveSideEffects();
}

// Reorder only maximal runs of non-anchor instructions.  Every anchor stays
// at exactly the same point relative to the surrounding regions.  If the
// global schedule is incomplete for a region, leave that region untouched.
static void reorderBlockWithinRegions(const std::vector<Instruction *> &schedule,
                                      BasicBlock *BB) {
    std::vector<std::vector<Instruction *>> regions;
    std::vector<Instruction *> boundaries;
    std::vector<Instruction *> current;

    for (Instruction &I : *BB) {
        if (!isSchedulingAnchor(&I)) {
            current.push_back(&I);
            continue;
        }

        if (!current.empty()) {
            regions.push_back(current);
            boundaries.push_back(&I);
            current.clear();
        }
    }

    // Every well-formed block ends in a terminator, which is an anchor.
    // Retain a defensive fallback for malformed/intermediate IR.
    if (!current.empty()) {
        regions.push_back(current);
        boundaries.push_back(nullptr);
    }

    for (size_t regionIdx = 0; regionIdx < regions.size(); ++regionIdx) {
        const std::vector<Instruction *> &region = regions[regionIdx];
        SmallPtrSet<Instruction *, 16> members(region.begin(), region.end());
        SmallPtrSet<Instruction *, 16> seen;
        std::vector<Instruction *> ordered;

        for (Instruction *I : schedule) {
            if (members.contains(I) && seen.insert(I).second)
                ordered.push_back(I);
        }

        if (ordered.size() != region.size())
            continue;

        Instruction *boundary = boundaries[regionIdx];
        if (!boundary)
            continue;

        for (Instruction *I : ordered)
            I->moveBefore(boundary->getIterator());
    }
}
