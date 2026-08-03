#include "schedulerPass.hpp"
#include "reaching.hpp"
#include "liveness.hpp"
#include "traceIdentification.hpp"
#include "ddg.hpp"
#include "critical.hpp"
#include "listScheduler.hpp"
#include "applySchedule.hpp"
#include "localScheduler.hpp"

using namespace llvm; 


struct globalSchedulerPass: PassInfoMixin<globalSchedulerPass> {

    // Should the pass run with or without reg pressure analysis
    bool usePressure;
    globalSchedulerPass(bool pressure) {
        usePressure = pressure;
    }
    
    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) {
        outs() << "globalSchedulerPass running on: ";
        F.printAsOperand(outs(), false);
        outs() << " | Register Pressure: " << (usePressure ? "enabled" : "disabled") << "\n";
        outs() << "\n";

        // get liveness results
        auto livenessResult = computeLiveness(F);
        
        // get reaching definitions results
        // Not used anywhere, has initial plans of using but it was not required
        auto reachingResult = computeReaching(F);

        // after profiling the benchmark program
        // get branch probability and block frequency for trace identification
        // Using LLVM apis for that
        // BranchProbabilityInfo reads branch weights from benchmark.ll
        // BFI --- Gives execution frequency per block, used to pick seed
        // BPI --- Gives probability per edge, used to pick best successor/predecessor
        auto& BPI = FAM.getResult<BranchProbabilityAnalysis>(F);
        auto& BFI = FAM.getResult<BlockFrequencyAnalysis>(F);
        
        // identify traces using profile data
        auto traces = identifyTraces(F, BPI, BFI);

        // build DDG for each trace
        auto ddgs = buildDDG(traces, livenessResult, reachingResult);

        // Use LLVM's canonical legality analyses.  The pass never changes the
        // CFG, so they remain structurally valid while instructions move.
        auto& DT = FAM.getResult<DominatorTreeAnalysis>(F);
        auto& AC = FAM.getResult<AssumptionAnalysis>(F);
        auto& TLI = FAM.getResult<TargetLibraryAnalysis>(F);

        // Compute critical path for each trace DDG
        for (DDG* ddg : ddgs) {
            computeCriticalPath(ddg);
        }

        // Build universe for liveness (args + non void instructions)
        // For knowing register pressure
        std::vector<Value*> universe;
        for (auto& arg : F.args()){
            universe.push_back(&arg);
        }
        for (auto& BB : F){
            for (auto& I : BB){
                if (!I.getType()->isVoidTy())
                universe.push_back(&I);
            }
        }

        // Get loop info
        // Pass to applySchedule
        auto& LI = FAM.getResult<LoopAnalysis>(F);

        // SCHED_STATS instrumentation: hoist stats (applySchedule) and peak
        // live pressure (listSchedule) are accumulated across every trace of
        // this function, so reset once before the loop and emit once after —
        // no-ops when SCHED_STATS is unset.
        resetHoistStats();
        resetPeakLivePressure();

        // Run list scheduling for each trace
        // List scheduler produces globally optimal order
        // applySchedule physically moves instructions
        for (size_t i = 0; i < ddgs.size(); ++i) {
            // Initialize live set from liveness.in of first block in trace
            BasicBlock* firstBB = traces[i][0];
            BitVector currentLive = livenessResult[firstBB].in;

            // Run list scheduler
            // Returns globally ordered instruction list
            auto schedule = listSchedule(ddgs[i], currentLive, universe, livenessResult,  usePressure);

            // Apply schedule
            // Phase 1 — reorder within blocks
            // Phase 2 — cross block movement
            applySchedule(schedule, traces[i], DT, AC, TLI, LI, universe,
                          livenessResult, usePressure);
        }

        emitHoistStats();
        emitPeakLivePressure();

        for (DDG* ddg : ddgs)
            delete ddg;

        return PreservedAnalyses::none();
    }
};


struct localSchedulerPass : PassInfoMixin<localSchedulerPass> {
    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) {
        outs() << "localSchedulerPass running on: ";
        F.printAsOperand(outs(), false);
        outs() << "\n";
        outs() << "\n";

        // Liveness analysis for register pressure
        auto livenessResult = computeLiveness(F);

        // Build universe — same as global scheduler
        std::vector<Value*> universe;
        for (auto& arg : F.args())
            universe.push_back(&arg);
        for (auto& BB : F)
            for (auto& I : BB)
                if (!I.getType()->isVoidTy())
                    universe.push_back(&I);

        // SCHED_STATS instrumentation: listSchedule() is called once per
        // basic block here, so accumulate peak live pressure across all of
        // them and emit one line for the function. No-op when unset.
        resetPeakLivePressure();

        // Run local scheduler
        runLocalScheduler(F, livenessResult, universe);

        emitPeakLivePressure();

        return PreservedAnalyses::none();
    }
};



extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "UnifiedPass", "v1", [](PassBuilder& PB) {
        PB.registerPipelineParsingCallback([](StringRef Name, FunctionPassManager& FPM,
            ArrayRef<PassBuilder::PipelineElement>) -> bool {

                    if (Name == "globalSchedulerPass") {
                        FPM.addPass(globalSchedulerPass(true));
                        return true;
                    }

                    if (Name == "localSchedulerPass") {
                        FPM.addPass(localSchedulerPass());
                        return true;
                    }

                    if (Name == "globalSchedulerPassNoPressure") {
                        FPM.addPass(globalSchedulerPass(false));
                        return true;
                    }

                    return false;
                });
        }};
}
