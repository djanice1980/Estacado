#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>

#include <file.h>
#include <image.h>

namespace {
uint32_t ParseAddress(const char* text) {
    return static_cast<uint32_t>(std::stoull(text, nullptr, 0));
}

uint32_t ReadBigEndianWord(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

int32_t SignExtend16(uint32_t value) {
    return static_cast<int16_t>(value & 0xFFFFu);
}

int32_t SignExtend(uint32_t value, unsigned bits) {
    return static_cast<int32_t>(value << (32 - bits)) >> (32 - bits);
}

void PrintReference(uint32_t target, uint32_t address, const char* section,
                    const char* kind) {
    std::cout << "target=0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << target << " reference=0x" << std::setw(8)
              << address << std::dec << std::setfill(' ') << " section="
              << section << " kind=" << kind << '\n';
}
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: xex_address_references <default.xex> <address> "
                     "[address ...]\n";
        return 2;
    }
    std::set<uint32_t> targets;
    for (int index = 2; index < argc; ++index) targets.insert(ParseAddress(argv[index]));

    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data) {
        std::cerr << "Unable to parse XEX image.\n";
        return 1;
    }

    size_t matches = 0;
    for (const auto& section : image.sections) {
        if (!section.data) continue;
        const auto imageOffset = static_cast<size_t>(section.data - image.data.get());
        if (imageOffset >= image.size) continue;
        const size_t readable = std::min<size_t>(section.size, image.size - imageOffset);

        if (section.name == ".rdata" || section.name == ".data") {
            for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
                const uint32_t word = ReadBigEndianWord(section.data + offset);
                if (!targets.count(word)) continue;
                PrintReference(word, static_cast<uint32_t>(section.base + offset),
                               section.name.c_str(), "absolute-word");
                ++matches;
            }
        }
        if (!(section.flags & SectionFlags_Code)) continue;
        for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
            const uint32_t address = static_cast<uint32_t>(section.base + offset);
            const uint32_t branch = ReadBigEndianWord(section.data + offset);
            const uint32_t branchOpcode = branch >> 26;
            if (branchOpcode == 18) {
                const uint32_t target = (branch & 2u)
                    ? (branch & 0x03FFFFFCu)
                    : address + SignExtend(branch & 0x03FFFFFCu, 26);
                if (targets.count(target)) {
                    PrintReference(target, address, section.name.c_str(),
                                   (branch & 1u) ? "direct-call" : "direct-branch");
                    ++matches;
                }
            } else if (branchOpcode == 16) {
                const uint32_t target = (branch & 2u)
                    ? (branch & 0x0000FFFCu)
                    : address + SignExtend(branch & 0x0000FFFCu, 16);
                if (targets.count(target)) {
                    PrintReference(target, address, section.name.c_str(),
                                   (branch & 1u) ? "conditional-call"
                                                 : "conditional-branch");
                    ++matches;
                }
            }

            const uint32_t lis = ReadBigEndianWord(section.data + offset);
            if ((lis >> 26) != 15 || ((lis >> 16) & 31u) != 0) continue;
            const uint32_t reg = (lis >> 21) & 31u;
            const uint32_t upper = (lis & 0xFFFFu) << 16;
            const size_t end = std::min(readable - 4, offset + size_t(8 * 4));
            for (size_t next = offset + 4; next <= end; next += 4) {
                const uint32_t word = ReadBigEndianWord(section.data + next);
                const uint32_t opcode = word >> 26;
                uint32_t value{};
                const char* kind{};
                if (opcode == 14 && ((word >> 16) & 31u) == reg) {
                    value = upper + SignExtend16(word);
                    kind = "lis-addi";
                } else if (opcode == 24 && ((word >> 21) & 31u) == reg) {
                    value = upper | (word & 0xFFFFu);
                    kind = "lis-ori";
                } else {
                    continue;
                }
                if (!targets.count(value)) continue;
                PrintReference(value, static_cast<uint32_t>(section.base + next),
                               section.name.c_str(), kind);
                ++matches;
            }
        }
    }
    std::cout << "matches=" << matches << '\n';
    return matches ? 0 : 3;
}
