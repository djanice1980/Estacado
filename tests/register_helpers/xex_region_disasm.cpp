#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

#include <disasm.h>
#include <file.h>
#include <image.h>

static size_t ParseAddress(const char* text)
{
    return std::stoull(text, nullptr, 0);
}

int main(int argc, char** argv)
{
    if (argc != 4 && argc != 5)
    {
        std::cerr << "Usage: xex_region_disasm <default.xex> <start-address> "
                     "<end-address-exclusive> [--words]\n";
        return 2;
    }

    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    const size_t start = ParseAddress(argv[2]);
    const size_t end = ParseAddress(argv[3]);
    if (!image.data || start >= end)
    {
        std::cerr << "Invalid XEX image or address range.\n";
        return 1;
    }

    const Section* section{};
    for (const auto& candidate : image.sections)
    {
        if (start >= candidate.base && end <= candidate.base + candidate.size)
        {
            section = &candidate;
            break;
        }
    }
    const bool dumpWords = argc == 5 && std::strcmp(argv[4], "--words") == 0;
    if (argc == 5 && !dumpWords)
    {
        std::cerr << "Unknown output mode. Expected --words.\n";
        return 2;
    }
    if (!section || (!dumpWords && !(section->flags & SectionFlags_Code)))
    {
        std::cerr << "Range is not fully contained in a suitable XEX section.\n";
        return 1;
    }

    std::cout << "section=" << section->name << " range=0x" << std::hex << std::uppercase
              << start << "..0x" << end << '\n';
    for (size_t address = start; address + 4 <= end; address += 4)
    {
        const auto* code = section->data + (address - section->base);
        ppc_insn insn{};
        ppc::Disassemble(code, address, insn);
        const uint32_t word = (static_cast<uint32_t>(code[0]) << 24)
            | (static_cast<uint32_t>(code[1]) << 16)
            | (static_cast<uint32_t>(code[2]) << 8)
            | static_cast<uint32_t>(code[3]);
        std::cout << "0x" << std::setw(8) << std::setfill('0') << address
                  << " 0x" << std::setw(8) << word << std::setfill(' ');
        if (!dumpWords)
        {
            std::cout << " " << (insn.opcode ? insn.opcode->name : "unknown")
                      << (insn.op_str[0] ? " " : "") << insn.op_str;
        }
        std::cout << '\n';
    }
}
