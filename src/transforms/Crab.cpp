#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Transforms/Utils/Local.h"

#include "transforms/Crab.h"
#include "clam/CfgBuilder.hh"
#include "clam/CfgBuilderParams.hh"
#include "clam/ClamAnalysisParams.hh"
#include "clam/Clam.hh"
#include "clam/RegisterAnalysis.hh"
#include "clam/crab/domains/intervals.hh"
#include "clam/SeaDsaHeapAbstraction.hh"
#include "clam/Support/Debug.hh"
#include "clam/Support/NameValues.hh"
#include "clam/Transforms/InsertInvariants.hh"

#include "crab/support/stats.hpp"

#include "seadsa/AllocWrapInfo.hh"
#include "seadsa/CompleteCallGraph.hh"
#include "seadsa/DsaLibFuncInfo.hh"
#include "seadsa/InitializePasses.hh"

static llvm::cl::opt<bool> OnlyMain(
    "Pcrab-only-main", 
    llvm::cl::desc("Analyze only a module if it contains main"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> PrintInvariants(
    "Pcrab-print-invariants", 
    llvm::cl::desc("Print invariants inferred by Crab"),
    llvm::cl::init(false));

namespace clam {
namespace CrabDomain {
constexpr Type OCCAM(1, "occam", "occam", false, false);  
} //end CrabDomain
// We create our own specialized abstract domain!  
using occam_domain_t = RGN_FUN(BOOL_NUM(BASE(interval_domain_t)));
REGISTER_DOMAIN(CrabDomain::OCCAM, occam_domain_t)    
} //end clam

namespace previrt {
namespace transforms {
  
using namespace llvm;
using namespace clam;

CrabPass::CrabPass(): ModulePass(ID) {}
  
bool CrabPass::runOnModule(Module &M) {
  if (M.empty()) return false;

  if (OnlyMain && !std::any_of(M.begin(), M.end(),
			       [](const Function &f) {
				 return f.getName() == "main";
			       })) {
    return false;
  }
  
  /// Create the CFG builder manager and all the Crab CFGs
  CrabBuilderParams builder_params;
  builder_params.precision_level = CrabBuilderPrecision::MEM;
  builder_params.simplify = true;
  builder_params.lower_singleton_aliases = true;
  builder_params.add_pointer_assumptions = false;
  auto &tli = getAnalysis<TargetLibraryInfoWrapperPass>();
  auto &cg = getAnalysis<seadsa::CompleteCallGraph>().getCompleteCallGraph();
  auto &allocWrapInfo = getAnalysis<seadsa::AllocWrapInfo>();
  // FIXME: if we pass "this" then allocWrapInfo can be more
  // precise because it can use LoopInfo. However, I get some
  // crash that I need to debug.
  allocWrapInfo.initialize(M, nullptr /*this*/);
  auto &dsaLibFuncInfo = getAnalysis<seadsa::DsaLibFuncInfo>();
  std::unique_ptr<HeapAbstraction> mem(new SeaDsaHeapAbstraction(
       M, cg, tli, allocWrapInfo, dsaLibFuncInfo, true /*context-sensitive*/));
  CrabBuilderManager builder_man(builder_params, tli, std::move(mem));
  
  /// Run the analysis
  AnalysisParams params;
  params.dom = CrabDomain::OCCAM;
  params.run_inter = true;
  params.max_calling_contexts = 9999999;
  params.analyze_recursive_functions = false;
  params.exact_summary_reuse = false;
  params.inter_entry_main = false;
  params.widening_delay = 2;
  params.narrowing_iters = 1;
  params.widening_jumpset = 0;
  params.stats = false;
  params.print_invars = PrintInvariants;
  // Needed to keep info needed by InsertInvariants
  params.store_invariants = true;
  std::unique_ptr<ClamGlobalAnalysis> ga(new InterGlobalClam(M, builder_man));
  typename ClamGlobalAnalysis::abs_dom_map_t abs_dom_assumptions /*no assumptions*/;    
  ga->analyze(params, abs_dom_assumptions);
  
  /// Optimize code using Crab invariants
  auto emptyFun = [](Function *F){ return nullptr;};
  InsertInvariants opt(*ga, &cg, emptyFun, emptyFun, InvariantsLocation::DEAD_CODE);
  bool res = opt.runOnModule(M);
  // Remove unreachable blocks
  if (res) {
    for (auto &F: M) {
      if (!F.empty()) {
	removeUnreachableBlocks(F);
      }
    }
  }
  return res;
}
  
void CrabPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  // dependency for immutable AllocWrapInfo
  //AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<seadsa::AllocWrapInfo>();
  AU.addRequired<seadsa::DsaLibFuncInfo>();
  // more precise than LLVM callgraph
  AU.addRequired<seadsa::CompleteCallGraph>();
  // clam requirements
  AU.addRequired<UnifyFunctionExitNodes>();
  AU.addRequired<clam::NameValues>();
}
  
char CrabPass::ID = 0;  
  
} // end namespace
} // end namespace

static llvm::RegisterPass<previrt::transforms::CrabPass>
X("Pcrab", "Use Crab invariants to simplify code",
  false, /*only looks CFG*/
  false  /*analysis pass*/);
