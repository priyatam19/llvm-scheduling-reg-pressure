// This file contains all standard library headers 
// and LLVM headers required for the project

#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>
#include <set>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/BranchProbabilityInfo.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/IR/Verifier.h>

// universe (args + non-void instructions, built once per function in
// schedulerPass.cpp) fixes the bit ordering shared by every BitVector this
// pass touches -- computeLiveness's own internal universe uses the same
// enumeration, so index i here must mean the same value as bit i there.
// This map is purely a lookup accelerator over that fixed ordering: every
// pressure/liveness site used to do a linear std::find(universe, V) per
// operand, which made scheduling cost O(instructions-in-function) per
// operand touched -- quadratic-ish over a whole function. Building this
// once and looking values up in O(1) doesn't change which index any value
// gets, so it can't change any scheduling decision.
static llvm::DenseMap<llvm::Value*, unsigned> buildValueIndex(std::vector<llvm::Value*>& universe) {
    llvm::DenseMap<llvm::Value*, unsigned> index;
    index.reserve(universe.size());
    for (unsigned i = 0; i < universe.size(); ++i)
        index[universe[i]] = i;
    return index;
}
