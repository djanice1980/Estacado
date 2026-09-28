#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include <file.h>
#include <image.h>

static uint32_t ReadBE32(const uint8_t* bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
        (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

static int32_t SignExtend16(uint32_t value)
{
    return static_cast<int16_t>(value);
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 4) return 2;
    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data) return 1;
    std::cout << "0x" << std::hex << std::uppercase << image.entry_point << '\n';
    if (argc == 4 && std::strcmp(argv[2], "--find-ascii") == 0) {
        std::string expected = argv[3];
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (expected.empty()) return 2;

        size_t matches{};
        for (const char* sectionName : { ".rdata", ".data" }) {
            const Section* sectionPointer = image.Find(sectionName);
            if (!sectionPointer || !sectionPointer->data) continue;
            const Section& section = *sectionPointer;
            const auto imageOffset = static_cast<size_t>(section.data - image.data.get());
            if (imageOffset >= image.size) continue;
            const size_t readable = std::min<size_t>(section.size, image.size - imageOffset);
            size_t previousStringEnd{};
            bool havePreviousString{};
            for (size_t offset = 0; offset + expected.size() <= readable; ++offset) {
                bool equal = true;
                for (size_t index = 0; index < expected.size(); ++index) {
                    if (std::tolower(static_cast<unsigned char>(section.data[offset + index])) !=
                        static_cast<unsigned char>(expected[index])) {
                        equal = false;
                        break;
                    }
                }
                if (!equal) continue;

                size_t begin = offset;
                while (begin && std::isprint(static_cast<unsigned char>(section.data[begin - 1]))) {
                    --begin;
                }
                size_t end = offset + expected.size();
                while (end < readable &&
                       std::isprint(static_cast<unsigned char>(section.data[end]))) {
                    ++end;
                }
                if (havePreviousString && begin < previousStringEnd) continue;
                previousStringEnd = end;
                havePreviousString = true;

                std::cout << "match=0x" << std::setw(8) << std::setfill('0')
                          << section.base + begin << " section=" << section.name << " text=\""
                          << std::string(reinterpret_cast<const char*>(section.data + begin),
                                         end - begin)
                          << "\"\n";
                ++matches;
            }
        }
        std::cout << std::dec << "matches=" << matches << '\n';
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[2], "--find-address") == 0) {
        const uint32_t expected = static_cast<uint32_t>(std::stoull(argv[3], nullptr, 0));
        size_t matches{};
        for (const auto& section : image.sections) {
            if (!(section.flags & SectionFlags_Code) || !section.data) continue;
            const auto imageOffset = static_cast<size_t>(section.data - image.data.get());
            if (imageOffset >= image.size) continue;
            const size_t readable = std::min<size_t>(section.size, image.size - imageOffset);
            for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
                const uint32_t first = ReadBE32(section.data + offset);
                if ((first >> 26) != 15 || ((first >> 16) & 31) != 0) continue;
                const uint32_t destination = (first >> 21) & 31;
                const uint32_t high = (first & 0xFFFFu) << 16;
                const size_t searchEnd = std::min(readable, offset + 36);
                for (size_t nextOffset = offset + 4; nextOffset + 4 <= searchEnd;
                     nextOffset += 4) {
                    const uint32_t next = ReadBE32(section.data + nextOffset);
                    const uint32_t opcode = next >> 26;
                    const uint32_t firstRegister = (next >> 21) & 31;
                    const uint32_t secondRegister = (next >> 16) & 31;
                    uint32_t candidate{};
                    const char* kind{};
                    if (opcode == 14 && secondRegister == destination) {
                        candidate = high + SignExtend16(next & 0xFFFFu);
                        kind = "LIS_ADDI";
                    } else if (opcode == 24 && firstRegister == destination) {
                        candidate = high | (next & 0xFFFFu);
                        kind = "LIS_ORI";
                    } else if (opcode >= 32 && opcode <= 47 &&
                               secondRegister == destination) {
                        candidate = high + SignExtend16(next & 0xFFFFu);
                        kind = "LIS_DFORM";
                    }
                    if (kind && candidate == expected) {
                        std::cout << "match=0x" << std::setw(8) << std::setfill('0')
                                  << section.base + offset << " use=0x" << std::setw(8)
                                  << section.base + nextOffset << " section=" << section.name
                                  << " type=" << kind << '\n';
                        ++matches;
                    }
                    if (opcode == 16 || opcode == 18 || opcode == 19) break;
                }
            }
        }
        for (const char* sectionName : { ".rdata", ".data" }) {
            const Section* section = image.Find(sectionName);
            if (!section || !section->data) continue;
            const auto imageOffset = static_cast<size_t>(section->data - image.data.get());
            if (imageOffset >= image.size) continue;
            const size_t readable = std::min<size_t>(section->size, image.size - imageOffset);
            for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
                if (ReadBE32(section->data + offset) != expected) continue;
                std::cout << "match=0x" << std::setw(8) << std::setfill('0')
                          << section->base + offset << " section=" << section->name
                          << " type=ABSOLUTE_WORD\n";
                ++matches;
            }
        }
        std::cout << std::dec << "matches=" << matches << '\n';
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[2], "--find-word") == 0) {
        const uint32_t expected = static_cast<uint32_t>(std::stoull(argv[3], nullptr, 0));
        size_t matches{};
        for (const char* sectionName : { ".rdata", ".data" }) {
            const Section* section = image.Find(sectionName);
            if (!section || !section->data) continue;
            const auto imageOffset = static_cast<size_t>(section->data - image.data.get());
            if (imageOffset >= image.size) continue;
            const size_t readable = std::min<size_t>(section->size, image.size - imageOffset);
            for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
                const auto* bytes = section->data + offset;
                const uint32_t word = (uint32_t(bytes[0]) << 24) |
                    (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
                if (word != expected) continue;
                std::cout << "match=0x" << std::setw(8) << std::setfill('0')
                          << section->base + offset << " section=" << section->name << '\n';
                ++matches;
            }
        }
        std::cout << std::dec << "matches=" << matches << '\n';
        return 0;
    }
    if (argc == 4) {
        const size_t address = std::stoull(argv[2], nullptr, 0);
        const size_t length = std::stoull(argv[3], nullptr, 0);
        const auto* bytes = static_cast<const uint8_t*>(image.Find(address));
        if (!bytes || address + length < address || address + length > image.base + image.size)
            return 3;
        for (size_t offset = 0; offset < length; offset += 16) {
            std::cout << "0x" << std::setw(8) << std::setfill('0') << address + offset << "  ";
            for (size_t index = 0; index < 16; ++index) {
                if (offset + index < length)
                    std::cout << std::setw(2) << unsigned(bytes[offset + index]) << ' ';
                else
                    std::cout << "   ";
            }
            std::cout << '\n';
        }
    }
}
