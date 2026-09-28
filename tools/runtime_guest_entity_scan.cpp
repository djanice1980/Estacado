#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
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

std::uint16_t BigEndian16(const std::uint8_t* bytes) {
  return std::uint16_t((std::uint16_t(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t BigEndian32(const std::uint8_t* bytes) {
  return (std::uint32_t(bytes[0]) << 24) |
         (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

bool Read(HANDLE process, std::uint64_t address, void* output,
          std::size_t size) {
  SIZE_T bytesRead = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                           output, size, &bytesRead) != FALSE &&
         bytesRead == size;
}

struct VtableAggregate {
  std::uint32_t count{};
  std::uint32_t validated{};
  std::uint32_t firstId{};
  std::uint32_t firstAddress{};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: runtime_guest_entity_scan <pid> <guest-memory-base> "
                 "<pointer-table> <entry-count> <id-base> <output.csv>\n";
    return 2;
  }
  try {
    const auto pidValue = Parse(argv[1], "pid");
    const auto memoryBase = Parse(argv[2], "guest memory base");
    const auto tableAddress = Parse(argv[3], "pointer table");
    const auto entryCount = Parse(argv[4], "entry count");
    const auto idBase = Parse(argv[5], "id base");
    if (!pidValue || pidValue > std::numeric_limits<DWORD>::max() ||
        tableAddress > std::numeric_limits<std::uint32_t>::max() ||
        !entryCount || entryCount > 0x100000u ||
        idBase > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("numeric argument out of range");
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                 FALSE, static_cast<DWORD>(pidValue));
    if (!process) throw std::runtime_error("unable to open process");

    std::vector<std::uint8_t> table(static_cast<std::size_t>(entryCount * 4));
    if (!Read(process, memoryBase + tableAddress, table.data(), table.size())) {
      CloseHandle(process);
      throw std::runtime_error("unable to read pointer table");
    }

    std::ofstream output(argv[6], std::ios::out | std::ios::trunc);
    if (!output) {
      CloseHandle(process);
      throw std::runtime_error("unable to open output CSV");
    }
    output << "id,address,vtable,descriptor_168,field_170,field_172,parent_248,flags_25c\n";

    std::map<std::uint32_t, VtableAggregate> aggregates;
    std::uint32_t nonNull = 0;
    std::uint32_t readable = 0;
    std::uint32_t validated = 0;
    std::vector<std::uint8_t> object(0x260);
    for (std::uint64_t index = 0; index < entryCount; ++index) {
      const auto address = BigEndian32(table.data() + index * 4);
      if (!address) continue;
      ++nonNull;
      if (!Read(process, memoryBase + address, object.data(), object.size())) {
        output << (idBase + index) << ",0x" << std::hex << std::setw(8)
               << std::setfill('0') << address << std::dec
               << ",UNREADABLE,,,,\n";
        continue;
      }
      ++readable;
      const auto vtable = BigEndian32(object.data());
      const auto descriptor168 = BigEndian32(object.data() + 0x168);
      const auto field170 = BigEndian16(object.data() + 0x170);
      const auto field172 = BigEndian16(object.data() + 0x172);
      const auto parent248 = BigEndian32(object.data() + 0x248);
      const auto flags25c = object[0x25C];
      const bool isValidated = field170 != 0 && field172 != 0;
      if (isValidated) ++validated;
      auto& aggregate = aggregates[vtable];
      if (!aggregate.count) {
        aggregate.firstId = static_cast<std::uint32_t>(idBase + index);
        aggregate.firstAddress = address;
      }
      ++aggregate.count;
      if (isValidated) ++aggregate.validated;

      output << (idBase + index) << ",0x" << std::hex << std::setw(8)
             << std::setfill('0') << address << ",0x" << std::setw(8)
             << vtable << ",0x" << std::setw(8) << descriptor168 << std::dec
             << ',' << field170 << ',' << field172
             << ",0x" << std::hex << std::setw(8) << parent248 << ",0x"
             << std::setw(2) << unsigned(flags25c) << std::dec << '\n';
    }
    CloseHandle(process);

    std::cout << "RUNTIME_GUEST_ENTITY_SCAN pid=" << pidValue
              << " table=0x" << std::hex << tableAddress << std::dec
              << " entries=" << entryCount << " id_base=" << idBase
              << " non_null=" << nonNull << " readable=" << readable
              << " validated=" << validated << " vtables="
              << aggregates.size() << '\n';
    for (const auto& [vtable, aggregate] : aggregates) {
      std::cout << "  vtable=0x" << std::hex << std::setw(8)
                << std::setfill('0') << vtable << std::dec
                << " count=" << aggregate.count
                << " validated=" << aggregate.validated
                << " first_id=" << aggregate.firstId << " first_address=0x"
                << std::hex << std::setw(8) << aggregate.firstAddress
                << std::dec << std::setfill(' ') << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "runtime_guest_entity_scan: " << error.what() << '\n';
    return 1;
  }
}
