#include "CPPUnit/CPPUnitRunner.h"

#include "CPPUnit/CPPUnit_Test.h"

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/OrcMCJITReplacement.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"

#include <chrono>
#include <execinfo.h>

using namespace mull;
using namespace llvm;
using namespace std::chrono;
using namespace llvm::orc;

namespace {
  class UnitTest;
}

typedef void (*mull_destructor_t)(void *);

struct atexit_entry {
  mull_destructor_t destructor;
  void *arg;
  void *dso_handle;
};

const static int dtors_count = 64;
static int current_dtor = 0;
static atexit_entry dtors[dtors_count];

extern "C" int mull_cppunit_cxa_atexit(mull_destructor_t destructor, void *arg, void *__dso_handle) {
  assert(current_dtor < dtors_count);

#if 0
  void* callstack[128];
  int i, frames = backtrace(callstack, 128);
  char** strs = backtrace_symbols(callstack, frames);
  for (i = 0; i < frames; ++i) {
    printf("%s\n", strs[i]);
  }
  free(strs);
#endif

  for (int i = 0; i < current_dtor; i++) {
    if (arg == dtors[i].arg) {
//      printf("dtor already registered: %d: %p\n", i, arg);
      return 0;
    }
  }

//  printf("record dtor: %d: %p\n", current_dtor, arg);

  dtors[current_dtor].destructor = destructor;
  dtors[current_dtor].arg = arg;
  dtors[current_dtor].dso_handle = __dso_handle;

  current_dtor++;

  return 0;
}

static void runDestructors() {
//  printf("dtors: %d\n", current_dtor);
  while (current_dtor > 0) {
    current_dtor--;
//    printf("cleaning dtor: %d: %p\n", current_dtor, dtors[current_dtor].arg);
    dtors[current_dtor].destructor(dtors[current_dtor].arg);
  }
}

extern "C" void *mull_cppunit_dso_handle = nullptr;

class Mull_CPPUnit_Resolver : public RuntimeDyld::SymbolResolver {
public:

  RuntimeDyld::SymbolInfo findSymbol(const std::string &Name) {
    if (Name == "___cxa_atexit") {
      return findSymbol("mull_cppunit_cxa_atexit");
    }

    if (Name == "___dso_handle") {
      return findSymbol("mull_cppunit_dso_handle");
    }

    if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
      return RuntimeDyld::SymbolInfo(SymAddr, JITSymbolFlags::Exported);

    return RuntimeDyld::SymbolInfo(nullptr);
  }

  RuntimeDyld::SymbolInfo findSymbolInLogicalDylib(const std::string &Name) {
    return RuntimeDyld::SymbolInfo(nullptr);   }
};

CPPUnitRunner::CPPUnitRunner(llvm::TargetMachine &machine)
  : TestRunner(machine) {
    sys::DynamicLibrary::LoadLibraryPermanently("/usr/lib/i386-linux-gnu/libcppunit.so");
}

std::string CPPUnitRunner::MangleName(const llvm::StringRef &Name) {
  std::string MangledName;
  {
    raw_string_ostream Stream(MangledName);
    Mangler.getNameWithPrefix(Stream, Name, machine.createDataLayout());
  }
  return MangledName;
}

void *CPPUnitRunner::GetCtorPointer(const llvm::Function &Function) {
  return FunctionPointer(MangleName(Function.getName()).c_str());
}

void *CPPUnitRunner::FunctionPointer(const char *FunctionName) {
  JITSymbol Symbol = ObjectLayer.findSymbol(FunctionName, false);
  void *FPointer = reinterpret_cast<void *>(static_cast<uintptr_t>(Symbol.getAddress()));
  assert(FPointer && "Can't find pointer to function");
  return FPointer;
}

void CPPUnitRunner::runStaticCtor(llvm::Function *Ctor) {
//  printf("Init: %s\n", Ctor->getName().str().c_str());

  void *CtorPointer = GetCtorPointer(*Ctor);

  auto ctor = ((int (*)())(intptr_t)CtorPointer);
  ctor();
}

ExecutionResult CPPUnitRunner::runTest(Test *Test, ObjectFiles &ObjectFiles) {
  CPPUnit_Test *GTest = dyn_cast<CPPUnit_Test>(Test);

  auto Handle = ObjectLayer.addObjectSet(ObjectFiles,
                                         make_unique<SectionMemoryManager>(),
                                         make_unique<Mull_CPPUnit_Resolver>());

  auto start = high_resolution_clock::now();

  for (auto &Ctor: GTest->GetGlobalCtors()) {
    runStaticCtor(Ctor);
  }

  std::string filter = "--gtest_filter=" + GTest->getTestName();
  const char *argv[] = { "mull", filter.c_str(), NULL };
  int argc = 2;

  /// Normally Google Test Driver looks like this:
  ///
  ///   int main(int argc, char **argv) {
  ///     InitCPPUnit(&argc, argv);
  ///     return UnitTest.GetInstance()->Run();
  ///   }
  ///
  /// Technically we can just call `main` function, but there is a problem:
  /// Among all the files that are being processed may be more than one
  /// `main` function, therefore we can call wrong driver.
  ///
  /// To avoid this from happening we implement the driver function on our own.
  /// We must keep in mind that each project can have its own, extended
  /// version of the driver (LLVM itself has one).
  ///

  void *initGTestPtr = FunctionPointer("__ZN7testing14InitCPPUnitEPiPPc");
  auto initGTest = ((void (*)(int *, const char**))(intptr_t)initGTestPtr);
  initGTest(&argc, argv);

  void *getInstancePtr = FunctionPointer("__ZN7testing8UnitTest11GetInstanceEv");
  auto getInstance = ((UnitTest *(*)())(intptr_t)getInstancePtr);
  UnitTest *test = getInstance();

  void *runAllTestsPtr = FunctionPointer("__ZN7testing8UnitTest3RunEv");
  auto runAllTests = ((int (*)(UnitTest *))(intptr_t)runAllTestsPtr);
  uint64_t result = runAllTests(test);

  runDestructors();
  auto elapsed = high_resolution_clock::now() - start;

  //printf("%llu %s\n", result, GTest->getTestName().c_str());

  ExecutionResult Result;
  Result.RunningTime = duration_cast<std::chrono::milliseconds>(elapsed).count();

  ObjectLayer.removeObjectSet(Handle);

  if (result == 0) {
    Result.Status = ExecutionStatus::Passed;
  } else {
    Result.Status = ExecutionStatus::Failed;
  }

  return Result;
}
