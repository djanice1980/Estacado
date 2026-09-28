#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace {

uint32_t ReadBigEndianU32(std::span<const uint8_t> data, size_t offset) {
  return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
         (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("Unable to open input file");
  }
  const auto size = stream.tellg();
  if (size < 0) {
    throw std::runtime_error("Unable to determine input size");
  }
  std::vector<uint8_t> data(static_cast<size_t>(size));
  stream.seekg(0);
  if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), size)) {
    throw std::runtime_error("Unable to read input file");
  }
  return data;
}

void WriteHex64(std::ostream& stream, uint64_t value) {
  stream << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value
         << std::dec << std::setfill(' ');
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--hash") {
    try {
      const std::filesystem::path input_path(argv[2]);
      const std::vector<uint8_t> bytes = ReadFile(input_path);
      std::cout << "XXH3=0x" << std::uppercase << std::hex << std::setw(16)
                << std::setfill('0') << XXH3_64bits(bytes.data(), bytes.size())
                << std::dec << std::setfill(' ') << " bytes=" << bytes.size()
                << " input=" << input_path.string() << '\n';
      return bytes.empty() ? 1 : 0;
    } catch (const std::exception& exception) {
      std::cerr << "xenos_shader_catalog: " << exception.what() << '\n';
      return 1;
    }
  }
  if (argc != 3) {
    std::cerr << "usage: xenos_shader_catalog <ProgramCache.xpc> <manifest.csv>\n"
                 "       xenos_shader_catalog --hash <binary>\n";
    return 2;
  }

  try {
    const std::filesystem::path input_path(argv[1]);
    const std::filesystem::path output_path(argv[2]);
    const std::vector<uint8_t> bytes = ReadFile(input_path);
    const std::span<const uint8_t> data(bytes);

    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Unable to create output manifest");
    }
    output << "Ordinal,Stage,CacheOffset,Flags,VirtualSize,PhysicalSize,ContainerSize,"
              "ShaderOffset,UcodePhysicalOffset,UcodeSize,ContainerXXH3,UcodeXXH3\n";

    size_t count = 0;
    size_t pixel_count = 0;
    size_t vertex_count = 0;
    for (size_t offset = 0; offset + 36 <= data.size(); ++offset) {
      const uint32_t flags = ReadBigEndianU32(data, offset);
      if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
        continue;
      }

      const uint32_t virtual_size = ReadBigEndianU32(data, offset + 4);
      const uint32_t physical_size = ReadBigEndianU32(data, offset + 8);
      const uint32_t constant_table_offset = ReadBigEndianU32(data, offset + 16);
      const uint32_t shader_offset = ReadBigEndianU32(data, offset + 24);
      const uint32_t field_1c = ReadBigEndianU32(data, offset + 28);
      const uint32_t field_20 = ReadBigEndianU32(data, offset + 32);
      const uint64_t container_size = uint64_t(virtual_size) + uint64_t(physical_size);

      if (container_size < 36 || container_size > data.size() - offset || field_1c != 0 ||
          field_20 != 0 || constant_table_offset == 0 ||
          constant_table_offset >= virtual_size || shader_offset == 0 ||
          shader_offset + 24 > virtual_size) {
        continue;
      }

      const size_t shader_header = offset + shader_offset;
      const uint32_t ucode_physical_offset = ReadBigEndianU32(data, shader_header);
      const uint32_t ucode_size = ReadBigEndianU32(data, shader_header + 4);
      const uint64_t ucode_relative_offset = uint64_t(virtual_size) + ucode_physical_offset;
      if ((ucode_size & 3u) != 0 || ucode_relative_offset > container_size ||
          ucode_size > container_size - ucode_relative_offset) {
        continue;
      }

      const bool is_vertex = (flags & 1u) != 0;
      const char* stage = is_vertex ? "vs" : "ps";
      vertex_count += is_vertex ? 1 : 0;
      pixel_count += is_vertex ? 0 : 1;

      const uint8_t* container_data = data.data() + offset;
      const uint8_t* ucode_data = container_data + ucode_relative_offset;
      const uint64_t container_hash = XXH3_64bits(container_data, size_t(container_size));
      const uint64_t ucode_hash = XXH3_64bits(ucode_data, ucode_size);

      output << count << ',' << stage << ",0x" << std::uppercase << std::hex << std::setw(8)
             << std::setfill('0') << offset << ",0x" << std::setw(8) << flags << std::dec
             << std::setfill(' ') << ',' << virtual_size << ',' << physical_size << ','
             << container_size << ',' << shader_offset << ',' << ucode_physical_offset << ','
             << ucode_size << ',';
      WriteHex64(output, container_hash);
      output << ',';
      WriteHex64(output, ucode_hash);
      output << '\n';
      ++count;
    }

    std::cout << "XENOS_SHADER_CATALOG input=" << input_path.string() << " total=" << count
              << " ps=" << pixel_count << " vs=" << vertex_count
              << " output=" << output_path.string() << '\n';
    return count == 0 ? 1 : 0;
  } catch (const std::exception& exception) {
    std::cerr << "xenos_shader_catalog: " << exception.what() << '\n';
    return 1;
  }
}
