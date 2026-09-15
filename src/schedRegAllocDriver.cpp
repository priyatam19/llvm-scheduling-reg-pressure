// Minimal llc-equivalent driver: exists solely because llc has no runtime
// plugin mechanism for register allocators (opt's -load-pass-plugin has no
// counterpart for -regalloc=; RegisterRegAlloc in
// llvm/CodeGen/RegAllocRegistry.h is a static, link-time registry). Any
// out-of-tree allocator must be linked into its own driver binary for its
// `static RegisterRegAlloc` registration to run.
//
// This intentionally does not reimplement llc's pipeline: it reuses
// llvm/CodeGen/CommandFlags.h (the same flag-parsing code llc.cpp itself
// uses) so -march=, -mcpu=, -mattr=, -regalloc=, -filetype=,
// -verify-machineinstrs, etc. all resolve identically to stock llc, and
// calls TargetMachine::addPassesToEmitFile -- the same high-level entry
// point llc uses -- so pass ordering, coalescing, and rewriting come from
// LLVM's own TargetPassConfig, not from anything reimplemented here.

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

// Phase 2 adds: #include "chaitinBriggsRegAlloc.hpp" -- deliberately not
// included yet. Phase 1's only goal is proving this driver is a faithful
// llc-equivalent for LLVM's *existing* allocators (-regalloc=greedy/fast)
// before any new allocator code exists, so later bugs are attributable to
// the allocator, not the harness.

using namespace llvm;

// Registers -march=, -mcpu=, -mattr=, -relocation-model=, -code-model=,
// -filetype=, -verify-machineinstrs, and (via already-linked
// TargetPassConfig.cpp) -regalloc=.
static codegen::RegisterCodeGenFlags CGF;

static cl::opt<std::string> InputFilename(cl::Positional,
                                           cl::desc("<input .ll/.bc file>"),
                                           cl::init("-"));

static cl::opt<std::string> OutputFilename("o", cl::desc("output filename"),
                                            cl::init("-"));

// Not part of CommandFlags.h -- llc.cpp defines this one itself (default
// true), so we match it explicitly rather than silently diverging on
// whether asm output carries `# %block_name` / `# @func` comments.
static cl::opt<bool> AsmVerbose("asm-verbose",
                                 cl::desc("Add comments to directives"),
                                 cl::init(true));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // Target-scoped init: only riscv64 is in scope for this project.
  LLVMInitializeRISCVTargetInfo();
  LLVMInitializeRISCVTarget();
  LLVMInitializeRISCVTargetMC();
  LLVMInitializeRISCVAsmPrinter();
  LLVMInitializeRISCVAsmParser();

  cl::ParseCommandLineOptions(argc, argv,
                               "out-of-tree register allocator driver\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  Triple TheTriple(M->getTargetTriple());
  if (TheTriple.getTriple().empty())
    TheTriple = Triple("riscv64-unknown-linux-gnu");

  std::string Error;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(codegen::getMArch(), TheTriple, Error);
  if (!TheTarget) {
    errs() << argv[0] << ": " << Error << "\n";
    return 1;
  }

  TargetOptions Options = codegen::InitTargetOptionsFromCodeGenFlags(TheTriple);
  Options.MCOptions.AsmVerbose = AsmVerbose;
  Options.MCOptions.PreserveAsmComments = AsmVerbose;
  std::string CPUStr = codegen::getCPUStr();
  std::string FeaturesStr = codegen::getFeaturesStr();

  std::unique_ptr<TargetMachine> Target(TheTarget->createTargetMachine(
      TheTriple, CPUStr, FeaturesStr, Options,
      codegen::getExplicitRelocModel(), codegen::getExplicitCodeModel(),
      CodeGenOptLevel::Default));
  if (!Target) {
    errs() << argv[0] << ": could not allocate target machine\n";
    return 1;
  }

  M->setDataLayout(Target->createDataLayout());

  std::error_code EC;
  sys::fs::OpenFlags OpenFlags = sys::fs::OF_None;
  if (codegen::getFileType() != CodeGenFileType::ObjectFile)
    OpenFlags |= sys::fs::OF_Text;
  auto Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, OpenFlags);
  if (EC) {
    errs() << argv[0] << ": " << EC.message() << "\n";
    return 1;
  }

  legacy::PassManager PM;
  TargetLibraryInfoImpl TLII(TheTriple);
  PM.add(new TargetLibraryInfoWrapperPass(TLII));

  if (Target->addPassesToEmitFile(PM, Out->os(), nullptr,
                                   codegen::getFileType())) {
    errs() << argv[0] << ": target does not support generation of this file type\n";
    return 1;
  }

  PM.run(*M);
  Out->keep();
  return 0;
}
