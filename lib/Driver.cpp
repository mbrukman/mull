#include "Driver.h"

#include "Config.h"
#include "Context.h"
#include "Logger.h"
#include "ModuleLoader.h"
#include "Result.h"
#include "Testee.h"
#include "MutationResult.h"
#include "TestFinder.h"
#include "TestRunner.h"
#include "MutationsFinder.h"

#include <llvm/ExecutionEngine/Orc/JITSymbol.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Support/DynamicLibrary.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>
#include <sys/mman.h>
#include <sys/types.h>

using namespace llvm;
using namespace llvm::object;
using namespace mull;
using namespace std;
using namespace std::chrono;

namespace mull {

extern "C" void mull_enterFunction(Driver *driver, uint64_t functionIndex) {
  assert(driver);
  assert(driver->callTreeMapping());
  DynamicCallTree::enterFunction(functionIndex,
                                 driver->callTreeMapping(),
                                 driver->callstack());
}

extern "C" void mull_leaveFunction(Driver *driver, uint64_t functionIndex) {
  assert(driver);
  assert(driver->callTreeMapping());
  DynamicCallTree::leaveFunction(functionIndex,
                                 driver->callTreeMapping(),
                                 driver->callstack());
}

}

Driver::~Driver() {
  delete this->Sandbox;
  delete this->diagnostics;
  munmap(_callTreeMapping, functions.size());
}

/// Populate mull::Context with modules using
/// ModulePaths from mull::Config.
/// mull::Context should be populated using ModuleLoader
/// so that we could inject modules from string for testing purposes

/// Having mull::Context in place we could instantiate TestFinder and find all tests
/// Using same TestFinder we could find mutation points, apply them sequentially and
/// run tests/mutants using newly created TestRunner

/// This method should return (somehow) results of the tests/mutants execution
/// So that we could easily plug in some TestReporter

/// UPD: The method returns set of results
/// Number of results equals to a number of tests
/// Each result contains result of execution of an original test and
/// all the results of each mutant within corresponding MutationPoint

