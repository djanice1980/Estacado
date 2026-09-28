#include "runtime_graphics_cache.h"
#include "runtime_package_validation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

void WriteFile(const std::filesystem::path& path, const char* contents) {
  std::ofstream(path, std::ios::binary) << contents;
}

void WriteManifest(const std::filesystem::path& root,
                   const std::string& executable_hash,
                   const std::string& runtime_hash,
                   const std::string& gpu_hash) {
  std::ofstream manifest(root / "TheDarkness.package.toml");
  manifest << "package_schema_version = 1\n"
              "[[file]]\npath = \"TheDarkness.exe\"\nsha256 = \""
           << executable_hash
           << "\"\n[[file]]\npath = \"rexruntime.dll\"\nsha256 = \""
           << runtime_hash
           << "\"\n[[file]]\npath = \"rexgpu-xenos.dll\"\nsha256 = \""
           << gpu_hash << "\"\n";
}
}  // namespace

int main() {
  bool passed = true;
  const auto root = std::filesystem::temp_directory_path() /
                    "darkness runtime package validation";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  const auto executable = root / "TheDarkness.exe";
  const auto runtime = root / "rexruntime.dll";
  const auto gpu = root / "rexgpu-xenos.dll";
  WriteFile(executable, "runtime executable");
  WriteFile(runtime, "runtime dll");
  WriteFile(gpu, "gpu dll");
  WriteManifest(root, RuntimeFileSha256(executable), RuntimeFileSha256(runtime),
                RuntimeFileSha256(gpu));

  const auto valid = ValidateRuntimePackage(root, true);
  passed &= Check(valid.exists && valid.valid && valid.errors.empty() &&
                      valid.files.size() == 3,
                  "matching core package files must validate");
  const auto lines = RuntimePackageValidationLines(valid);
  passed &= Check(std::find(lines.begin(), lines.end(), "PACKAGE_VALID=1") !=
                      lines.end(),
                  "package validation output must be machine readable");

  WriteFile(runtime, "stale runtime dll");
  const auto stale = ValidateRuntimePackage(root, true);
  passed &= Check(!stale.valid && !stale.errors.empty(),
                  "a stale mixed DLL must fail package validation");

  const auto traversal_root = root / "traversal";
  std::filesystem::create_directories(traversal_root, error);
  WriteFile(traversal_root / "TheDarkness.exe", "runtime executable");
  WriteFile(traversal_root / "rexruntime.dll", "runtime dll");
  WriteFile(traversal_root / "rexgpu-xenos.dll", "gpu dll");
  std::ofstream traversal(traversal_root / "TheDarkness.package.toml");
  traversal << "package_schema_version = 1\n"
               "[[file]]\npath = \"sub/../TheDarkness.exe\"\nsha256 = \""
            << RuntimeFileSha256(traversal_root / "TheDarkness.exe")
            << "\"\n";
  traversal.close();
  passed &= Check(!ValidateRuntimePackage(traversal_root, true).valid,
                  "non-normalized or escaping manifest paths must fail");

  const auto missing_root = root / "missing";
  std::filesystem::create_directories(missing_root, error);
  passed &= Check(ValidateRuntimePackage(missing_root, false).valid,
                  "a developer build may omit the package manifest");
  passed &= Check(!ValidateRuntimePackage(missing_root, true).valid,
                  "explicit package verification must require a manifest");

  std::filesystem::remove_all(root, error);
  if (passed) std::cout << "Runtime package validation tests passed\n";
  return passed ? 0 : 1;
}

