#pragma once

#include "TestRunner.h"

#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/Mangler.h>

namespace llvm {

class Function;
class TargetMachine;

}

namespace mull {

class Test;

class SimpleTestRunner : public TestRunner {
  llvm::orc::ObjectLinkingLayer<> ObjectLayer;
  llvm::Mangler Mangler;
public:
  SimpleTestRunner(llvm::TargetMachine &targetMachine);
  ExecutionStatus runTest(Test *test, TestRunner::ObjectFiles &objectFiles) override;

private:
  std::string MangleName(const llvm::StringRef &Name);
  void *TestFunctionPointer(const llvm::Function &Function);
};

}
