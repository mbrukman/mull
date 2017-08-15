#pragma once

#include "CPPUnit/CPPUnitMutationOperatorFilter.h"
#include "MutationPoint.h"
#include "TestFinder.h"

#include "llvm/ADT/StringMap.h"

#include <map>
#include <vector>

namespace llvm {

class Function;

}

namespace mull {

class Context;
class MutationOperator;
class MutationPoint;

class CPPUnitFinder : public TestFinder {
  llvm::StringMap<llvm::Function *> FunctionRegistry;
  std::vector<std::unique_ptr<MutationPoint>> MutationPoints;
  std::map<llvm::Function *, std::vector<MutationPoint *>> MutationPointsRegistry;

  std::vector<std::unique_ptr<MutationOperator>> mutationOperators;

  CPPUnitMutationOperatorFilter filter;
public:
  CPPUnitFinder(std::vector<std::unique_ptr<MutationOperator>> mutationOperators,
                   std::vector<std::string> testsToFilter,
                   std::vector<std::string> excludeLocations);

  std::vector<std::unique_ptr<Test>> findTests(Context &Ctx) override;
  std::vector<std::unique_ptr<Testee>> findTestees(Test *Test,
                                                   Context &Ctx,
                                                   int maxDistance) override;

  std::vector<std::unique_ptr<MutationPoint>> findMutationPoints(
                          std::vector<MutationOperator *> &MutationOperators,
                          llvm::Function &F) override;

  std::vector<MutationPoint *> findMutationPoints(const Context &context,
                                                  llvm::Function &F) override;
};

}
