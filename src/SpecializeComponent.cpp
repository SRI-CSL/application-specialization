//
// OCCAM
//
// Copyright (c) 2011-2012, SRI International
//
//  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of SRI International nor the names of its contributors may
//   be used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "llvm/Pass.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/User.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "Logging.h"
#include "Specializer.h"
#include "SpecializationPolicy.h"

#include <vector>
#include <list>
#include <string>
#include <stdio.h>
#include <fstream>

using namespace llvm;

namespace previrt
{

  static Function*
  resolveFunction(Module& M, StringRef name)
  {
    Function* f = M.getFunction(name);
    if (f != NULL)
      return f;
    GlobalAlias* ga = M.getNamedAlias(name);
    if (ga != NULL) {
      const GlobalValue* v = ga->getBaseObject();
      f = dyn_cast<Function>(const_cast<GlobalValue*>(v));
      if (f != NULL) {
        errs() << "Resolved alias " << name << " to " << f->getName() << "\n";
        return f;
      }
    }
    return NULL;
  }

  /*
   * Reduce this module with respect to the given interface.
   * - The interface suggests some of the uses of the functions,
   *   so here we can generate special versions of those functions.
   * Generate a ComponentInterfaceTransform for clients to rewrite their
   * code to use the new API
   */
  bool
  SpecializeComponent(Module& M, ComponentInterfaceTransform& T,
      SpecializationPolicy &policy, std::list<Function*>& to_add)
  {
    int rewrite_count = 0;
    const ComponentInterface& I = T.getInterface();
    // TODO: What needs to be done?
    // - Should try to handle strings & arrays
    for (ComponentInterface::FunctionIterator ff = I.begin(), fe = I.end(); ff
        != fe; ++ff) {
      StringRef name = ff->first();
      Function* func = resolveFunction(M, name);
      if (func == NULL || func->isDeclaration()) {
        // We don't specialize declarations because we don't own them
        continue;
      }

      for (ComponentInterface::CallIterator cc = I.call_begin(name), ce =
          I.call_end(name); cc != ce; ++cc) {
        const CallInfo* const call = *cc;

        const unsigned arg_count = call->args.size();

        if (func->isVarArg()) {
          // TODO: I don't know how to specialize variable argument functions yet
          continue;
        }
        if (arg_count != func->getArgumentList().size()) {
          // Not referring to this function?
          // NOTE: I can't assert this equality because of the way that approximations occur
          continue;
        }

        SmallBitVector slice(arg_count);
        bool shouldSpecialize = policy.specializeOn(func, call->args.begin(),
            call->args.end(), slice);

        if (!shouldSpecialize)
          continue;

        std::vector<Value*> args;
        std::vector<unsigned> argPerm;
        args.reserve(arg_count);
        argPerm.reserve(slice.count());
        for (unsigned i = 0; i < arg_count; i++) {
          if (slice.test(i)) {
	      Type * paramType = func->getFunctionType()->getParamType(i);
	      Value *concreteArg = call->args[i].concretize(M, paramType);
	      args.push_back(concreteArg);
	      assert(concreteArg->getType() == paramType
		     && "Specializing function with concrete argument of wrong type!");
          } else {
            args.push_back(NULL);
            argPerm.push_back(i);
          }
        }

        Function* nfunc = specializeFunction(func, args);
        nfunc->setLinkage(GlobalValue::ExternalLinkage);

        FunctionHandle rewriteTo = nfunc->getName();

        T.rewrite(name, call, rewriteTo, argPerm);

        to_add.push_back(nfunc);
        rewrite_count++;
      }
    }
    if (rewrite_count > 0) {
      errs() << rewrite_count << " pending rewrites\n";
    }
    return rewrite_count > 0;
  }
}

namespace {
  using namespace previrt;

  static cl::list<std::string> SpecializeComponentInput("Pspecialize-input",
      cl::NotHidden, cl::desc(
          "specifies the interface to specialize with respect to"));
  static cl::opt<std::string> SpecializeComponentOutput("Pspecialize-output",
        cl::init(""), cl::NotHidden, cl::desc(
            "specifies the output file for the rewrite specification"));

  class SpecializeComponentPass : public ModulePass
  {
  public:
    ComponentInterfaceTransform transform;
    static char ID;

  private:
    Logging oclog;
    
  public:
    SpecializeComponentPass() :
      ModulePass(ID), oclog("SpecializeComponentPass")
    {
      for (cl::list<std::string>::const_iterator b = SpecializeComponentInput.begin(), e = SpecializeComponentInput.end();
           b != e; ++b) {
        oclog << "Reading file '" << *b << "'...";
        if (transform.readInterfaceFromFile(*b)) {
          oclog << "success\n";
        } else {
          oclog << "failed\n";
        }
      }
      oclog << "Done reading.\n";
      if (transform.interface != NULL) {
        oclog << transform.interface->calls.size() << " calls\n";
      } else {
        oclog << "No interfaces read.\n";
      }
    }
    virtual
    ~SpecializeComponentPass()
    {
    }
  public:
    virtual bool
    runOnModule(Module& M)
    {
      if (transform.interface == NULL) return false;
      
      oclog << Logging::level::INFO << "runOnModule: " << M.getModuleIdentifier() << "\n";
      

      std::list<Function*> to_add;
      CallGraphWrapperPass& CG = getAnalysis<CallGraphWrapperPass> ();
      SpecializationPolicy* policy = SpecializationPolicy::recursiveGuard(
          SpecializationPolicy::aggressivePolicy(), CG);

      bool modified = SpecializeComponent(M, this->transform, *policy, to_add);
     
      Module::FunctionListType &functionList = M.getFunctionList();
      while (!to_add.empty()) {
	Function *add = to_add.front();
	to_add.pop_front();
	if (add->getParent() == &M) {
	  // Already in module
	  continue;
	} else {
	  functionList.push_back(add);
	}
      }

      if (SpecializeComponentOutput != "") {
        proto::ComponentInterfaceTransform buf;
        codeInto(this->transform, buf);
        std::ofstream output(SpecializeComponentOutput.c_str(), std::ios::binary | std::ios::trunc);
        assert(buf.SerializeToOstream(&output));
        output.close();
      }

      policy->release();

      return modified;
    }
    virtual void
    getAnalysisUsage(AnalysisUsage &AU) const
    {
      AU.addRequired<CallGraphWrapperPass> ();
    }
  };
  char SpecializeComponentPass::ID;

  static RegisterPass<SpecializeComponentPass> X("Pspecialize",
      "previrtualize the given module (requires parameters)", false, false);
}
