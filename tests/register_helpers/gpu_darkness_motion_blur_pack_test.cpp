#include <rex/graphics/pipeline/shader/replacement_pack.h>

#include <xxhash.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>

namespace {

struct ExpectedPatch {
  uint64_t source_hash;
  uint64_t replacement_hash;
  size_t ucode_bytes;
  size_t instruction_offset;
};

constexpr std::array<ExpectedPatch, 8> kExpectedPatches = {{
    {0x5466CF9942F35964ull, 0xA23C29A98EDD487Aull, 324, 0x30},
    {0xA167D4EAF622F7C2ull, 0xDC82C28DF3BC0175ull, 408, 0x30},
    {0xED6B74C3B52BE18Full, 0xB9EB15C32F3E6392ull, 468, 0x3C},
    {0x41AA202FFE632222ull, 0x2491B6393ABD9161ull, 552, 0x48},
    {0x147A61D164BBEBB9ull, 0x1DDE1237B7B93292ull, 408, 0x54},
    {0x7C6AF8B69563DB69ull, 0xFFB9ED0143D987C1ull, 492, 0x54},
    {0xF7B11AA01AB700F3ull, 0x77D4077FA59126E3ull, 540, 0x54},
    {0xA59B41D0BD79484Bull, 0x22FC55CE134777ACull, 624, 0x60},
}};

uint32_t ReadBigEndianU32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  const std::filesystem::path pack_path =
      std::filesystem::path(DARKNESS_SOURCE_ROOT) / "build" / "runtime_data" /
      "TheDarkness.motion_blur_off.xsrp";
  rex::graphics::ShaderReplacementPack pack;
  std::string error;
  bool passed = Check(pack.Load(pack_path, &error), error.c_str());
  passed &= Check(pack.size() == kExpectedPatches.size(),
                  "The Darkness motion-blur pack count is incorrect");

  for (const ExpectedPatch& expected : kExpectedPatches) {
    const auto* replacement = pack.Find(
        rex::graphics::xenos::ShaderType::kPixel, expected.source_hash);
    passed &= Check(replacement != nullptr,
                    "The Darkness motion-on shader mapping is missing");
    if (!replacement) {
      continue;
    }
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(replacement->ucode.data());
    const size_t byte_count = replacement->ucode.size() * sizeof(uint32_t);
    passed &= Check(byte_count == expected.ucode_bytes,
                    "patched shader changed the source contract size");
    passed &= Check(replacement->replacement_hash == expected.replacement_hash,
                    "patched shader identity is unexpected");
    passed &= Check(XXH3_64bits(bytes, byte_count) ==
                        expected.replacement_hash,
                    "patched shader hash does not match its bytes");
    if (byte_count >= expected.instruction_offset + 12) {
      passed &= Check(
          ReadBigEndianU32(bytes + expected.instruction_offset) == 0xC80C0000u,
          "motion-coordinate MAD destination contract changed");
      passed &= Check(
          ReadBigEndianU32(bytes + expected.instruction_offset + 8) ==
              0x8B00FCFCu,
          "motion-coordinate MAD does not read verified zero constants");
    } else {
      passed &= Check(false, "motion-coordinate MAD is outside the shader");
    }
    passed &= Check(
        pack.Find(rex::graphics::xenos::ShaderType::kVertex,
                  expected.source_hash) == nullptr,
        "motion-blur replacement crossed shader stages");
  }

  if (passed) {
    std::cout << "The Darkness motion-blur shader contract regression passed\n";
  }
  return passed ? 0 : 1;
}
