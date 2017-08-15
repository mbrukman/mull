#pragma once

#include "TestRunner.h"

#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

class Function;
class Module;

}

namespace mull {

  class CPPUnitRunner : public TestRunner {
  llvm::orc::ObjectLinkingLayer<> ObjectLayer;
  llvm::Mangler Mangler;
public:

  CPPUnitRunner(llvm::TargetMachine &machine);
  ExecutionResult runTest(Test *Test, ObjectFiles &ObjectFiles) override;

private:
  std::string MangleName(const llvm::StringRef &Name);
  void *GetCtorPointer(const llvm::Function &Function);
  void *FunctionPointer(const char *FunctionName);

  void runStaticCtor(llvm::Function *Ctor);
};

}
