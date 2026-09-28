// V290: rex::filesystem::GetExecutablePath must work in the embedded title,
// whose host uses a narrow main. _get_wpgmptr is filled only for wide-entry
// programs; calling it there invokes the CRT invalid-parameter handler and
// terminates the process (seen as a 0xC0000409 fail-fast at startup).
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string path =
      std::string(REXGLUE_SOURCE_ROOT) + "/src/core/filesystem_win.cpp";
  std::ifstream file(path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  bool passed = !source.empty();
  if (!passed) std::cerr << "FAIL: filesystem_win.cpp not readable\n";
  if (source.find("_get_wpgmptr(") != std::string::npos) {
    std::cerr << "FAIL: GetExecutablePath must not use _get_wpgmptr\n";
    passed = false;
  }
  if (source.find("GetModuleFileNameW(nullptr") == std::string::npos) {
    std::cerr << "FAIL: GetExecutablePath must query the process module path\n";
    passed = false;
  }
  if (passed) std::cout << "Executable path policy passed\n";
  return passed ? 0 : 1;
}
