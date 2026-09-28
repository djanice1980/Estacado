#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef REXGLUE_SOURCE_ROOT
#error REXGLUE_SOURCE_ROOT must name the ReXGlue source directory
#endif

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  const std::string root =
      std::string(REXGLUE_SOURCE_ROOT) +
      "/src/graphics/shaders/bytecode/d3d12_5_1/";
  // The canonical EDRAM resolve shaders use raw UAV byte addresses. The
  // second 64bpp output group therefore advances 32 bytes at native scale and
  // 16 bytes in the scaled variant. These are byte-address equivalents of
  // the previous typed-UAV +2 / +1 element offsets.
  const std::vector<std::pair<std::string, std::string>> expectations = {
      {"resolve_fast_64bpp_1x2xmsaa_cs.h", "iadd r0.y, r0.y, l(32)"},
      {"resolve_fast_64bpp_1x2xmsaa_scaled_cs.h",
       "iadd r0.w, r0.y, l(16)"},
      {"resolve_fast_64bpp_4xmsaa_cs.h", "iadd r0.y, r0.y, l(32)"},
      {"resolve_fast_64bpp_4xmsaa_scaled_cs.h",
       "iadd r0.w, r0.y, l(16)"},
      {"resolve_full_128bpp_cs.h", "iadd r0.y, r0.y, l(32)"},
      {"resolve_full_128bpp_scaled_cs.h", "iadd r0.w, r0.y, l(16)"},
      {"resolve_full_64bpp_cs.h", "iadd r0.y, r0.y, l(32)"},
      {"resolve_full_64bpp_scaled_cs.h", "iadd r0.w, r0.y, l(16)"},
      {"texture_load_8bpb_cs.h", "iadd r0.x, r0.x, r0.y"},
      {"texture_load_8bpb_scaled_cs.h", "iadd r0.x, r0.x, l(1)"},
  };

  bool passed = true;
  for (const auto& expectation : expectations) {
    const std::string source = ReadFile(root + expectation.first);
    if (source.empty()) {
      std::cerr << "Unable to read generated shader header: "
                << expectation.first << "\n";
      passed = false;
      continue;
    }
    if (source.find(expectation.second) == std::string::npos) {
      std::cerr << "Missing local-X additive offset in "
                << expectation.first << ": " << expectation.second << "\n";
      passed = false;
    }
  }

  if (passed) {
    std::cout << "GPU local-X address policy tests passed\n";
  }
  return passed ? 0 : 1;
}
