#include "CustomTestFramework/CustomTestRunner.h"
#include "CustomTestFramework/CustomTest_Test.h"

#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>

using namespace mull;
using namespace llvm;
using namespace llvm::orc;

class Mull_CustomTest_Resolver : public RuntimeDyld::SymbolResolver {
  LocalCXXRuntimeOverrides &overrides;
public:

  Mull_CustomTest_Resolver(LocalCXXRuntimeOverrides &overrides)
    : overrides(overrides) {}

  RuntimeDyld::SymbolInfo findSymbol(const std::string &name) {
    if (auto symbol = overrides.searchOverrides(name)) {
      return symbol;
    }

    if (auto address = RTDyldMemoryManager::getSymbolAddressInProcess(name)) {
      return RuntimeDyld::SymbolInfo(address, JITSymbolFlags::Exported);
    }

    return RuntimeDyld::SymbolInfo(nullptr);
  }

  RuntimeDyld::SymbolInfo findSymbolInLogicalDylib(const std::string &Name) {
    return RuntimeDyld::SymbolInfo(nullptr);
  }
};

CustomTestRunner::CustomTestRunner(llvm::TargetMachine &machine) :
  TestRunner(machine),
  mangler(Mangler(machine.createDataLayout())),
  overrides([this](const char *name) {
    return this->mangler.getNameWithPrefix(name);
  })
{
}

void *CustomTestRunner::GetCtorPointer(const llvm::Function &Function) {
  return getFunctionPointer(mangler.getNameWithPrefix(Function.getName()));
}

void *CustomTestRunner::getFunctionPointer(const std::string &functionName) {
  JITSymbol symbol = ObjectLayer.findSymbol(functionName, false);

  void *fpointer =
    reinterpret_cast<void *>(static_cast<uintptr_t>(symbol.getAddress()));

  if (fpointer == nullptr) {
    errs() << "CustomTestRunner> Can't find pointer to function: "
    << functionName << "\n";
    exit(1);
  }

  return fpointer;
}

void CustomTestRunner::runStaticCtor(llvm::Function *Ctor) {
//  printf("Init: %s\n", Ctor->getName().str().c_str());

  void *CtorPointer = GetCtorPointer(*Ctor);

  auto ctor = ((int (*)())(intptr_t)CtorPointer);
  ctor();
}

ExecutionStatus CustomTestRunner::runTest(Test *test, ObjectFiles &objectFiles) {
  CustomTest_Test *customTest = dyn_cast<CustomTest_Test>(test);

  auto Handle =
    ObjectLayer.addObjectSet(objectFiles,
                             make_unique<SectionMemoryManager>(),
                             make_unique<Mull_CustomTest_Resolver>(overrides));

  for (auto &constructor: customTest->getConstructors()) {
    runStaticCtor(constructor);
  }

  std::vector<std::string> arguments = customTest->getArguments();
  arguments.insert(arguments.begin(), customTest->getProgramName());
  int argc = arguments.size();
  char **argv = new char*[argc + 1];

  for (int i = 0; i < argc; i++) {
    std::string &argument = arguments[i];
    argv[i] = new char[argument.length() + 1];
    strcpy(argv[i], argument.c_str());
  }
  argv[argc] = nullptr;

  void *mainPointer = getFunctionPointer(mangler.getNameWithPrefix("main"));
  auto main = ((int (*)(int, char**))(intptr_t)mainPointer);
  int exitStatus = main(argc, argv);

  for (int i = 0; i < argc; i++) {
    delete[] argv[i];
  }
  delete[] argv;

  overrides.runDestructors();

  ObjectLayer.removeObjectSet(Handle);

  if (exitStatus == 0) {
    return ExecutionStatus::Passed;
  }

  return ExecutionStatus::Failed;
}
