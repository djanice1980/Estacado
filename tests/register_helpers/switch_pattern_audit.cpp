#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <vector>

#include <file.h>
#include <image.h>

static uint32_t ReadBigEndian(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24)
        | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8)
        | static_cast<uint32_t>(data[3]);
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: switch_pattern_audit <default.xex> <recompiler-log>\n";
        return 2;
    }

    std::ifstream log(argv[2]);
    const std::regex warning{ R"(^ERROR: Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)$)" };
    std::map<uint32_t, std::vector<uint32_t>> cases;
    std::string line;
    while (std::getline(log, line))
    {
        std::smatch match;
        if (std::regex_match(line, match, warning))
            cases[std::stoul(match[1].str(), nullptr, 16)].push_back(std::stoul(match[2].str(), nullptr, 16));
    }

    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        std::cerr << "Unable to load XEX image.\n";
        return 1;
    }

    size_t bctrCount{};
    size_t tableMatchCount{};
    size_t targetsAfterTableCount{};
    for (const auto& [source, targets] : cases)
    {
        const auto* instruction = static_cast<const uint8_t*>(image.Find(source));
        const bool isBctr = ReadBigEndian(instruction) == 0x4E800420;
        if (isBctr)
            ++bctrCount;

        bool match = true;
        const auto* table = static_cast<const uint8_t*>(image.Find(source + 4));
        for (size_t i = 0; i < targets.size(); ++i)
        {
            const uint32_t tableWord = ReadBigEndian(table + i * 4);
            if (tableWord != targets[i])
            {
                if (match)
                {
                    std::cout << "first_inline_table_mismatch=0x" << std::hex << std::uppercase << source
                              << " index=" << std::dec << i
                              << " table_address=0x" << std::hex << (source + 4 + static_cast<uint32_t>(i * 4))
                              << " table_word=0x" << tableWord
                              << " logged_target=0x" << targets[i] << '\n';
                }
                match = false;
            }
        }
        if (match)
            ++tableMatchCount;

        const uint32_t tableEnd = source + 4 + static_cast<uint32_t>(targets.size() * 4);
        if (std::all_of(targets.begin(), targets.end(), [tableEnd](uint32_t target) { return target >= tableEnd; }))
            ++targetsAfterTableCount;
        else
            std::cout << "target_before_inline_table=0x" << std::hex << std::uppercase << source << '\n';
        if (!isBctr)
            std::cout << "non_bctr_source=0x" << std::hex << std::uppercase << source << '\n';
        if (!match)
            std::cout << "inline_table_mismatch=0x" << std::hex << std::uppercase << source << '\n';
    }

    std::cout << std::dec;
    std::cout << "warning_sources=" << cases.size() << '\n';
    std::cout << "bctr_at_warning_source=" << bctrCount << '\n';
    std::cout << "inline_table_words_match_warning_targets=" << tableMatchCount << '\n';
    std::cout << "all_targets_after_inline_table=" << targetsAfterTableCount << '\n';
    return (bctrCount == cases.size() && tableMatchCount == cases.size() && targetsAfterTableCount == cases.size()) ? 0 : 1;
}
