#include <array>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

#include <disasm.h>
#include <file.h>
#include <image.h>

struct Pattern
{
    std::string_view name;
    std::array<uint8_t, 8> bytes;
    size_t length;
};

static constexpr Pattern Patterns[] = {
    { "restgprlr_14", { 0xE9, 0xC1, 0xFF, 0x68 }, 4 },
    { "savegprlr_14", { 0xF9, 0xC1, 0xFF, 0x68 }, 4 },
    { "restfpr_14",   { 0xC9, 0xCC, 0xFF, 0x70 }, 4 },
    { "savefpr_14",   { 0xD9, 0xCC, 0xFF, 0x70 }, 4 },
    { "restvmx_14",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE }, 8 },
    { "savevmx_14",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE }, 8 },
    { "restvmx_64",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB }, 8 },
    { "savevmx_64",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB }, 8 },
};

static void PrintInstruction(const uint8_t* code, size_t address)
{
    ppc_insn insn{};
    ppc::Disassemble(code, address, insn);
    const uint32_t word = (static_cast<uint32_t>(code[0]) << 24)
        | (static_cast<uint32_t>(code[1]) << 16)
        | (static_cast<uint32_t>(code[2]) << 8)
        | static_cast<uint32_t>(code[3]);
    std::cout << "  0x" << std::hex << std::uppercase << address << "  0x"
              << std::setw(8) << std::setfill('0') << word << std::setfill(' ')
              << "  " << (insn.opcode ? insn.opcode->name : "unknown")
              << (insn.op_str[0] ? " " : "") << insn.op_str << '\n';
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: register_helper_scan <default.xex>\n";
        return 2;
    }

    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data || image.sections.empty())
    {
        std::cerr << "Unable to load XEX image.\n";
        return 1;
    }

    std::cout << "image_base=0x" << std::hex << std::uppercase << image.base
              << " image_size=0x" << image.size << '\n';

    for (const auto& pattern : Patterns)
    {
        size_t hits{};
        std::cout << "\nPATTERN " << pattern.name << '\n';
        for (const auto& section : image.sections)
        {
            if (!(section.flags & SectionFlags_Code))
                continue;

            for (size_t offset = 0; offset + pattern.length <= section.size; offset += 4)
            {
                if (std::memcmp(section.data + offset, pattern.bytes.data(), pattern.length) != 0)
                    continue;

                const size_t address = section.base + offset;
                ++hits;
                std::cout << "MATCH section=" << section.name << " address=0x" << std::hex << std::uppercase << address << '\n';
                constexpr size_t Before = 16;
                constexpr size_t After = 0x240;
                const size_t begin = offset >= Before ? offset - Before : 0;
                const size_t end = std::min<size_t>(section.size, offset + After);
                for (size_t cursor = begin; cursor + 4 <= end; cursor += 4)
                    PrintInstruction(section.data + cursor, section.base + cursor);
            }
        }
        std::cout << "COUNT " << std::dec << hits << '\n';
    }
}
