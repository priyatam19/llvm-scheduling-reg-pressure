#pragma once

#include "schedulerPass.hpp"

using namespace llvm;

// Builds one DDG per trace to capture scheduling constraints.
// The DDG is a directed acyclic graph where:
//   - Each node represents one instruction
//   - Each edge A -- B means A must execute before B

// Three types of dependences are captured:
//   1. True dependences (RAW) —-- B uses value produced by A
//   2. Anti dependences (WAR) —-- B writes value read by A
//   3. Output dependences (WAW) —-- both A and B write same location

// Anti and output dependences are not eliminated in our code
// However they are kept as ordering constraints 

// Each DDG Node represents one instruction
// Each node has an operation type and a delay
// delay = number of cycles the operation takes
struct DDGNode {
    // the instruction this node represents
    Instruction* instr;
    // delay — cycles required to complete that instruction
    int delay;        
    // longest path to end of DDG
    int criticalPath; 
    // number of unscheduled predecessors
    // when predCount == 0 node enters ready queue
    int predCount;   

    // nodes that use result from this instruction
    std::vector<DDGNode*> successors;   
    // nodes whose result this instruction uses
    std::vector<DDGNode*> predecessors; 
};


// DDG —-- Data Dependence Graph for one trace
// D = (N, E)
// N = one node per operation
// E = edges encoding true dependences
struct DDG {

    // all nodes in this trace's DDG
    std::vector<DDGNode*> nodes;  

    // Leaves are nodes with no predecessors
    // These go into the ready queue first
    // They can be scheduled immediately
    std::vector<DDGNode*> leaves() const {
        std::vector<DDGNode*> result;
        for (DDGNode* N : nodes)
            if (N->predecessors.empty())
                result.push_back(N);
        return result;
    }

    // Roots are nodes with no successors
    // Cannot execute until all ancestors done
    std::vector<DDGNode*> roots() const {
        std::vector<DDGNode*> result;
        for (DDGNode* N : nodes)
            if (N->successors.empty())
                result.push_back(N);
        return result;
    }

    // Destructor, cleans up allocated nodes
    ~DDG() {
        for (DDGNode* N : nodes)
            delete N;
    }
};


// Latency model for instructions similar to RISC-V
// LOAD = 3 cycles 
// MUL = 2 cycles 
// DIV = 10 cycles 
// CALL = 5 cycles 
// other = 1 cycle (add, sub, cmp, branch, etc)
static int getDelay(Instruction* I) {
    if (isa<LoadInst>(I))  return 3;
    if (isa<StoreInst>(I)) return 1;
    if (isa<CallInst>(I))  return 5;
    if (auto* BO = dyn_cast<BinaryOperator>(I)) {
        if (BO->getOpcode() == Instruction::Mul)  return 2;
        if (BO->getOpcode() == Instruction::UDiv) return 10;
        if (BO->getOpcode() == Instruction::SDiv) return 10;
    }
    return 1;
}


// adds a directed edge from src to dst in DDG
// src must execute before dst
static void addEdge(DDGNode* src, DDGNode* dst) {

    // To avoid duplicate edges
    for (DDGNode* s : src->successors)
        if (s == dst) return;

    src->successors.push_back(dst);
    dst->predecessors.push_back(src);
    dst->predCount++;
}


// builds one DDG per trace
// For each pair of instructions (A, B) in trace:
// if B uses result of A, add edge A ---> B (true dependence)
// True dependences are detected directly from SSA operands
// since every value has exactly one definition
// We don't optimize anti and output dependences
// Liveness analysis result is used for anti and output dependence detection
std::vector<DDG*> buildDDG(
    std::vector<std::vector<BasicBlock*>>& traces,
    DenseMap<const BasicBlock*, BlockStateLiveness>& livenessResult,
    DenseMap<const BasicBlock*, BlockStateReaching>& reachingResult){
    std::vector<DDG*> result;

    // Build one DDG per trace
    for (auto& trace : traces) {

        DDG* ddg = new DDG();

        // Map for instruction to its DDG node
        DenseMap<Instruction*, DDGNode*> instrToNode;

        // Create one node per instruction in trace
        // Every instruction in every block of the trace gets one DDG node
        for (BasicBlock* BB : trace) {
            for (Instruction& I : *BB) {

                if (isa<PHINode>(&I)) continue;
                DDGNode* node = new DDGNode();
                node->instr = &I;
                node->delay = getDelay(&I);
                node->criticalPath = 0;   
                node->predCount  = 0; 
                ddg->nodes.push_back(node);
                instrToNode[&I] = node;
            }
        }

        // Add true dependence edges
        // for each instruction B:
        //     for each operand V of B:
        //       which instruction A produced V?
        //          if A is in this trace, add edge A ---> B
        // Add true dependence edges
        for (BasicBlock* BB : trace) {
            for (Instruction& I : *BB) {

                // Skip PHI nodes — not in DDG
                if (isa<PHINode>(&I)) continue;
                // Skip terminators — not moveable
                if (I.isTerminator()) continue;

                DDGNode* dst = instrToNode.lookup(&I);
                if (!dst) continue; 

                for (Use& U : I.operands()) {
                    Value* V = U.get();
                    Instruction* defInstr = dyn_cast<Instruction>(V);
                    if (!defInstr) continue;

                    // Skip PHI nodes
                    if (isa<PHINode>(defInstr)) continue;

                    // Skip terminators
                    if (defInstr->isTerminator()) continue;

                    DDGNode* src = instrToNode.lookup(defInstr);
                    if (!src) continue;

                    addEdge(src, dst);
                }
            }
        }

        // Conservatively preserve original order for every instruction that
        // may observe or alter memory/program state, including calls.
        for (BasicBlock* BB : trace) {
            std::vector<Instruction*> orderedOps;
            for (Instruction& I : *BB) {
                if (isa<CallBase>(&I) || I.mayReadOrWriteMemory() || I.mayHaveSideEffects()) {
                    for (Instruction* prev : orderedOps) {
                        DDGNode* src = instrToNode.lookup(prev);
                        DDGNode* dst = instrToNode.lookup(&I);
                        if (src && dst)
                            addEdge(src, dst);
                    }
                    orderedOps.push_back(&I);
                }
            }
        }

        result.push_back(ddg);
    }

    return result;
}