std::unique_ptr<Result> Driver::Run() {
  /// Assumption: all modules will be used during the execution
  /// Therefore we load them into memory and compile immediately
  /// Later on modules used only for generating of mutants
  std::vector<std::string> bitcodePaths = Cfg.getBitcodePaths();
  std::vector<unique_ptr<MullModule>> modules =
    Loader.loadModulesFromBitcodeFileList(bitcodePaths);

  for (auto &ownedModule : modules) {
    MullModule &module = *ownedModule.get();
    assert(ownedModule && "Can't load module");
    Ctx.addModule(std::move(ownedModule));

    ObjectFile *objectFile = toolchain.cache().getObject(module);

    if (objectFile == nullptr) {
      LLVMContext localContext;

      auto clonedModule = module.clone(localContext);

      for (auto &function: module.getModule()->getFunctionList()) {
        if (function.isDeclaration()) {
          continue;
        }
        CallTreeFunction callTreeFunction(&function);
        uint64_t index = functions.size();
        functions.push_back(callTreeFunction);
        auto clonedFunction = clonedModule->getModule()->getFunction(function.getName());
        injectCallbacks(clonedFunction, index);
      }

      auto owningObjectFile = toolchain.compiler().compileModule(*clonedModule.get());
      objectFile = owningObjectFile.getBinary();
      toolchain.cache().putObject(std::move(owningObjectFile), module);
    }

    InnerCache.insert(std::make_pair(module.getModule(), objectFile));
  }

  for (std::string &objectFilePath: Cfg.getObjectFilesPaths()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> buffer =
      MemoryBuffer::getFile(objectFilePath.c_str());

    if (!buffer) {
      Logger::error() << "Cannot load object file: " << objectFilePath << "\n";
      continue;
    }

    Expected<std::unique_ptr<ObjectFile>> objectOrError =
      ObjectFile::createObjectFile(buffer.get()->getMemBufferRef());

    if (!objectOrError) {
      Logger::error() << "Cannot create object file: " << objectFilePath << "\n";
      continue;
    }

    std::unique_ptr<ObjectFile> objectFile(std::move(objectOrError.get()));

    auto owningObject = OwningBinary<ObjectFile>(std::move(objectFile),
                                                 std::move(buffer.get()));
    precompiledObjectFiles.push_back(std::move(owningObject));
  }

  prepareForExecution();

  auto foundTests = Finder.findTests(Ctx, filter);
  const int testsCount = foundTests.size();

  Logger::debug() << "Driver::Run> found "
                  << testsCount
                  << " tests\n";

  for (std::string &dylibPath: Cfg.getDynamicLibrariesPaths()) {
    sys::DynamicLibrary::LoadLibraryPermanently(dylibPath.c_str());
  }

  Logger::debug() << "Driver::Run> running tests and searching mutations\n";

  std::vector<MutationPoint *> allMutationPoints;
  auto objectFiles = AllObjectFiles();
  for (auto &test : foundTests) {
    Logger::debug() << ".";

    _callstack = stack<uint64_t>();
    memset(_callTreeMapping, 0, functions.size() * sizeof(_callTreeMapping[0]));

    ExecutionResult testExecutionResult = Sandbox->run([&]() {
      return Runner.runTest(test.get(), objectFiles);
    }, Cfg.getTimeout());

    test->setExecutionResult(testExecutionResult);

    if (testExecutionResult.status != Passed) {
      continue;
    }

    std::unique_ptr<CallTree> callTree(dynamicCallTree.createCallTree());

    auto subtrees = dynamicCallTree.extractTestSubtrees(callTree.get(), test.get());
    auto testees = dynamicCallTree.createTestees(subtrees, test.get(),
                                                 Cfg.getMaxDistance(),
                                                 filter);

    dynamicCallTree.cleanupCallTree(std::move(callTree));
    if (testees.empty()) {
      continue;
    }

    for (auto testee_it = std::next(testees.begin()), ee = testees.end();
         testee_it != ee;
         ++testee_it) {

      std::unique_ptr<Testee> &testee = *testee_it;

      auto mutationPoints = mutationsFinder.getMutationPoints(Ctx, *testee.get(), filter);
      std::copy(mutationPoints.begin(), mutationPoints.end(), std::back_inserter(allMutationPoints));
    }
  }

  Logger::debug() << "Driver::Run> found " << allMutationPoints.size() << " mutations\n";

  std::vector<std::unique_ptr<MutationResult>> mutationResults;

  for (auto mutationPoint : allMutationPoints) {
    auto objectFilesWithMutant = AllButOne(mutationPoint->getOriginalModule()->getModule());

    LLVMContext localContext;
    auto clonedModule = mutationPoint->getOriginalModule()->clone(localContext);
    mutationPoint->applyMutation(*clonedModule.get());
    auto owningObject = toolchain.compiler().compileModule(*clonedModule.get());
    ObjectFile *mutant = owningObject.getBinary();

    objectFilesWithMutant.push_back(mutant);

    for (auto &reachableTest : mutationPoint->getReachableTests()) {
      auto test = reachableTest.first;
      auto distance = reachableTest.second;

      auto timeout = test->getExecutionResult().runningTime * 10;

      ExecutionResult result;
      bool dryRun = Cfg.isDryRun();
      if (dryRun) {
        result.status = DryRun;
        result.runningTime = timeout;
      } else {
        const auto sandboxTimeout = std::max(30LL, timeout);

        result = Sandbox->run([&]() {
          ExecutionStatus status = Runner.runTest(test, objectFilesWithMutant);
          assert(status != ExecutionStatus::Invalid && "Expect to see valid TestResult");
          return status;
        }, sandboxTimeout);

        assert(result.status != ExecutionStatus::Invalid &&
               "Expect to see valid TestResult");
      }

      mutationResults.push_back(make_unique<MutationResult>(result, mutationPoint, distance, test));
    }

    objectFilesWithMutant.pop_back();
  }

  return make_unique<Result>(std::move(foundTests), std::move(mutationResults), allMutationPoints);
}

