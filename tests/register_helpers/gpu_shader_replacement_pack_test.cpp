#include <rex/graphics/pipeline/shader/replacement_pack.h>

#include <xxhash.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

void WriteU32(std::ostream& stream, uint32_t value) {
  const std::array<char, 4> bytes = {
      char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
  stream.write(bytes.data(), bytes.size());
}

void WriteU64(std::ostream& stream, uint64_t value) {
  WriteU32(stream, uint32_t(value));
  WriteU32(stream, uint32_t(value >> 32));
}

bool WritePack(const std::filesystem::path& path, uint64_t source_hash,
               const std::array<uint8_t, 12>& ucode) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  constexpr std::array<char, 8> magic = {'R', 'E', 'X', 'S', 'R', 'P', '1', 0};
  stream.write(magic.data(), magic.size());
  WriteU32(stream, 1);
  WriteU32(stream, 1);
  WriteU64(stream, source_hash);
  WriteU64(stream, XXH3_64bits(ucode.data(), ucode.size()));
  WriteU32(stream, uint32_t(rex::graphics::xenos::ShaderType::kPixel));
  WriteU32(stream, uint32_t(ucode.size()));
  stream.write(reinterpret_cast<const char*>(ucode.data()), ucode.size());
  return bool(stream);
}

}  // namespace

int main() {
  constexpr uint64_t source_hash = 0x1122334455667788ull;
  constexpr std::array<uint8_t, 12> ucode = {
      0x10, 0x2A, 0x11, 0x00, 0x12, 0x34,
      0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path pack_path =
      std::filesystem::temp_directory_path() /
      ("rex_shader_replacement_" + std::to_string(suffix) + ".xsrp");

  bool passed = Check(WritePack(pack_path, source_hash, ucode),
                      "could not write synthetic shader replacement pack");
  rex::graphics::ShaderReplacementPack pack;
  std::string error;
  passed &= Check(pack.Load(pack_path, &error), error.c_str());
  passed &= Check(pack.size() == 1, "replacement pack count is incorrect");
  const auto* replacement = pack.Find(
      rex::graphics::xenos::ShaderType::kPixel, source_hash);
  passed &= Check(replacement != nullptr, "pixel replacement was not found");
  if (replacement) {
    passed &= Check(replacement->replacement_hash ==
                        XXH3_64bits(ucode.data(), ucode.size()),
                    "replacement hash is incorrect");
    passed &= Check(replacement->ucode.size() * sizeof(uint32_t) ==
                        ucode.size() &&
                        std::memcmp(replacement->ucode.data(), ucode.data(),
                                    ucode.size()) == 0,
                    "replacement microcode bytes changed");
  }
  passed &= Check(pack.Find(rex::graphics::xenos::ShaderType::kVertex,
                            source_hash) == nullptr,
                  "replacement incorrectly crossed shader stages");

  if (passed) {
    std::fstream corrupt(pack_path, std::ios::binary | std::ios::in |
                                        std::ios::out);
    corrupt.seekp(-1, std::ios::end);
    const char changed = char(ucode.back() ^ 0xFF);
    corrupt.write(&changed, 1);
    corrupt.close();
    passed &= Check(!pack.Load(pack_path, &error),
                    "corrupt replacement microcode was accepted");
    passed &= Check(pack.size() == 0,
                    "failed pack load retained stale replacement data");
  }

  std::error_code remove_error;
  std::filesystem::remove(pack_path, remove_error);
  if (passed) {
    std::cout << "GPU shader replacement pack regression passed\n";
  }
  return passed ? 0 : 1;
}
