#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <file.h>
#include <image.h>

struct SwitchTable { uint32_t source{}; std::vector<uint32_t> labels; };
struct Record { uint32_t target{}, source{}; std::string type, currentFunction, classification; };

static uint32_t ReadBE(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}
static int32_t SignExtend(uint32_t value, unsigned bits) {
    return static_cast<int32_t>(value << (32 - bits)) >> (32 - bits);
}
static std::optional<uint32_t> Hex(const std::string& text) {
    const auto pos = text.find("0x"); if (pos == std::string::npos) return {};
    size_t end = pos + 2; while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) ++end;
    return static_cast<uint32_t>(std::stoul(text.substr(pos + 2, end - pos - 2), nullptr, 16));
}
static std::map<uint32_t, SwitchTable> ReadSwitches(const char* path) {
    std::ifstream file(path); std::map<uint32_t, SwitchTable> out; SwitchTable current{}; bool active{}, labels{}; std::string line;
    while (std::getline(file, line)) {
        if (line == "[[switch]]") { if (active) out.emplace(current.source, current); current = {}; active = true; labels = false; }
        else if (active && line.find("base =") != std::string::npos) current.source = *Hex(line) + 0x14;
        else if (active && line.find("labels = [") != std::string::npos) labels = true;
        else if (labels) { if (line.find(']') != std::string::npos) labels = false; if (const auto label = Hex(line)) current.labels.push_back(*label); }
    }
    if (active) out.emplace(current.source, current); return out;
}
static std::set<uint32_t> ReadMap(const char* path) {
    std::ifstream file(path); std::regex entry(R"(\{ 0x([0-9A-F]+), sub_)"); std::smatch match; std::string line; std::set<uint32_t> entries;
    while (std::getline(file, line)) if (std::regex_search(line, match, entry)) entries.insert(std::stoul(match[1].str(), nullptr, 16));
    return entries;
}
static std::set<uint32_t> ReadProposalStarts(const char* path) {
    std::ifstream file(path); std::string line; std::set<uint32_t> starts; std::getline(file, line);
    while (std::getline(file, line)) {
        std::stringstream row(line); std::string field; std::getline(row, field, ','); std::getline(row, field, ',');
        if (const auto start = Hex(field)) starts.insert(*start);
    }
    return starts;
}
static bool InSection(const Section& section, uint32_t address) { return address >= section.base && address + 4 <= section.base + section.size; }
static std::string Address(uint32_t value) { std::ostringstream out; out << "0x" << std::hex << std::uppercase << value; return out.str(); }
static std::string Owner(uint32_t address, const std::vector<uint32_t>& starts) {
    const auto it = std::upper_bound(starts.begin(), starts.end(), address);
    return it == starts.begin() ? "" : Address(*std::prev(it));
}
static std::string Classify(uint32_t target, const std::set<uint32_t>& labels, const std::set<uint32_t>& mapEntries,
                            const std::set<uint32_t>& candidates, const std::set<uint32_t>& helpers) {
    if (helpers.count(target)) return "REGISTER_HELPER_ENTRY";
    if (labels.count(target)) return "SWITCH_TARGET";
    if (candidates.count(target)) return "SWITCH_OWNER_CANDIDATE";
    if (mapEntries.count(target)) return "FUNCTION_MAP_ENTRY";
    return "OTHER";
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Usage: xex_cross_reference <xex> <switch-tables.toml> <function-map.cpp> <range-proposals.csv> <output.csv>\n";
        return 2;
    }
    const auto switches = ReadSwitches(argv[2]); const auto mapEntries = ReadMap(argv[3]);
    const auto candidates = ReadProposalStarts(argv[4]);
    const std::set<uint32_t> helpers = { 0x82899A40, 0x828999F0, 0x8289A1DC, 0x8289A190, 0x829B8F08, 0x829B8C70, 0x829B8F9C, 0x829B8D04 };
    std::set<uint32_t> labels, tableWords;
    for (const auto& [source, table] : switches) {
        for (size_t i = 0; i < table.labels.size(); ++i) { labels.insert(table.labels[i]); tableWords.insert(source + 4 + static_cast<uint32_t>(i * 4)); }
    }
    std::set<uint32_t> relevant = labels; relevant.insert(candidates.begin(), candidates.end()); relevant.insert(helpers.begin(), helpers.end());
    std::vector<uint32_t> starts(mapEntries.begin(), mapEntries.end());
    const auto file = LoadFile(argv[1]); const auto image = Image::ParseImage(file.data(), file.size()); if (!image.data) return 1;
    std::vector<Record> records;
    auto add = [&](uint32_t target, uint32_t source, const char* type) {
        records.push_back({ target, source, type, Owner(source, starts), Classify(target, labels, mapEntries, candidates, helpers) });
    };
    for (const auto& section : image.sections) {
        if (!(section.flags & SectionFlags_Code)) continue;
        if (!section.data) continue;
        // Use an offset-bounded loop: some PE virtual sections end at the top
        // of the 32-bit address space, where an address-based end comparison
        // would wrap.  Non-code sections are scanned only for exact pointers.
        const auto imageOffset = static_cast<size_t>(section.data - image.data.get());
        if (imageOffset >= image.size) continue;
        const size_t readable = std::min<size_t>(section.size, image.size - imageOffset);
        for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
            const uint32_t pc = static_cast<uint32_t>(section.base + offset);
            const uint32_t word = ReadBE(section.data + offset);
            if (tableWords.count(pc)) { add(word, pc, "SWITCH_TABLE_ENTRY"); continue; }
            const uint32_t opcode = word >> 26;
            if (opcode == 18) {
                const uint32_t target = (word & 2) ? (word & 0x03FFFFFC) : pc + SignExtend(word & 0x03FFFFFC, 26);
                add(target, pc, (word & 1) ? "DIRECT_CALL_BL" : "DIRECT_BRANCH_B");
            } else if (opcode == 16) {
                const uint32_t target = (word & 2) ? (word & 0xFFFC) : pc + SignExtend(word & 0xFFFC, 16);
                add(target, pc, "CONDITIONAL_BRANCH_BC");
            } else if (opcode == 15 && InSection(section, pc + 4) && !tableWords.count(pc + 4)) {
                const uint32_t next = ReadBE(section.data + (pc + 4 - section.base));
                if ((next >> 26) == 14 && ((word >> 21) & 31) == ((next >> 21) & 31) && ((next >> 16) & 31) == ((word >> 21) & 31)) {
                    const uint32_t value = (uint32_t(word & 0xFFFF) << 16) + SignExtend(next & 0xFFFF, 16);
                    if (relevant.count(value)) add(value, pc, "ADDRESS_MATERIALIZATION_LIS_ADDI");
                }
            }
        }
    }
    // Imported function/vtable pointers are stored in the normal PE data areas.
    // Restrict this exact-word scan to the two standard, decoded sections rather
    // than treating relocation metadata as executable-reference evidence.
    for (const char* name : { ".rdata", ".data" }) {
        const Section* section = image.Find(name);
        if (!section || !section->data) continue;
        const auto imageOffset = static_cast<size_t>(section->data - image.data.get());
        if (imageOffset >= image.size) continue;
        const size_t readable = std::min<size_t>(section->size, image.size - imageOffset);
        for (size_t offset = 0; offset + 4 <= readable; offset += 4) {
            const uint32_t word = ReadBE(section->data + offset);
            if (relevant.count(word)) add(word, static_cast<uint32_t>(section->base + offset), "ABSOLUTE_ADDRESS_WORD");
        }
    }
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return std::tie(a.target, a.source, a.type) < std::tie(b.target, b.source, b.type);
    });
    std::ofstream out(argv[5], std::ios::trunc); if (!out.good()) return 1;
    out << "TARGET,REFERENCE_SOURCE,REFERENCE_TYPE,CURRENT_FUNCTION,TARGET_CLASSIFICATION\n";
    for (const auto& r : records) out << Address(r.target) << ',' << Address(r.source) << ',' << r.type << ',' << r.currentFunction << ',' << r.classification << '\n';
    size_t directCalls{}, other{}; for (const auto& r : records) if (r.type == "DIRECT_CALL_BL") ++directCalls; else ++other;
    std::cout << "records=" << records.size() << " direct_calls=" << directCalls << " other_references=" << other << "\n";
}
