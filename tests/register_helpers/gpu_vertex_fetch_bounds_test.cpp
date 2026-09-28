#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

std::array<uint32_t, 4> GetWordMasks(uint32_t buffer_base,
                                     uint32_t buffer_size_words,
                                     uint32_t first_loaded_word_address,
                                     uint32_t first_word_index) {
  const uint64_t buffer_end = uint64_t(buffer_base) +
                              uint64_t(buffer_size_words) * sizeof(uint32_t);
  std::array<uint32_t, 4> masks{};
  for (uint32_t word = 0; word < masks.size(); ++word) {
    const int64_t address = int64_t(first_loaded_word_address) +
                            (int64_t(word) - int64_t(first_word_index)) * 4;
    masks[word] = address >= 0 && uint64_t(address) < buffer_end
                      ? UINT32_MAX
                      : 0;
  }
  return masks;
}

}  // namespace

int main() {
  bool passed = true;

  const auto final_word = GetWordMasks(0x1000, 4, 0x100C, 0);
  passed &= Check(final_word[0] == UINT32_MAX && final_word[1] == 0 &&
                      final_word[2] == 0 && final_word[3] == 0,
                  "words after the exclusive fetch-buffer end must clamp");

  // Xenos/Xenia validate vertex fetches against the exclusive upper end of
  // the declared buffer. They deliberately don't add a lower-bound check:
  // element-relative loads may begin before the fetch base while still being
  // below the encoded end address. Keep this asymmetry explicit so a future
  // cleanup can't reintroduce the stretched-geometry regression.
  const auto before_base = GetWordMasks(0x1000, 4, 0x0FFC, 0);
  passed &= Check(before_base[0] == UINT32_MAX &&
                      before_base[1] == UINT32_MAX &&
                      before_base[2] == UINT32_MAX &&
                      before_base[3] == UINT32_MAX,
                  "fetch words below the base must not gain a lower-bound clamp");

  const auto partial_element = GetWordMasks(0x2000, 3, 0x2008, 2);
  passed &= Check(partial_element[0] == UINT32_MAX &&
                      partial_element[1] == UINT32_MAX &&
                      partial_element[2] == UINT32_MAX &&
                      partial_element[3] == 0,
                  "element-relative word masks are incorrect");

  const auto empty = GetWordMasks(0x3000, 0, 0x3000, 0);
  passed &= Check(empty[0] == 0 && empty[1] == 0 && empty[2] == 0 &&
                      empty[3] == 0,
                  "an empty fetch buffer must return only zero words");

  const std::string source_path =
      std::string(REXGLUE_SOURCE_ROOT) +
      "/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp";
  std::ifstream source_file(source_path, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  passed &= Check(!source.empty(), "DXBC translator source was not readable");
  passed &= Check(source.find("a_.OpUBFE(dxbc::Dest::R(bounds_temp") !=
                      std::string::npos,
                  "fetch-buffer word count is not extracted in DXBC");
  passed &= Check(source.find("a_.OpULT(dxbc::Dest::R(word_mask_temp") !=
                      std::string::npos,
                  "DXBC fetch words are not bounds-tested");
  passed &= Check(source.find("dxbc::Src::R(word_mask_temp));") !=
                      std::string::npos,
                  "DXBC fetched data is not zero-masked");
  passed &= Check(source.find("Bound checking is not done here") ==
                      std::string::npos,
                  "the obsolete no-bounds-check path returned");

  const std::string translator_header_path =
      std::string(REXGLUE_SOURCE_ROOT) +
      "/include/rex/graphics/pipeline/shader/dxbc_translator.h";
  std::ifstream translator_header_file(translator_header_path,
                                       std::ios::binary);
  const std::string translator_header(
      (std::istreambuf_iterator<char>(translator_header_file)),
      std::istreambuf_iterator<char>());
  passed &= Check(!translator_header.empty(),
                  "DXBC translator header was not readable");
  passed &= Check(
      translator_header.find(
          "static constexpr uint32_t kVersion = 0x20260908;") !=
          std::string::npos,
      "vertex-fetch correction lost the current persistent pipeline-cache version bump");

  if (passed) {
    std::cout << "Xenos vertex-fetch bounds regression passed\n";
  }
  return passed ? 0 : 1;
}