void Driver::prepareForExecution() {
  assert(_callTreeMapping == nullptr && "Called twice?");
  assert(functions.size() > 1 && "Functions must be filled in before this call");

  /// Creating a memory to be shared between child and parent.
  _callTreeMapping = (uint64_t *) mmap(NULL,
                                       sizeof(uint64_t) * functions.size(),
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS,
                                       -1,
                                       0);
  memset(_callTreeMapping, 0, functions.size() * sizeof(_callTreeMapping[0]));
  dynamicCallTree.prepare(_callTreeMapping);
}

void Driver::injectCallbacks(llvm::Function *function, uint64_t index) {
  auto &context = function->getParent()->getContext();
  auto int64Type = Type::getInt64Ty(context);
  auto driverPointerType = Type::getVoidTy(context)->getPointerTo();
  auto voidType = Type::getVoidTy(context);
  std::vector<Type *> parameterTypes({driverPointerType, int64Type});

  FunctionType *callbackType = FunctionType::get(voidType, parameterTypes, false);

  Value *functionIndex = ConstantInt::get(int64Type, index);
  uint32_t pointerWidth = toolchain.targetMachine().createDataLayout().getPointerSize();
  ConstantInt *driverPointerAddress = ConstantInt::get(context, APInt(pointerWidth * 8, (orc::TargetAddress)this));
  Value *driverPointer = ConstantExpr::getCast(Instruction::IntToPtr,
                                               driverPointerAddress,
                                               int64Type->getPointerTo());
  std::vector<Value *> parameters({driverPointer, functionIndex});

  Function *enterFunction = function->getParent()->getFunction("mull_enterFunction");
  Function *leaveFunction = function->getParent()->getFunction("mull_leaveFunction");

  if (enterFunction == nullptr && leaveFunction == nullptr) {
    enterFunction = Function::Create(callbackType,
                                     Function::ExternalLinkage,
                                     "mull_enterFunction",
                                     function->getParent());

    leaveFunction = Function::Create(callbackType,
                                     Function::ExternalLinkage,
                                     "mull_leaveFunction",
                                     function->getParent());
  }

  assert(enterFunction);
  assert(leaveFunction);

  auto &entryBlock = *function->getBasicBlockList().begin();
  CallInst *enterFunctionCall = CallInst::Create(enterFunction, parameters);
  enterFunctionCall->insertBefore(&*entryBlock.getInstList().begin());

  for (auto &block : function->getBasicBlockList()) {
    ReturnInst *returnStatement = nullptr;
    if (!(returnStatement = dyn_cast<ReturnInst>(block.getTerminator()))) {
      continue;
    }

    CallInst *leaveFunctionCall = CallInst::Create(leaveFunction, parameters);
    leaveFunctionCall->insertBefore(returnStatement);
  }
}

std::vector<llvm::object::ObjectFile *> Driver::AllButOne(llvm::Module *One) {
  std::vector<llvm::object::ObjectFile *> Objects;

  for (auto &CachedEntry : InnerCache) {
    if (One != CachedEntry.first) {
      Objects.push_back(CachedEntry.second);
    }
  }

  for (OwningBinary<ObjectFile> &object: precompiledObjectFiles) {
    Objects.push_back(object.getBinary());
  }

  return Objects;
}

std::vector<llvm::object::ObjectFile *> Driver::AllObjectFiles() {
  std::vector<llvm::object::ObjectFile *> Objects;

  for (auto &CachedEntry : InnerCache) {
    Objects.push_back(CachedEntry.second);
  }

  for (OwningBinary<ObjectFile> &object: precompiledObjectFiles) {
    Objects.push_back(object.getBinary());
  }

  return Objects;
}
