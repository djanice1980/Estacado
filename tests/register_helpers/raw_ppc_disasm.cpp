#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <disasm.h>

namespace
{
uint64_t ParseAddress(const char* text)
{
    return std::stoull(text, nullptr, 0);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::cerr << "Usage: raw_ppc_disasm <raw-image> <base-address> "
                     "<start-address> <end-address-exclusive>\n";
        return 2;
    }

    const std::filesystem::path path(argv[1]);
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        std::cerr << "Unable to open raw image: " << path << '\n';
        return 1;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)),
                                     std::istreambuf_iterator<char>());
    const uint64_t base = ParseAddress(argv[2]);
    const uint64_t start = ParseAddress(argv[3]);
    const uint64_t end = ParseAddress(argv[4]);
    if (start < base || start >= end || end - base > image.size() ||
        ((start | end | base) & 3u) != 0)
    {
        std::cerr << "Invalid raw image or address range.\n";
        return 1;
    }

    std::cout << "image=" << path.string() << " range=0x" << std::hex
              << std::uppercase << start << "..0x" << end << '\n';
    for (uint64_t address = start; address + 4 <= end; address += 4)
    {
        const auto* code = image.data() + (address - base);
        ppc_insn insn{};
        ppc::Disassemble(code, address, insn);
        const uint32_t word = (static_cast<uint32_t>(code[0]) << 24)
            | (static_cast<uint32_t>(code[1]) << 16)
            | (static_cast<uint32_t>(code[2]) << 8)
            | static_cast<uint32_t>(code[3]);
        std::cout << "0x" << std::setw(8) << std::setfill('0') << address
                  << " 0x" << std::setw(8) << word << std::setfill(' ')
                  << " " << (insn.opcode ? insn.opcode->name : "unknown")
                  << (insn.op_str[0] ? " " : "") << insn.op_str << '\n';
    }
    return 0;
}
