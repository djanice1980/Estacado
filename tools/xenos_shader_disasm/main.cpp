#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

// Shader analysis only queries this graphics cvar to decide whether to emit a
// second copy of the microcode. Keep the standalone tool's storage local and
// empty rather than linking the full GPU plugin flag registry.
std::string& FLAGS_dump_shaders_storage_() {
  static std::string value;
  return value;
}

namespace {

void PrintUsage(const char* executable) {
  std::cerr << "Usage: " << executable
            << " <vs|ps> <input.bin> [output.txt] [--little-endian]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    PrintUsage(argv[0]);
    return 2;
  }

  rex::graphics::xenos::ShaderType shader_type;
  const std::string type_argument = argv[1];
  if (type_argument == "vs") {
    shader_type = rex::graphics::xenos::ShaderType::kVertex;
  } else if (type_argument == "ps") {
    shader_type = rex::graphics::xenos::ShaderType::kPixel;
  } else {
    PrintUsage(argv[0]);
    return 2;
  }

  std::filesystem::path output_path;
  bool little_endian = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--little-endian") {
      little_endian = true;
    } else if (output_path.empty()) {
      output_path = argv[i];
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  const std::filesystem::path input_path = argv[2];
  std::ifstream input(input_path, std::ios::binary | std::ios::ate);
  if (!input) {
    std::cerr << "Unable to open input: " << input_path << '\n';
    return 1;
  }
  const std::streamoff byte_count = input.tellg();
  if (byte_count <= 0 || (byte_count % sizeof(uint32_t)) != 0) {
    std::cerr << "Shader size must be a non-zero multiple of four bytes: "
              << byte_count << '\n';
    return 1;
  }
  input.seekg(0);
  std::vector<uint32_t> ucode(static_cast<size_t>(byte_count) /
                              sizeof(uint32_t));
  input.read(reinterpret_cast<char*>(ucode.data()), byte_count);
  if (!input) {
    std::cerr << "Unable to read complete input: " << input_path << '\n';
    return 1;
  }

  rex::graphics::Shader shader(
      shader_type, 0, ucode.data(), ucode.size(),
      little_endian ? std::endian::little : std::endian::big);
  rex::string::StringBuffer disassembly_buffer;
  shader.AnalyzeUcode(disassembly_buffer);

  if (output_path.empty()) {
    std::cout << shader.ucode_disassembly();
  } else {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      std::cerr << "Unable to create output: " << output_path << '\n';
      return 1;
    }
    output.write(shader.ucode_disassembly().data(),
                 static_cast<std::streamsize>(
                     shader.ucode_disassembly().size()));
    if (!output) {
      std::cerr << "Unable to write complete output: " << output_path << '\n';
      return 1;
    }
  }
  return 0;
}
