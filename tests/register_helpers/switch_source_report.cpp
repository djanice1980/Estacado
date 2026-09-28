#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <file.h>
#include <image.h>
#include <xbox.h>

struct SwitchEntry {
    uint32_t base{};
    std::vector<uint32_t> labels;
};

static uint32_t ReadBigEndian(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16)
         | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

static std::optional<uint32_t> ParseHexAfterEquals(const std::string& line) {
    const auto marker = line.find("0x");
    if (marker == std::string::npos) return std::nullopt;
    size_t end = marker + 2;
    while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end]))) ++end;
    return static_cast<uint32_t>(std::stoul(line.substr(marker + 2, end - marker - 2), nullptr, 16));
}

static std::map<uint32_t, SwitchEntry> ParseSwitches(const char* path) {
    std::ifstream input(path);
    std::map<uint32_t, SwitchEntry> entries;
    SwitchEntry current{};
    bool haveCurrent{};
    bool inLabels{};
    std::string line;
    while (std::getline(input, line)) {
        if (line == "[[switch]]") {
            if (haveCurrent) entries.emplace(current.base + 0x14, current);
            current = {};
            haveCurrent = true;
            inLabels = false;
        } else if (haveCurrent && line.find("base =") != std::string::npos) {
            current.base = *ParseHexAfterEquals(line);
        } else if (haveCurrent && line.find("labels = [") != std::string::npos) {
            inLabels = true;
        } else if (inLabels) {
            if (line.find(']') != std::string::npos) inLabels = false;
            const auto value = ParseHexAfterEquals(line);
            if (value) current.labels.push_back(*value);
        }
    }
    if (haveCurrent) entries.emplace(current.base + 0x14, current);
    return entries;
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: switch_source_report <default.xex> <recompiler-log> <switch-tables.toml> [output.csv]\n";
        return 2;
    }
    const std::regex warning{R"(^ERROR: Switch case at ([0-9A-F]+) is trying to jump outside function: ([0-9A-F]+)$)"};
    std::ifstream log(argv[2]);
    std::map<uint32_t, std::vector<uint32_t>> warnings;
    std::string line;
    while (std::getline(log, line)) {
        std::smatch match;
        if (std::regex_match(line, match, warning))
            warnings[std::stoul(match[1].str(), nullptr, 16)].push_back(std::stoul(match[2].str(), nullptr, 16));
    }
    const auto switches = ParseSwitches(argv[3]);
    const auto file = LoadFile(argv[1]);
    const auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data) return 1;
    std::ofstream output;
    std::ostream* report = &std::cout;
    if (argc == 5) {
        output.open(argv[4], std::ios::trunc);
        if (!output.good()) return 1;
        report = &output;
    }
    std::vector<std::pair<uint32_t, uint32_t>> pdata;
    if (const auto* section = image.Find(".pdata")) {
        const auto* records = reinterpret_cast<const IMAGE_CE_RUNTIME_FUNCTION*>(section->data);
        for (size_t i = 0; i < section->size / sizeof(*records); ++i) {
            const uint32_t start = _byteswap_ulong(records[i].BeginAddress);
            const uint32_t data = _byteswap_ulong(records[i].Data);
            const uint32_t length = (data >> 8) & 0x3FFFFF;
            if (length) pdata.emplace_back(start, start + length * 4);
        }
    }
    *report << "source,bctr,config_base,table_start,table_end,label_count,warning_count,raw_table_matches_config,pdata_start,pdata_end,classification\n";
    for (const auto& [source, targets] : warnings) {
        const auto sw = switches.find(source);
        bool configMatch{};
        uint32_t tableEnd = source + 4;
        uint32_t base{};
        size_t labelCount{};
        if (sw != switches.end()) {
            base = sw->second.base;
            labelCount = sw->second.labels.size();
            const auto* table = static_cast<const uint8_t*>(image.Find(source + 4));
            configMatch = ReadBigEndian(static_cast<const uint8_t*>(image.Find(source))) == 0x4E800420;
            for (size_t i = 0; configMatch && i < labelCount; ++i)
                configMatch = ReadBigEndian(table + i * 4) == sw->second.labels[i];
            tableEnd += static_cast<uint32_t>(labelCount * 4);
        }
        uint32_t pdataStart{};
        uint32_t pdataEnd{};
        for (const auto& [start, end] : pdata) if (source >= start && source < end) { pdataStart = start; pdataEnd = end; break; }
        const char* classification = configMatch ? "VERIFIED_ABSOLUTE_INLINE_TABLE" : "UNKNOWN";
        *report << "0x" << std::hex << std::uppercase << source << ",0x" << source << ",0x" << base
                << ",0x" << (source + 4) << ",0x" << tableEnd << std::dec << ',' << labelCount << ',' << targets.size()
                << ',' << (configMatch ? "YES" : "NO") << ",0x" << std::hex << pdataStart << ",0x" << pdataEnd
                << ',' << classification << '\n';
    }
}
