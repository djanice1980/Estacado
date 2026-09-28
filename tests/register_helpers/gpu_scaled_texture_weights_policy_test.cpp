#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef REXGLUE_SOURCE_ROOT
#error REXGLUE_SOURCE_ROOT must name the ReXGlue source directory
#endif

int main() {
  const std::string path =
      std::string(REXGLUE_SOURCE_ROOT) +
      "/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp";
  std::ifstream file(path, std::ios::binary);
  const std::string source{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
  const std::string branch_marker =
      "if (instr.opcode == FetchOpcode::kGetTextureWeights)";
  const size_t scaling_context =
      source.find("uint32_t revert_resolution_scale_axes");
  const size_t branch_begin = source.find(branch_marker, scaling_context);
  const size_t branch_end = source.find("  } else {", branch_begin);
  bool passed = branch_begin != std::string::npos &&
                branch_end != std::string::npos;
  if (!passed) {
    std::cerr << "Unable to isolate the GetTextureWeights translation branch\n";
    return 1;
  }

  const std::string branch =
      source.substr(branch_begin, branch_end - branch_begin);
  if (branch.find("guest texel space") == std::string::npos) {
    std::cerr << "Missing guest-texel-space invariant documentation\n";
    passed = false;
  }
  if (branch.find("resolution_scaled_result_components") !=
          std::string::npos ||
      branch.find("resolution_scale_src") != std::string::npos ||
      branch.find("kTexturesResolutionScaled") != std::string::npos) {
    std::cerr << "GetTextureWeights still applies host resolution scale\n";
    passed = false;
  }

  if (passed) {
    std::cout << "GPU scaled texture weights policy tests passed\n";
  }
  return passed ? 0 : 1;
}
