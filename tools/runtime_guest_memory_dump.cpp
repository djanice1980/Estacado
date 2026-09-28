#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint64_t Parse(const char* value, const char* name) {
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(value, &end, 0);
  if (errno || !end || *end) {
    throw std::runtime_error(std::string("invalid ") + name);
  }
  return parsed;
}

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes) {
  return (std::uint32_t(bytes[0]) << 24) |
         (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: runtime_guest_memory_dump <pid> <guest-memory-base> "
                 "<guest-address> <size> <output-file>\n";
    return 2;
  }

  try {
    const auto pidValue = Parse(argv[1], "pid");
    const auto memoryBase = Parse(argv[2], "guest memory base");
    const auto guestAddress = Parse(argv[3], "guest address");
    const auto sizeValue = Parse(argv[4], "size");
    if (!pidValue || pidValue > std::numeric_limits<DWORD>::max() ||
        guestAddress > std::numeric_limits<std::uint32_t>::max() ||
        !sizeValue || sizeValue > 256u * 1024u * 1024u ||
        memoryBase > std::numeric_limits<std::uint64_t>::max() - guestAddress) {
      throw std::runtime_error("numeric argument out of range");
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, static_cast<DWORD>(pidValue));
    if (!process) throw std::runtime_error("unable to open process");

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sizeValue));
    SIZE_T bytesRead = 0;
    const auto hostAddress = memoryBase + guestAddress;
    const bool read = ReadProcessMemory(
        process, reinterpret_cast<const void*>(hostAddress), bytes.data(),
        bytes.size(), &bytesRead) != FALSE;
    CloseHandle(process);
    if (!read || bytesRead != bytes.size()) {
      throw std::runtime_error("unable to read complete guest range");
    }

    const std::filesystem::path output =
        std::filesystem::absolute(std::filesystem::path(argv[5]));
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("unable to open output file");
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("unable to write output file");

    std::cout << "RUNTIME_GUEST_MEMORY_DUMP pid=" << pidValue
              << " memory_base=0x" << std::hex << memoryBase
              << " guest_address=0x" << guestAddress << " host_address=0x"
              << hostAddress << std::dec << " size=" << bytes.size()
              << " output=" << output.string() << '\n';
    const auto previewWords = std::min<std::size_t>(bytes.size() / 4, 16);
    for (std::size_t index = 0; index < previewWords; ++index) {
      std::cout << "  +0x" << std::hex << std::setw(4) << std::setfill('0')
                << index * 4 << "=0x" << std::setw(8)
                << ReadBigEndian32(bytes.data() + index * 4) << std::dec
                << std::setfill(' ') << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "runtime_guest_memory_dump: " << error.what() << '\n';
    return 1;
  }
}
