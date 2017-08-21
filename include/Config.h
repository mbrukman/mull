#pragma once

#include "Logger.h"
#include "MutationOperators/AddMutationOperator.h"
#include "MutationOperators/NegateConditionMutationOperator.h"
#include "MutationOperators/RemoveVoidFunctionMutationOperator.h"

#include "llvm/Support/YAMLTraits.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int MullDefaultTimeoutMilliseconds = 3000;

// We need these forward declarations to make our config friends with the
// mapping traits.
namespace mull {
class Config;
}
namespace llvm {
namespace yaml {
template<typename T>
struct MappingTraits;
}
}
namespace mull {

struct CustomTest {
  std::string name;
  std::string method;
};

class Config {
  std::string bitcodeFileList;

  std::string projectName;
  std::string testFramework;

  std::vector<std::string> mutationOperators;
  std::string dynamicLibraryFileList;
  std::vector<std::string> tests;
  std::vector<std::string> excludeLocations;
  std::vector<CustomTest> customTests;

  bool fork;
  bool dryRun;
  bool useCache;
  bool emitDebugInfo;
  int timeout;
  int maxDistance;
  std::string cacheDirectory;

  friend llvm::yaml::MappingTraits<mull::Config>;
public:
  // Constructor initializes defaults.
  // TODO: Refactoring into constants.
  Config() :
    bitcodeFileList(""),
    projectName(""),
    testFramework("GoogleTest"),
    mutationOperators(
      // Yaml::Traits stops reading mutation_operators from config.yaml
      // if these 3 default operators are set here (BUG?).
      // So leaving out the empty ()
      // {
      //   AddMutationOperator::ID,
      //   NegateConditionMutationOperator::ID,
      //   RemoveVoidFunctionMutationOperator::ID
      // }
    ),
    dynamicLibraryFileList(),
    tests(),
    excludeLocations(),
    customTests(),
    fork(true),
    dryRun(false),
    useCache(true),
    emitDebugInfo(false),
    timeout(MullDefaultTimeoutMilliseconds),
    maxDistance(128),
    cacheDirectory("/tmp/mull_cache")
  {
  }

  Config(const std::string &bitcodeFileList,
         const std::string &project,
         const std::string &testFramework,
         const std::vector<std::string> mutationOperators,
         const std::string &dynamicLibraryFileList,
         const std::vector<std::string> tests,
         const std::vector<std::string> excludeLocations,
         const std::vector<CustomTest> customTests,
         bool fork,
         bool dryrun,
         bool cache,
         bool debugInfo,
         int timeout,
         int distance,
         const std::string &cacheDir) :
    bitcodeFileList(bitcodeFileList),
    projectName(project),
    testFramework(testFramework),
    mutationOperators(mutationOperators),
    dynamicLibraryFileList(dynamicLibraryFileList),
    tests(tests),
    excludeLocations(excludeLocations),
    customTests(customTests),
    fork(fork),
    dryRun(dryrun),
    useCache(cache),
    emitDebugInfo(debugInfo),
    timeout(timeout),
    maxDistance(distance),
    cacheDirectory(cacheDir)
  {
  }

  const std::string &getBitcodeFileList() const {
    return bitcodeFileList;
  }

  std::vector<std::string> getBitcodePaths() const {
    std::vector<std::string> bitcodePaths;

    std::ifstream ifs(bitcodeFileList);

    for (std::string path; getline(ifs, path); ) {
      if (path.size() && path.at(0) == '#') {
        continue;
      }

      bitcodePaths.push_back(path);
    }

    return bitcodePaths;
  }

  const std::string &getProjectName() const {
    return projectName;
  }

  const std::string &getTestFramework() const {
    return testFramework;
  }

  const std::string &getDynamicLibraries() const {
    return dynamicLibraryFileList;
  }

  std::vector<std::string> getDynamicLibrariesPaths() const {
    std::vector<std::string> dynamicLibrariesPaths;

    std::ifstream ifs(dynamicLibraryFileList);

    for (std::string path; getline(ifs, path); ) {
      if (path.at(0) == '#') {
        continue;
      }

      dynamicLibrariesPaths.push_back(path);
    }

    return dynamicLibrariesPaths;
  }

  const std::vector<std::string> &getMutationOperators() const {
    return mutationOperators;
  }

  const std::vector<std::string> &getTests() const {
    return tests;
  }

  const std::vector<std::string> &getExcludeLocations() const {
    return excludeLocations;
  }

  std::vector<CustomTest> &getCustomTests() {
    return customTests;
  }

  const std::vector<CustomTest> &getCustomTests() const {
    return customTests;
  }

  bool getFork() const {
    return fork;
  }

  int getTimeout() const {
    return timeout;
  }

  bool getUseCache() const {
    return useCache;
  }

  bool isDryRun() const {
    return dryRun;
  }

  bool shouldEmitDebugInfo() const {
    return emitDebugInfo;
  }

  int getMaxDistance() const {
    return maxDistance;
  }

  std::string getCacheDirectory() const {
    return cacheDirectory;
  }

  void dump() const {
    Logger::debug() << "Config>\n"
    << "\t" << "bitcode_file_list: " << bitcodeFileList << '\n'
    << "\t" << "dynamic_library_file_list: " << dynamicLibraryFileList << '\n'
    << "\t" << "project_name: " << getProjectName() << '\n'
    << "\t" << "test_framework: " << getTestFramework() << '\n'
    << "\t" << "distance: " << getMaxDistance() << '\n'
    << "\t" << "dry_run: " << isDryRun() << '\n'
    << "\t" << "fork: " << getFork() << '\n'
    << "\t" << "emit_debug_info: " << shouldEmitDebugInfo() << '\n';

    if (mutationOperators.empty() == false) {
      Logger::debug() << "\t" << "mutation_operators: " << '\n';

      for (auto mutationOperator : mutationOperators) {
        Logger::debug() << "\t- " << mutationOperator << '\n';
      }
    }

    if (getTests().empty() == false) {
      Logger::debug() << "\t" << "tests: " << '\n';

      for (auto test : getTests()) {
        Logger::debug() << "\t- " << test << '\n';
      }
    }

    if (getExcludeLocations().empty() == false) {
      Logger::debug() << "\t" << "exclude_locations: " << '\n';

      for (auto excludeLocation : getExcludeLocations()) {
        Logger::debug() << "\t- " << excludeLocation << '\n';
      }
    }
    if (getCustomTests().empty() == false) {
      Logger::debug() << "\t" << "custom_tests: " << '\n';

      for (auto customTest : getCustomTests()) {
        Logger::debug() << "\t - name: " << customTest.name << '\n';
        Logger::debug() << "\t   method: " << customTest.method << '\n';
      }
    }
  }

  std::vector<std::string> validate() {
    std::vector<std::string> errors;

    if (bitcodeFileList.size() == 0) {
      std::string error = "bitcode_file_list parameter is not specified.";
      errors.push_back(error);
      return errors;
    }

    std::ifstream bitcodeFile(bitcodeFileList.c_str());

    if (bitcodeFile.good() == false) {
      std::stringstream error;

      error << "bitcode_file_list parameter points to a non-existing file: "
            << bitcodeFileList;

      errors.push_back(error.str());
    }

    if (dynamicLibraryFileList.empty() == false) {
      std::ifstream dynamicLibraryFile(dynamicLibraryFileList.c_str());

      if (dynamicLibraryFile.good() == false) {
        std::stringstream error;

        error << "dynamic_library_file_list parameter points to a non-existing file: "
        << dynamicLibraryFileList;

        errors.push_back(error.str());
      }
    }

    return errors;
  }

};
}
