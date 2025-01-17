#include "transforms/DevirtFunctions.hh"
#include "analysis/ClassHierarchyAnalysis.hh"
#include "transforms/utils/CallPromotionUtils.hh"

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "seadsa/CompleteCallGraph.hh"

#include <algorithm>
#include <set>

using namespace llvm;

#define DEVIRT_LOG(...) __VA_ARGS__
//#define DEVIRT_LOG(...)

namespace previrt {
namespace transforms {

static bool isIndirectCall(CallSite &CS) {
  Value *v = CS.getCalledValue();
  if (!v)
    return false;

  v = v->stripPointerCastsAndAliases();
  return !isa<Function>(v);
}

static PointerType *getVoidPtrType(LLVMContext &C) {
  Type *Int8Type = IntegerType::getInt8Ty(C);
  return PointerType::getUnqual(Int8Type);
}

static Value *castTo(Value *V, Type *Ty, std::string Name,
                     Instruction *InsertPt) {
  // Don't bother creating a cast if it's already the correct type.
  if (V->getType() == Ty)
    return V;

  // If it's a constant, just create a constant expression.
  if (Constant *C = dyn_cast<Constant>(V)) {
    Constant *CE = ConstantExpr::getZExtOrBitCast(C, Ty);
    return CE;
  }

  // Otherwise, insert a cast instruction.
  return CastInst::CreateZExtOrBitCast(V, Ty, Name, InsertPt);
}

namespace devirt_impl {
AliasSetId typeAliasId(CallSite &CS, bool LookThroughCast) {
  assert(isIndirectCall(CS) && "Not an indirect call");
  PointerType *pTy = nullptr;

  if (LookThroughCast) {
    /*
        %390 = load void (i8*, i32*, i32*, i64, i32)*,
                          void (i8*, i32*, i32*, i64, i32)**
                          bitcast (i64 (i8*, i32*, i32*, i64, i32)** @listdir to
                                   void (i8*, i32*, i32*, i64, i32)**)
        call void %390(i8* %385, i32* %1, i32* %2, i64 %139, i32 %26)
    */
    if (LoadInst *LI = dyn_cast<LoadInst>(CS.getCalledValue())) {
      if (Constant *C = dyn_cast<Constant>(LI->getPointerOperand())) {
        if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
          if (CE->getOpcode() == Instruction::BitCast) {
            if (PointerType *ppTy =
                    dyn_cast<PointerType>(CE->getOperand(0)->getType())) {
              pTy = dyn_cast<PointerType>(ppTy->getElementType());
              if (pTy) {
                assert(
                    isa<FunctionType>(pTy->getElementType()) &&
                    "The type of called value is not a pointer to a function");
              }
            }
          }
        }
      }
    }
  }

  if (pTy) {
    return pTy;
  }

