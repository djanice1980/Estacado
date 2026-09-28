#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool CheckContains(const std::string& source, const char* token,
                   const char* message) {
  if (source.find(token) != std::string::npos) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

bool CheckNotContains(const std::string& source, const char* token,
                      const char* message) {
  if (source.find(token) == std::string::npos) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

}  // namespace

int main() {
  const std::string source_path =
      std::string(DARKNESS_SOURCE_ROOT) + "/runtime/CMakeLists.txt";
  std::ifstream source_file(source_path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());

  bool passed = !source.empty();
  if (!passed) {
    std::cerr << "runtime CMake source was not readable\n";
  }
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/O2>",
      "TheDarkness target lost its Release /O2 contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/Ob2>",
      "TheDarkness target lost its Release /Ob2 contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:NDEBUG>",
      "TheDarkness target lost its Release NDEBUG contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/INCREMENTAL:NO>",
      "TheDarkness target lost its non-incremental Release link contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/OPT:REF>",
      "TheDarkness target lost its Release dead-code elimination contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/OPT:ICF>",
      "TheDarkness target lost its Release identical-COMDAT contract");
  passed &= CheckContains(
      source, "$<$<CONFIG:Release>:/Brepro>",
      "TheDarkness target lost its deterministic PE timestamp contract");
  passed &= CheckContains(
      source, "apply_darkness_release_policy(TheDarkness)",
      "TheDarkness target no longer uses the shared Release contract");
  passed &= CheckContains(
      source, "apply_darkness_release_policy(TheDarknessSettings)",
      "settings utility lost the shared Release contract");
  passed &= CheckContains(
      source, "add_dependencies(TheDarkness TheDarknessSettings)",
      "runtime build no longer includes the settings utility");
  passed &= CheckContains(
      source, "add_custom_target(TheDarknessPcPresets",
      "settings utility build no longer declares packaged preset outputs");
  passed &= CheckContains(
      source, "add_dependencies(TheDarknessSettings TheDarknessPcPresets)",
      "settings utility no longer depends on its packaged presets");
  passed &= CheckContains(
      source, "../config/pc_presets/${preset_name}.toml",
      "settings utility presets no longer come from the source-controlled schema set");
  passed &= CheckContains(
      source, "out/win-amd64/$<CONFIG>",
      "multi-config runtime deployment no longer selects matching ReXGlue DLLs");
  passed &= CheckContains(
      source, "out/win-amd64/${CMAKE_BUILD_TYPE}",
      "single-config runtime deployment no longer selects matching ReXGlue DLLs");

  const std::string rexruntime_path = std::string(DARKNESS_SOURCE_ROOT) +
                                      "/external/ReXGlue/src/system/CMakeLists.txt";
  std::ifstream rexruntime_file(rexruntime_path, std::ios::binary);
  const std::string rexruntime_source(
      (std::istreambuf_iterator<char>(rexruntime_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      rexruntime_source, "$<$<CONFIG:Release>:LINKER:/Brepro>",
      "packaged ReXGlue runtime lost its deterministic PE timestamp contract");

  const std::string rexgpu_path = std::string(DARKNESS_SOURCE_ROOT) +
                                  "/external/ReXGlue/src/graphics/CMakeLists.txt";
  std::ifstream rexgpu_file(rexgpu_path, std::ios::binary);
  const std::string rexgpu_source(
      (std::istreambuf_iterator<char>(rexgpu_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      rexgpu_source, "$<$<CONFIG:Release>:LINKER:/Brepro>",
      "packaged ReXGlue GPU lost its deterministic PE timestamp contract");

  const std::string rex_root_path = std::string(DARKNESS_SOURCE_ROOT) +
                                    "/external/ReXGlue/CMakeLists.txt";
  std::ifstream rex_root_file(rex_root_path, std::ios::binary);
  const std::string rex_root_source(
      (std::istreambuf_iterator<char>(rex_root_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      rex_root_source, "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>",
      "clean ReXGlue clang-cl builds lost explicit C++ exception handling");
  passed &= CheckContains(
      rex_root_source, "$<$<CONFIG:RELEASE>:/O2>",
      "clean ReXGlue clang-cl builds lost effective Release optimization");

  const std::string rex_presets_path = std::string(DARKNESS_SOURCE_ROOT) +
                                       "/external/ReXGlue/CMakePresets.json";
  std::ifstream rex_presets_file(rex_presets_path, std::ios::binary);
  const std::string rex_presets_source(
      (std::istreambuf_iterator<char>(rex_presets_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      rex_presets_source, "\"CMAKE_CXX_COMPILER\": \"clang-cl\"",
      "ReXGlue Windows preset no longer selects the supported clang-cl mode");
  passed &= CheckContains(
      rex_presets_source, "\"CMAKE_CXX_FLAGS\": \"/clang:-march=x86-64-v3\"",
      "ReXGlue Windows preset lost its clang-cl architecture flag spelling");
  passed &= CheckContains(
      rex_presets_source, "\"REXGLUE_ENABLE_TRACY\": false",
      "ReXGlue Windows preset no longer disables incompatible Tracy profiling");
  passed &= CheckContains(
      rex_presets_source, "\"REXGLUE_ENABLE_PERF_COUNTERS\": false",
      "ReXGlue Windows preset no longer disables Release perf counters");

  const std::string rex_version_path = std::string(DARKNESS_SOURCE_ROOT) +
                                       "/external/ReXGlue/cmake/rex_version.cmake";
  std::ifstream rex_version_file(rex_version_path, std::ios::binary);
  const std::string rex_version_source(
      (std::istreambuf_iterator<char>(rex_version_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      rex_version_source, "rev-parse --short=8 HEAD",
      "ReXGlue untagged-build provenance lost its exact commit fallback");
  passed &= CheckContains(
      rex_version_source, "0.0-dev.g${ARG_GIT_REV_PARSE}",
      "ReXGlue untagged version no longer records its pinned commit");

  const std::string rex_header_path = std::string(DARKNESS_SOURCE_ROOT) +
                                      "/external/ReXGlue/include/rex/version.h.in";
  std::ifstream rex_header_file(rex_header_path, std::ios::binary);
  const std::string rex_header_source(
      (std::istreambuf_iterator<char>(rex_header_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckNotContains(
      rex_header_source, "REXGLUE_BUILD_TIMESTAMP",
      "ReXGlue build metadata regained a configure-time wall clock");

  const std::string source_identity_path =
      std::string(DARKNESS_SOURCE_ROOT) +
      "/scripts/get-rexglue-source-identity.ps1";
  std::ifstream source_identity_file(source_identity_path, std::ios::binary);
  const std::string source_identity_source(
      (std::istreambuf_iterator<char>(source_identity_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      source_identity_source, "ls-files",
      "ReXGlue provenance lost tracked and untracked source enumeration");
  passed &= CheckContains(
      source_identity_source, "IncrementalHash",
      "ReXGlue provenance lost its deterministic source-tree digest");
  passed &= CheckContains(
      source_identity_source, "System.Collections.Generic.List[string]",
      "ReXGlue provenance lost shell-independent ordinal path ordering");
  passed &= CheckNotContains(
      source_identity_source, "[Array]::Sort",
      "ReXGlue provenance regained shell-dependent PowerShell array sorting");
  passed &= CheckContains(
      source_identity_source, "[BitConverter]::ToString",
      "ReXGlue provenance lost Windows PowerShell 5.1-compatible hex encoding");
  passed &= CheckNotContains(
      source_identity_source, "[Convert]::ToHexString",
      "ReXGlue provenance regained a PowerShell 7-only .NET hex API");
  passed &= CheckContains(
      source_identity_source, "submodule status --recursive",
      "ReXGlue provenance lost recursive submodule identity");

  const std::string candidate_verifier_path =
      std::string(DARKNESS_SOURCE_ROOT) +
      "/scripts/verify-pc-phase1-candidate.ps1";
  std::ifstream candidate_verifier_file(candidate_verifier_path,
                                        std::ios::binary);
  const std::string candidate_verifier_source(
      (std::istreambuf_iterator<char>(candidate_verifier_file)),
      std::istreambuf_iterator<char>());
  passed &= CheckContains(
      candidate_verifier_source, "ExpectedSourceTreeSHA256",
      "candidate verification no longer enforces patched ReXGlue source identity");
  passed &= CheckContains(
      candidate_verifier_source, "[switch]$SealedPackageOnly",
      "candidate verification lost explicit historical sealed-package mode");
  passed &= CheckContains(
      candidate_verifier_source, "$manifest.source_build -and -not $SealedPackageOnly",
      "sealed-package mode no longer isolates current-worktree provenance");
  passed &= CheckContains(
      candidate_verifier_source, "--verify-package",
      "sealed-package mode lost executable-internal manifest verification");
  passed &= CheckContains(
      candidate_verifier_source, "PACKAGE_ERROR_COUNT=0",
      "sealed-package mode no longer requires zero internal package errors");

  if (passed) {
    std::cout << "runtime Release policy source regression passed\n";
  }
  return passed ? 0 : 1;
}
