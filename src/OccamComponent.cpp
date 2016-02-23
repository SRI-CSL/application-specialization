//
// OCCAM
//
// Copyright (c) 2011-2016, SRI International
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

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/User.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "PrevirtualizeInterfaces.h"
#include "Logging.h"

#include <vector>
#include <string>
#include <stdio.h>

using namespace llvm;

namespace previrt
{
  static inline GlobalValue::LinkageTypes
  localizeLinkage(GlobalValue::LinkageTypes l)
  {
    switch (l) {
    // TODO I'm not sure if all external definitions have an appropriate internal counterpart
    default:
      errs() << "Got other linkage! " << l << "\n";
      return l;
    case GlobalValue::ExternalLinkage:
      return GlobalValue::InternalLinkage;
    case GlobalValue::ExternalWeakLinkage:
      return GlobalValue::WeakODRLinkage;
    case GlobalValue::AppendingLinkage:
      return GlobalValue::AppendingLinkage;
    }
  }

  /*
   * Remove all code from the given module that is not necessary to
   * implement the given interface.
   */
  bool
  MinimizeComponent(Module& M, ComponentInterface& I, Logging& oclog)
  {
    bool modified = false;

    oclog << "interface!\n";
    I.dump();

    // Set all functions that are not in the interface to internal linkage only
    const StringMap<std::vector<CallInfo*> >::const_iterator end =
        I.calls.end();
    for (Module::iterator f = M.begin(), e = M.end(); f != e; ++f) {
      if (!f->isDeclaration() && f->hasExternalLinkage() &&
          I.calls.find(f->getName()) == end &&
          I.references.find(f->getName()) == I.references.end()) {
        oclog << "Hiding '" << f->getName() << "'\n";
        f->setLinkage(GlobalValue::InternalLinkage);
        modified = true;
      }
    }

    for (Module::global_iterator i = M.global_begin(), e = M.global_end(); i != e; ++i) {
      if (i->hasExternalLinkage() && i->hasInitializer() &&
          I.references.find(i->getName()) == I.references.end()) {
        oclog << "internalizing '" << i->getName() << "'\n";
        i->setLinkage(localizeLinkage(i->getLinkage()));
        modified = true;
      }
    }
    /* TODO: We want to do this, but libc has some problems...
    for (Module::alias_iterator i = M.alias_begin(), e = M.alias_end(); i != e; ++i) {
      if (i->hasExternalLinkage() &&
          I.references.find(i->getName()) == I.references.end() &&
          I.calls.find(i->getName()) == end) {
        oclog << "internalizing '" << i->getName() << "'\n";
        i->setLinkage(localizeLinkage(i->getLinkage()));
        modified = true;
      }
    }
    */



    // Perform global dead code elimination
    // TODO: To what extent should we do this here, versus
    //       doing it elsewhere?
    PassManager cdeMgr, mfMgr, mcMgr;
    cdeMgr.add(createGlobalDCEPass());
    //mfMgr.add(createMergeFunctionsPass());
    mcMgr.add(createConstantMergePass());
    bool moreToDo = true;
    unsigned int iters = 0;
    while (moreToDo && iters < 10000) {
      moreToDo = false;
      if (cdeMgr.run(M)) moreToDo = true;
      //if (mfMgr.run(M)) moreToDo = true;
      if (mcMgr.run(M)) moreToDo = true;
      modified = modified || moreToDo;
      ++iters;
    }

    if (moreToDo) {
      if (cdeMgr.run(M)) oclog << "GlobalDCE still had more to do\n";
      //if (mfMgr.run(M)) oclog << "MergeFunctions still had more to do\n";
      if (mcMgr.run(M)) oclog << "MergeConstants still had more to do\n";
    }

    if (modified) {
      oclog << "...progress...\n";
    }

    return modified;
  }

  static cl::list<std::string> OccamComponentInput(
      "Poccam-input", cl::NotHidden, cl::desc(
          "specifies the interface to prune with respect to"));
  class OccamPass : public ModulePass
  {
  public:
    ComponentInterface interface;
    static char ID;
  private:
    Logging oclog;
      public:
    OccamPass() :
      ModulePass(ID), oclog("OccamPass")
    {

      oclog << Logging::level::INFO << "OccamPass()\n";

      for (cl::list<std::string>::const_iterator b =
          OccamComponentInput.begin(), e = OccamComponentInput.end(); b
          != e; ++b) {
        oclog << "Reading file '" << *b << "'...";
        if (interface.readFromFile(*b)) {
          oclog << "success\n";
        } else {
          oclog << "failed\n";
        }
      }
      oclog << "Done reading.\n";
    }
    virtual
    ~OccamPass()
    {
    }
  public:
    virtual bool
    runOnModule(Module& M)
    {
      oclog << Logging::level::INFO << "runOnModule: " << M.getModuleIdentifier() << "\n";
      return MinimizeComponent(M, this->interface, oclog);
    }
  };
  char OccamPass::ID;

  static RegisterPass<OccamPass> X("Poccam",
      "hide/eliminate all non-external dependencies", false, false);

}