  pTy = dyn_cast<PointerType>(CS.getCalledValue()->getType());
  assert(pTy && "Unexpected call not through a pointer");
  assert(isa<FunctionType>(pTy->getElementType()) &&
         "The type of called value is not a pointer to a function");
  return pTy;
}

AliasSetId typeAliasId(const Function &F) {
  return F.getFunctionType()->getPointerTo();
}
} // namespace devirt_impl

/***
 * Begin specific callsites resolvers
 ***/

CallSiteResolverByTypes::CallSiteResolverByTypes(Module &M)
    : CallSiteResolver(RESOLVER_TYPES), m_M(M) {
  populateTypeAliasSets();
}

CallSiteResolverByTypes::~CallSiteResolverByTypes() = default;

void CallSiteResolverByTypes::populateTypeAliasSets() {
  // -- Create type-based alias sets
  for (auto const &F : m_M) {
    // -- intrinsics are never called indirectly
    if (F.isIntrinsic())
      continue;

    // -- local functions whose address is not taken cannot be
    // -- resolved by a function pointer
    if (F.hasLocalLinkage() && !F.hasAddressTaken())
      continue;

    // -- skip calls to declarations, these are resolved implicitly
    // -- by calling through the function pointer argument in the
    // -- default case of bounce function

    // XXX: In OCCAM, it's common to take the address of an external
    // function if declared in another library.
    // if (F.isDeclaration())
    //  continue;

    // -- skip seahorn and verifier specific intrinsics
    if (F.getName().startswith("seahorn."))
      continue;
    if (F.getName().startswith("verifier."))
      continue;
    // -- assume entry point is never called indirectly
    if (F.getName().equals("main"))
      continue;

    // -- add F to its corresponding alias set (keep sorted the Targets)
    AliasSet &Targets = m_targets_map[devirt_impl::typeAliasId(F)];
    auto it = std::upper_bound(Targets.begin(), Targets.end(), &F);
    Targets.insert(it, &F);
  }
}

const CallSiteResolverByTypes::AliasSet *
CallSiteResolverByTypes::getTargets(CallSite &CS) {
  AliasSetId id = devirt_impl::typeAliasId(CS, true);
  auto it = m_targets_map.find(id);
  if (it != m_targets_map.end()) {
    return &(it->second);
  }
  // errs() << "WARNING Devirt (types): cannot resolved " << CS.getInstruction()
  // 	   << " because no functions found in the module with same signature\n";
  return nullptr;
}

#ifdef USE_BOUNCE_FUNCTIONS
Function *CallSiteResolverByTypes::getBounceFunction(CallSite &CS) {
  AliasSetId id = devirt_impl::typeAliasId(CS, false);
  auto it = m_bounce_map.find(id);
  if (it != m_bounce_map.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

void CallSiteResolverByTypes::cacheBounceFunction(CallSite &CS,
                                                  Function *bounce) {
  AliasSetId id = devirt_impl::typeAliasId(CS, false);
  m_bounce_map.insert({id, bounce});
}
#endif

// // Discard all dsa targets whose signature do not match with function
// // defined within the module. 
// bool CallSiteResolverBySeaDsa::enforceWellTyping(CallSite &CS,
// 						 const AliasSet &dsa_targets,
// 						 AliasSet &out) {
//   if (const AliasSet *types_targets =
//       CallSiteResolverByTypes::getTargets(CS)) {
//     DEVIRT_LOG(errs() << "Type-based targets: \n";
// 	       for (auto F: *types_targets) {
// 		 errs() << "\t" << F->getName()
// 			<< "::" << *(F->getType()) << "\n";
// 	       });
    
//     std::set_intersection(dsa_targets.begin(), dsa_targets.end(),
// 			  types_targets->begin(),
// 			  types_targets->end(),
// 			  std::back_inserter(out));
//     if (out.empty()) {
//       errs() << "WARNING Devirt (dsa): cannot resolve "
// 	     << *(CS.getInstruction())
// 	     << " after refining dsa targets with calsite type\n";
//     } else if (out.size() <= m_max_num_targets) {
//       // Sort by name so that we can fix a
//       // deterministic ordering (useful for e.g., tests)
//       if (std::all_of(out.begin(), out.end(),
// 		      [](const Function *f) { return f->hasName(); })) {
// 	std::sort(out.begin(), out.end(),
// 		  [](const Function *f1, const Function *f2) {
// 		    return f1->getName() < f2->getName();
// 		  });
//       }
//       DEVIRT_LOG(errs() << "Devirt (dsa) resolved "
// 		 << *(CS.getInstruction())
// 		   << " with targets=\n";
// 		 for (auto F : out) {
// 		     errs() << "\t" << F->getName()
// 			    << "::" << *(F->getType()) << "\n";
// 		 });
//       return true;
//     } else {
//       errs() << "WARNING Devirt (dsa): unresolve "
// 	     << *(CS.getInstruction())
// 	     << " because the number of targets is greater than "
// 	     << m_max_num_targets << "\n";
//     }
//   }
//   return false;
// }
						   
						   
CallSiteResolverBySeaDsa::CallSiteResolverBySeaDsa(
    Module &M, seadsa::CompleteCallGraph &cg, bool incomplete,
    unsigned max_num_targets)
    : CallSiteResolverByTypes(M), m_M(M), m_seadsa_cg(cg),
      m_allow_incomplete(incomplete), m_max_num_targets(max_num_targets) {

  CallSiteResolver::m_kind = RESOLVER_SEADSA;

  /*
    Assume that seadsa::CompleteCallGraph provides these methods:
     - bool isComplete(CallSite&)
     - iterator begin(CallSite&)
     - iterator end(CallSite&)
     where each element of iterator is of type Function*
  */

  // build the target map
  unsigned num_indirect_calls = 0;
  unsigned num_complete_calls = 0;
  unsigned num_resolved_calls = 0;
  for (auto &F: M) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      CallSite CS(&*I);
      if (CS.getInstruction() && isIndirectCall(CS)) {
	num_indirect_calls++;
	if (m_allow_incomplete || m_seadsa_cg.isComplete(CS)) {
	  num_complete_calls++;
	  AliasSet dsa_targets;
	  dsa_targets.append(m_seadsa_cg.begin(CS), m_seadsa_cg.end(CS));
	  if (dsa_targets.empty()) {
	    errs() << "WARNING Devirt (dsa): does not have any target for "
		 << *(CS.getInstruction()) << "\n";
	    continue;
	  }

	  if (std::all_of(dsa_targets.begin(), dsa_targets.end(),
			  [](const Function *f) { return f->hasName(); })) {
	    std::sort(dsa_targets.begin(), dsa_targets.end(),
		      [](const Function *f1, const Function *f2) {
			return f1->getName() < f2->getName();
		      });
	  } else {
	    std::sort(dsa_targets.begin(), dsa_targets.end());
	  }
	  
	  DEVIRT_LOG(errs() << "Devirt (dsa): resolved " << *(CS.getInstruction())
		            << " with targets:\n";
		     for (auto F: dsa_targets) {
		       errs() << "\t" << F->getName()
			      << "::" << *(F->getType()) << "\n";
		     });
	  m_targets_map.insert({CS.getInstruction(), dsa_targets});
	  num_resolved_calls++;
	} else {
	  errs() << "WARNING Devirt (dsa): cannot resolve "
		 << *(CS.getInstruction())
		 << " because the corresponding dsa node is not complete\n";
	  DEVIRT_LOG(errs() << "Dsa-based targets: \n";
		     for (auto F: llvm::make_range(m_seadsa_cg.begin(CS),
						   m_seadsa_cg.end(CS))) { 
		       errs() << "\t" << F->getName()
			      << "::" << *(F->getType()) << "\n";
		     };)
	    }
      }
    }
  }
  errs() << "=== DEVIRT (Dsa+types) stats===\n";
  errs() << "BRUNCH_STAT INDIRECT CALLS " << num_indirect_calls << "\n";
  errs() << "BRUNCH_STAT COMPLETE CALLS " << num_complete_calls << "\n";
  errs() << "BRUNCH_STAT RESOLVED CALLS " << num_resolved_calls << "\n";
}

const typename CallSiteResolverBySeaDsa::AliasSet *
CallSiteResolverBySeaDsa::getTargets(CallSite &CS) {
  auto it = m_targets_map.find(CS.getInstruction());
  if (it != m_targets_map.end()) {
    return &(it->second);
  }
  return nullptr;
}

#ifdef USE_BOUNCE_FUNCTIONS
Function *CallSiteResolverBySeaDsa::getBounceFunction(CallSite &CS) {
  AliasSetId id = devirt_impl::typeAliasId(CS, false);
  auto it = m_bounce_map.find(id);
  if (it != m_bounce_map.end()) {
    const AliasSet *cachedTargets = it->second.first;
    const AliasSet *Targets = getTargets(CS);
    if (cachedTargets && Targets) {
      if (std::equal(cachedTargets->begin(), cachedTargets->end(),
                     Targets->begin())) {
        return it->second.second;
      }
    }
  }
  return nullptr;
}

void CallSiteResolverBySeaDsa::cacheBounceFunction(CallSite &CS,
                                                   Function *bounce) {
  if (const AliasSet *targets = getTargets(CS)) {
    AliasSetId id = devirt_impl::typeAliasId(CS, false);
    m_bounce_map.insert({id, {targets, bounce}});
  }
}
#endif

CallSiteResolverByCHA::CallSiteResolverByCHA(Module &M)
    : CallSiteResolverByTypes(M),
      m_cha(std::make_unique<analysis::ClassHierarchyAnalysis>(M)) {
  CallSiteResolver::m_kind = RESOLVER_CHA;
  m_cha->calculate();
  DEVIRT_LOG(errs() << "Results of the Class Hierarchy Analysis\n";
             m_cha->printStats(errs()););
}

CallSiteResolverByCHA::~CallSiteResolverByCHA() {}

const typename CallSiteResolverByCHA::AliasSet *
CallSiteResolverByCHA::getTargets(CallSite &CS) {
  auto it = m_targets_map.find(CS.getInstruction());
  if (it != m_targets_map.end()) {
    return &(it->second);
  } else {
    AliasSet out;
    if (m_cha->resolveVirtualCall(CS, out)) {
      if (out.empty()) {
        // This can print too much noise if the program has very few
        // virtual calls.
        errs() << "WARNING Devirt (cha): cannot resolve "
               << *(CS.getInstruction()) << "\n";
      } else {
        DEVIRT_LOG(errs() << "Devirt (cha): resolved " << *(CS.getInstruction())
                          << " with targets=\n";
                   for (auto F
                        : out) {
                     errs() << "\t" << F->getName() << "::" << *(F->getType())
                            << "\n";
                   });
        m_targets_map.insert({CS.getInstruction(), out});
      }
    } else {
      // This can print too much noise if the program has very few
      // virtual calls.
      errs() << "WARNING Devirt (cha): cannot resolve "
             << *(CS.getInstruction()) << "\n";
    }
  }
  return nullptr;
}

#ifdef USE_BOUNCE_FUNCTIONS
Function *CallSiteResolverByCHA::getBounceFunction(CallSite &CS) {
  AliasSetId id = devirt_impl::typeAliasId(CS, false);
  auto it = m_bounce_map.find(id);
  if (it != m_bounce_map.end()) {
    const AliasSet *cachedTargets = it->second.first;
    const AliasSet *Targets = getTargets(CS);
    if (cachedTargets && Targets) {
      if (std::equal(cachedTargets->begin(), cachedTargets->end(),
                     Targets->begin())) {
        return it->second.second;
      }
    }
  }
  return nullptr;
}

void CallSiteResolverByCHA::cacheBounceFunction(CallSite &CS,
                                                Function *bounce) {
  if (const AliasSet *targets = getTargets(CS)) {
    AliasSetId id = devirt_impl::typeAliasId(CS, false);
    m_bounce_map.insert({id, {targets, bounce}});
  }
}
#endif

/***
 * End specific callsites resolver
 ***/

DevirtualizeFunctions::DevirtualizeFunctions(llvm::CallGraph * /*cg*/) {}

#ifdef USE_BOUNCE_FUNCTIONS
Function *DevirtualizeFunctions::mkBounceFn(CallSite &CS,
                                            CallSiteResolver *CSR) {
  assert(isIndirectCall(CS) && "Not an indirect call");

  if (Function *bounce = CSR->getBounceFunction(CS)) {
    DEVIRT_LOG(errs() << "Reusing bounce function for "
                      << *(CS.getInstruction()) << "\n\t" << bounce->getName()
                      << "::" << *(bounce->getType()) << "\n";);
    return bounce;
  }

  const AliasSet *Targets = CSR->getTargets(CS);
  if (!Targets || Targets->empty()) {
    return nullptr;
  }

  DEVIRT_LOG(errs() << *CS.getInstruction() << "\n";
             errs() << "Possible targets:\n"; for (const Function *F
                                                   : *Targets) {
               errs() << "\t" << F->getName() << ":: " << *(F->getType())
                      << "\n";
             })

  // Create a bounce function that has a function signature almost
  // identical to the function being called.  The only difference is
  // that it will have an additional pointer argument at the
  // beginning of its argument list that will be the function to
  // call.
  Value *ptr = CS.getCalledValue();
  SmallVector<Type *, 8> TP;
  TP.push_back(ptr->getType());
  for (auto i = CS.arg_begin(), e = CS.arg_end(); i != e; ++i)
    TP.push_back((*i)->getType());

  FunctionType *NewTy = FunctionType::get(CS.getType(), TP, false);
  Module *M = CS.getInstruction()->getParent()->getParent()->getParent();
  assert(M);
  Function *F = Function::Create(NewTy, GlobalValue::InternalLinkage,
                                 "__occam.bounce", M);

  // Set the names of the arguments.  Also, record the arguments in a vector
  // for subsequence access.
  F->arg_begin()->setName("funcPtr");
  SmallVector<Value *, 8> fargs;
  auto ai = F->arg_begin();
  ++ai;
  for (auto ae = F->arg_end(); ai != ae; ++ai) {
    fargs.push_back(&*ai);
    ai->setName("arg");
  }

  // Create an entry basic block for the function.  All it should do is perform
  // some cast instructions and branch to the first comparison basic block.
  BasicBlock *entryBB = BasicBlock::Create(M->getContext(), "entry", F);

  // For each function target, create a basic block that will call that
  // function directly.
  DenseMap<const Function *, BasicBlock *> targets;
  for (const Function *FL : *Targets) {
    // Create the basic block for doing the direct call
    BasicBlock *BL = BasicBlock::Create(M->getContext(), FL->getName(), F);
    targets[FL] = BL;
    // Create the direct function call
    CallingConv::ID cc = FL->getCallingConv();
    CallInst *directCall =
        CallInst::Create(const_cast<Function *>(FL), fargs, "", BL);
    directCall->setCallingConv(cc);
    // update call graph
    // if (m_cg) {
    //   auto fl_cg = m_cg->getOrInsertFunction (const_cast<Function*> (FL));
    //   auto cf_cg = m_cg->getOrInsertFunction (directCall->getCalledFunction
    //   ());
    //   fl_cg->addCalledFunction (CallSite (directCall), cf_cg);
    // }

    // Add the return instruction for the basic block
    if (CS.getType()->isVoidTy())
      ReturnInst::Create(M->getContext(), BL);
    else
      ReturnInst::Create(M->getContext(), directCall, BL);
  }

  BasicBlock *defaultBB = nullptr;
  if (false) {
    // Create a default basic block having the original indirect call
    //
    // JN: For now, we never execute this code because leaving the
    // original indirect call defeats a the purpose of the whole
    // devirtualization process.  This code be only needed if we
    // allow incomplete callees to resolve an indirect call.

    defaultBB = BasicBlock::Create(M->getContext(), "default", F);
    if (CS.getType()->isVoidTy()) {
      ReturnInst::Create(M->getContext(), defaultBB);
    } else {
      CallInst *defaultRet =
          CallInst::Create(&*(F->arg_begin()), fargs, "", defaultBB);
      ReturnInst::Create(M->getContext(), defaultRet, defaultBB);
    }
  } else {
    // Create a failure basic block.  This basic block should simply be an
    // unreachable instruction.
    defaultBB = BasicBlock::Create(M->getContext(), "fail", F);
    new UnreachableInst(M->getContext(), defaultBB);
  }

  // Setup the entry basic block.  For now, just have it call the default
  // basic block.  We'll change the basic block to which it branches later.
  BranchInst *InsertPt = BranchInst::Create(defaultBB, entryBB);

  // Create basic blocks which will test the value of the incoming function
  // pointer and branch to the appropriate basic block to call the function.
  Type *VoidPtrType = getVoidPtrType(M->getContext());
  Value *FArg = castTo(&*(F->arg_begin()), VoidPtrType, "", InsertPt);
  BasicBlock *tailBB = defaultBB;
  for (const Function *FL : *Targets) {
    // Cast the function pointer to an integer.  This can go in the entry
    // block.
    Value *TargetInt =
        castTo(const_cast<Function *>(FL), VoidPtrType, "", InsertPt);

    // Create a new basic block that compares the function pointer to the
    // function target.  If the function pointer matches, we'll branch to the
    // basic block performing the direct call for that function; otherwise,
    // we'll branch to the next function call target.
    BasicBlock *TB = targets[FL];
    BasicBlock *newB =
        BasicBlock::Create(M->getContext(), "test." + FL->getName(), F);
    CmpInst *setcc = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ,
                                     TargetInt, FArg, "sc", newB);
    BranchInst::Create(TB, tailBB, setcc, newB);

    // Make this newly created basic block the next block that will be reached
    // when the next comparison will need to be done.
    tailBB = newB;
  }

