#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#include <file.h>
#include <image.h>

namespace {
std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool PrintableAscii(uint8_t value) {
    return value >= 0x20 && value <= 0x7E;
}
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3 && argc != 5 && argc != 6) {
        std::cerr << "Usage: xex_string_search <default.xex> [substring]\n"
                     "       xex_string_search <default.xex> --range <start> "
                     "<end-exclusive> [substring]\n";
        return 2;
    }
    uint32_t rangeStart = 0;
    uint64_t rangeEnd = uint64_t{UINT32_MAX} + 1;
    std::string needle;
    if (argc >= 5) {
        if (std::string(argv[2]) != "--range") {
            std::cerr << "Expected --range before address bounds.\n";
            return 2;
        }
        rangeStart = static_cast<uint32_t>(std::stoull(argv[3], nullptr, 0));
        rangeEnd = std::stoull(argv[4], nullptr, 0);
        if (rangeEnd <= rangeStart || rangeEnd > uint64_t{UINT32_MAX} + 1) {
            std::cerr << "Invalid string search range.\n";
            return 2;
        }
        if (argc == 6) needle = LowerAscii(argv[5]);
    } else if (argc == 3) {
        needle = LowerAscii(argv[2]);
    }
    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data) {
        std::cerr << "Unable to parse XEX image.\n";
        return 1;
    }

    size_t matches = 0;
    for (const auto& section : image.sections) {
        // Title strings and reflection names live in the decoded PE data
        // sections. Avoid virtual code/BSS tails whose declared image extent
        // may exceed the committed backing returned by the XEX parser.
        if ((section.name != ".rdata" && section.name != ".data") ||
            !section.data) {
            continue;
        }
        const auto imageOffset = static_cast<size_t>(section.data - image.data.get());
        if (imageOffset >= image.size) continue;
        const size_t readable =
            std::min<size_t>(section.size, image.size - imageOffset);
        for (size_t offset = 0; offset < readable;) {
            if (!PrintableAscii(section.data[offset])) {
                ++offset;
                continue;
            }
            const size_t start = offset;
            while (offset < readable && PrintableAscii(section.data[offset])) ++offset;
            if (offset - start < 4) continue;
            const uint64_t address = uint64_t(section.base) + start;
            if (address < rangeStart || address >= rangeEnd) continue;
            const std::string value(
                reinterpret_cast<const char*>(section.data + start),
                offset - start);
            if (!needle.empty() && LowerAscii(value).find(needle) == std::string::npos)
                continue;
            std::cout << "address=0x" << std::hex << std::uppercase
                      << std::setw(8) << std::setfill('0')
                      << static_cast<uint32_t>(address)
                      << std::dec << std::setfill(' ') << " section="
                      << section.name << " text=" << value << '\n';
            ++matches;
        }
    }
    std::cout << "matches=" << matches << '\n';
    return matches || needle.empty() ? 0 : 3;
}
