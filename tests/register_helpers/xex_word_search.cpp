#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include <file.h>
#include <image.h>

namespace
{
uint32_t ParseWord(const char* text)
{
    return static_cast<uint32_t>(std::stoull(text, nullptr, 0));
}

uint32_t ReadBigEndianWord(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24)
        | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8)
        | static_cast<uint32_t>(data[3]);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: xex_word_search <default.xex> <rdata-word> [rdata-word ...]\n"
                     "       xex_word_search <default.xex> --dump <start> <end>\n"
                     "       xex_word_search <default.xex> --string <ASCII-text>\n"
                     "       xex_word_search <default.xex> --xref <address> [instruction-window]\n";
        return 2;
    }

    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        std::cerr << "Unable to parse XEX image.\n";
        return 1;
    }

    if (argc == 5 && std::string(argv[2]) == "--dump")
    {
        const uint32_t start = ParseWord(argv[3]);
        const uint32_t end = ParseWord(argv[4]);
        if (end <= start || (start & 3) != 0 || (end & 3) != 0)
        {
            std::cerr << "Dump range must be a non-empty, word-aligned half-open interval.\n";
            return 2;
        }

        for (const auto& section : image.sections)
        {
            const uint64_t sectionEnd = static_cast<uint64_t>(section.base) + section.size;
            if (!section.data || start < section.base || end > sectionEnd)
            {
                continue;
            }

            for (uint32_t address = start; address < end; address += 4)
            {
                const uint32_t word = ReadBigEndianWord(section.data + (address - section.base));
                std::cout << "address=0x" << std::hex << std::uppercase
                          << std::setw(8) << std::setfill('0') << address
                          << " word=0x" << std::setw(8) << word
                          << std::setfill(' ') << " section=" << section.name << '\n';
            }
            return 0;
        }

        std::cerr << "Dump range is not fully contained in a mapped XEX section.\n";
        return 3;
    }

    if (argc == 4 && std::string(argv[2]) == "--string")
    {
        const std::string needle = argv[3];
        if (needle.empty())
        {
            std::cerr << "String search text must not be empty.\n";
            return 2;
        }

        size_t matchCount = 0;
        for (const auto& section : image.sections)
        {
            // XEX images may describe logical zero-filled sections whose
            // declared virtual size is not backed by bytes in the parsed
            // file image. Title-owned property strings live in .rdata, which
            // is the same file-backed section this tool already scans safely.
            if (section.name != ".rdata" || !section.data ||
                section.size < needle.size())
            {
                continue;
            }
            for (uint32_t offset = 0;
                 offset + needle.size() <= section.size; ++offset)
            {
                if (std::memcmp(section.data + offset, needle.data(),
                                needle.size()) != 0)
                {
                    continue;
                }
                std::cout << "string=" << needle << " address=0x" << std::hex
                          << std::uppercase << std::setw(8) << std::setfill('0')
                          << (section.base + offset) << std::setfill(' ')
                          << " section=" << section.name << " value=";
                const size_t valueLimit =
                    std::min<size_t>(section.size - offset, 256);
                for (size_t index = 0; index < valueLimit; ++index)
                {
                    const uint8_t character = section.data[offset + index];
                    if (!character)
                    {
                        break;
                    }
                    if (character < 0x20 || character > 0x7E)
                    {
                        std::cout << "\\x" << std::setw(2)
                                  << std::setfill('0')
                                  << static_cast<unsigned>(character)
                                  << std::setfill(' ');
                    }
                    else
                    {
                        std::cout << static_cast<char>(character);
                    }
                }
                std::cout << '\n';
                ++matchCount;
            }
        }
        if (!matchCount)
        {
            std::cout << "string=" << needle << " matches=0\n";
        }
        return matchCount ? 0 : 3;
    }

    if ((argc == 4 || argc == 5) && std::string(argv[2]) == "--xref")
    {
        const uint32_t target = ParseWord(argv[3]);
        const uint32_t window = argc == 5 ? ParseWord(argv[4]) : 8;
        if (!window || window > 32)
        {
            std::cerr << "Instruction window must be between 1 and 32.\n";
            return 2;
        }

        size_t matchCount = 0;
        for (const auto& section : image.sections)
        {
            if (!section.data || !(section.flags & SectionFlags_Code))
            {
                continue;
            }
            for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4)
            {
                const uint32_t lis = ReadBigEndianWord(section.data + offset);
                if ((lis >> 26) != 15 || ((lis >> 16) & 0x1F) != 0)
                {
                    continue;
                }
                const uint32_t baseRegister = (lis >> 21) & 0x1F;
                const uint32_t upper = (lis & 0xFFFF) << 16;
                const uint32_t available = (section.size - offset - 4) / 4;
                const uint32_t count = available < window ? available : window;
                for (uint32_t distance = 1; distance <= count; ++distance)
                {
                    const uint32_t referenceOffset = offset + distance * 4;
                    const uint32_t word =
                        ReadBigEndianWord(section.data + referenceOffset);
                    if (((word >> 16) & 0x1F) != baseRegister)
                    {
                        continue;
                    }
                    const uint32_t opcode = word >> 26;
                    uint32_t address{};
                    const char* kind{};
                    if (opcode == 14)
                    {
                        address = upper + static_cast<uint32_t>(
                            static_cast<int32_t>(static_cast<int16_t>(word)));
                        kind = "lis+addi";
                    }
                    else if (opcode == 24)
                    {
                        address = upper | (word & 0xFFFF);
                        kind = "lis+ori";
                    }
                    else
                    {
                        continue;
                    }
                    if (address != target)
                    {
                        continue;
                    }
                    std::cout << "target=0x" << std::hex << std::uppercase
                              << std::setw(8) << std::setfill('0') << target
                              << " lis=0x" << std::setw(8)
                              << (section.base + offset)
                              << " reference=0x" << std::setw(8)
                              << (section.base + referenceOffset)
                              << std::setfill(' ') << " kind=" << kind
                              << " section=" << section.name << '\n';
                    ++matchCount;
                }
            }
        }
        if (!matchCount)
        {
            std::cout << "target=0x" << std::hex << std::uppercase
                      << std::setw(8) << std::setfill('0') << target
                      << " matches=0\n";
        }
        return matchCount ? 0 : 3;
    }

    bool foundAny = false;
    for (int argument = 2; argument < argc; ++argument)
    {
        const uint32_t target = ParseWord(argv[argument]);
        size_t matchCount = 0;
        for (const auto& section : image.sections)
        {
            if (section.name != ".rdata" || !section.data)
            {
                continue;
            }

            for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4)
            {
                if (ReadBigEndianWord(section.data + offset) != target)
                {
                    continue;
                }

                std::cout << "target=0x" << std::hex << std::uppercase
                          << std::setw(8) << std::setfill('0') << target
                          << " address=0x" << std::setw(8)
                          << (section.base + offset) << std::setfill(' ')
                          << " section=" << section.name << '\n';
                ++matchCount;
                foundAny = true;
            }
        }

        if (matchCount == 0)
        {
            std::cout << "target=0x" << std::hex << std::uppercase
                      << std::setw(8) << std::setfill('0') << target
                      << " matches=0\n";
        }
    }

    return foundAny ? 0 : 3;
}