  // Make the entry basic block branch to the first comparison basic block.
  InsertPt->setSuccessor(0, tailBB);

  // -- cache the newly created function
  CSR->cacheBounceFunction(CS, F);

  // Return the newly created bounce function.
  return F;
}
#endif

void DevirtualizeFunctions::mkDirectCall(CallSite CS, CallSiteResolver *CSR) {
#ifndef USE_BOUNCE_FUNCTIONS
  const AliasSet *Targets = CSR->getTargets(CS);
  if (!Targets || Targets->empty()) {
    // cannot resolve the indirect call
    return;
  }

  DEVIRT_LOG(errs() << "OCCAM -- Resolving indirect call site:\n"
                    << *CS.getInstruction() << " using:\n";
             for (auto &f
                  : *Targets) {
               errs() << "\t" << f->getName() << " :: " << *(f->getType())
                      << "\n";
             });

  std::vector<Function *> Callees;
  Callees.resize(Targets->size());
  std::transform(Targets->begin(), Targets->end(), Callees.begin(),
                 [](const Function *fn) { return const_cast<Function *>(fn); });
  previrt::transforms::promoteIndirectCall(CS, Callees);
#else
  const Function *bounceFn = mkBounceFn(CS, CSR);
  // -- something failed
  if (!bounceFn)
    return;

  DEVIRT_LOG(errs() << "Callsite: " << *(CS.getInstruction()) << "\n";
             errs() << "Bounce function: " << bounceFn->getName()
                    << ":: " << *(bounceFn->getType()) << "\n";)

  // Replace the original call with a call to the bounce function.
  if (CallInst *CI = dyn_cast<CallInst>(CS.getInstruction())) {
    // The last operand in the op list is the callee
    SmallVector<Value *, 8> Params;
    Params.reserve(std::distance(CI->op_begin(), CI->op_end()));
    Params.push_back(*(CI->op_end() - 1));
    Params.insert(Params.end(), CI->op_begin(), (CI->op_end() - 1));
    std::string name = CI->hasName() ? CI->getName().str() + ".dv" : "";
    CallInst *CN =
        CallInst::Create(const_cast<Function *>(bounceFn), Params, name, CI);

    // update call graph
    // if (m_cg) {
    //   m_cg->getOrInsertFunction (const_cast<Function*> (bounceFn));
    //   (*m_cg)[CI->getParent ()->getParent ()]->addCalledFunction
    //     (CallSite (CN), (*m_cg)[CN->getCalledFunction ()]);
    // }

    CN->setDebugLoc(CI->getDebugLoc());
    CI->replaceAllUsesWith(CN);
    CI->eraseFromParent();
  } else if (InvokeInst *CI = dyn_cast<InvokeInst>(CS.getInstruction())) {
    SmallVector<Value *, 8> Params;
    Params.reserve(
        std::distance(CI->arg_operands().begin(), CI->arg_operands().end()));
    // insert first the callee
    Params.push_back(CI->getCalledValue());
    Params.insert(Params.end(), CI->arg_operands().begin(),
                  CI->arg_operands().end());

    std::string name = CI->hasName() ? CI->getName().str() + ".dv" : "";
    InvokeInst *CN = InvokeInst::Create(const_cast<Function *>(bounceFn),
                                        CI->getNormalDest(),
                                        CI->getUnwindDest(), Params, name, CI);

    // update call graph
    // if (m_cg) {
    //   m_cg->getOrInsertFunction (const_cast<Function*> (bounceFn));
    //   (*m_cg)[CI->getParent ()->getParent ()]->addCalledFunction
    //     (CallSite (CN), (*m_cg)[CN->getCalledFunction ()]);
    // }

    CN->setDebugLoc(CI->getDebugLoc());
    CI->replaceAllUsesWith(CN);
    CI->eraseFromParent();
  }
#endif
}

void DevirtualizeFunctions::visitCallSite(CallSite CS) {
  // -- skip direct calls
  if (!isIndirectCall(CS))
    return;

  // This is an indirect call site.  Put it in the worklist of call
  // sites to transforms.
  m_worklist.push_back(CS.getInstruction());
  return;
}

void DevirtualizeFunctions::visitCallInst(CallInst &CI) {
  // we cannot take the address of an inline asm
  if (CI.isInlineAsm())
    return;

  CallSite CS(&CI);
  visitCallSite(CS);
}

void DevirtualizeFunctions::visitInvokeInst(InvokeInst &II) {
  CallSite CS(&II);
  visitCallSite(CS);
}

bool DevirtualizeFunctions::resolveCallSites(Module &M, CallSiteResolver *CSR) {
  // -- Visit all of the call instructions in this function and
  // -- record those that are indirect function calls.
  visit(M);

  // -- Now go through and transform all of the indirect calls that
  // -- we found that need transforming.
  bool Changed = !m_worklist.empty();
  while (!m_worklist.empty()) {
    auto I = m_worklist.back();
    m_worklist.pop_back();
    CallSite CS(I);
    mkDirectCall(CS, CSR);
  }
  // -- Conservatively assume that we've changed one or more call
  // -- sites.
  return Changed;
}
} // end namespace
} // end namespace
