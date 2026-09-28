#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <file.h>
#include <image.h>

struct SwitchTable { uint32_t base{}; std::vector<uint32_t> labels; };
struct Result {
    uint32_t source{}, start{}, end{}, tableEnd{};
    size_t returns{}, externalBranches{}, inboundCalls{};
    bool reachedSource{}, allLabelsReachable{}, unknownIndirect{};
};

static uint32_t ReadBE(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
}
static int32_t SignExtend(uint32_t value, unsigned bits) {
    return static_cast<int32_t>(value << (32 - bits)) >> (32 - bits);
}
static std::optional<uint32_t> Hex(const std::string& line) {
    const auto pos = line.find("0x"); if (pos == std::string::npos) return {};
    size_t end = pos + 2; while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end]))) ++end;
    return static_cast<uint32_t>(std::stoul(line.substr(pos + 2, end - pos - 2), nullptr, 16));
}
static std::map<uint32_t, SwitchTable> ReadSwitches(const char* path) {
    std::ifstream file(path); std::map<uint32_t, SwitchTable> out; SwitchTable current{}; bool active{}, labels{}; std::string line;
    while (std::getline(file, line)) {
        if (line == "[[switch]]") { if (active) out.emplace(current.base + 0x14, current); current = {}; active = true; labels = false; }
        else if (active && line.find("base =") != std::string::npos) current.base = *Hex(line);
        else if (active && line.find("labels = [") != std::string::npos) labels = true;
        else if (labels) { if (line.find(']') != std::string::npos) labels = false; if (const auto value = Hex(line)) current.labels.push_back(*value); }
    }
    if (active) out.emplace(current.base + 0x14, current); return out;
}
static std::vector<uint32_t> ReadMap(const char* path) {
    std::ifstream file(path); std::regex entry(R"(\{ 0x([0-9A-F]+), sub_)"); std::string line; std::smatch match; std::vector<uint32_t> starts;
    while (std::getline(file, line)) if (std::regex_search(line, match, entry)) starts.push_back(std::stoul(match[1].str(), nullptr, 16));
    std::sort(starts.begin(), starts.end()); return starts;
}
static std::set<uint32_t> ReadSources(const char* csv) {
    std::ifstream file(csv); std::string line; std::set<uint32_t> sources; std::getline(file, line);
    while (std::getline(file, line)) if (const auto value = Hex(line)) sources.insert(*value); return sources;
}
static bool InSection(const Section& section, uint32_t address) { return address >= section.base && address + 4 <= section.base + section.size; }

int main(int argc, char** argv) {
    if (argc != 6) { std::cerr << "Usage: switch_range_proposal <xex> <switch-tables.toml> <function-map.cpp> <sources.csv> <output.csv>\n"; return 2; }
    const auto switches = ReadSwitches(argv[2]); const auto starts = ReadMap(argv[3]); const auto sources = ReadSources(argv[4]);
    const auto file = LoadFile(argv[1]); const auto image = Image::ParseImage(file.data(), file.size()); if (!image.data) return 1;
    std::set<uint32_t> tableBytes;
    for (const auto& [source, sw] : switches) for (uint32_t a = source + 4; a < source + 4 + sw.labels.size() * 4; a += 4) tableBytes.insert(a);
    std::set<uint32_t> directCallTargets;
    for (const auto& section : image.sections) {
        if (!(section.flags & SectionFlags_Code)) continue;
        for (uint32_t pc = section.base; InSection(section, pc); pc += 4) {
            const uint32_t word = ReadBE(section.data + pc - section.base);
            if ((word >> 26) == 18 && (word & 1))
                directCallTargets.insert((word & 2) ? (word & 0x03FFFFFC) : pc + SignExtend(word & 0x03FFFFFC, 26));
        }
    }
    std::ofstream out(argv[5], std::ios::trunc); if (!out.good()) return 1;
    out << "SOURCE,PROPOSED_FUNCTION_START,PROPOSED_FUNCTION_END,TABLE_START,TABLE_END,RETURN_BLOCKS,EXTERNAL_BRANCHES,INBOUND_CALLS,CONFIDENCE,EVIDENCE\n";
    for (const uint32_t source : sources) {
        const auto it = switches.find(source); if (it == switches.end()) continue;
        const auto* section = static_cast<const Section*>(nullptr);
        for (const auto& candidate : image.sections) if ((candidate.flags & SectionFlags_Code) && InSection(candidate, source)) { section = &candidate; break; }
        Result result{}; result.source = source; result.tableEnd = source + 4 + static_cast<uint32_t>(it->second.labels.size() * 4);
        if (!section) continue;
        auto upper = std::upper_bound(starts.begin(), starts.end(), source);
        if (upper != starts.begin()) result.start = *std::prev(upper);
        if (!InSection(*section, result.start)) result.start = source;
        // Cross-reference evidence: direct branch-with-link targets only.
        for (uint32_t pc = section->base; InSection(*section, pc); pc += 4) {
            const uint32_t word = ReadBE(section->data + pc - section->base);
            if ((word >> 26) == 18 && (word & 1)) {
                const uint32_t target = (word & 2) ? (word & 0x03FFFFFC) : pc + SignExtend(word & 0x03FFFFFC, 26);
                if (target == result.start) ++result.inboundCalls;
            }
        }
        std::queue<uint32_t> work; std::set<uint32_t> visited; work.push(result.start);
        while (!work.empty()) {
            const uint32_t pc = work.front(); work.pop();
            if (!InSection(*section, pc) || tableBytes.count(pc) || !visited.emplace(pc).second) continue;
            const uint32_t word = ReadBE(section->data + pc - section->base); result.end = std::max(result.end, pc + 4);
            if (pc == source) result.reachedSource = true;
            if (word == 0x4E800020) { ++result.returns; continue; }
            if (word == 0x4E800420) {
                const auto table = switches.find(pc);
                if (table == switches.end()) { result.unknownIndirect = true; continue; }
                for (const uint32_t target : table->second.labels) { if (InSection(*section, target)) work.push(target); else ++result.externalBranches; }
                continue;
            }
            const uint32_t opcode = word >> 26;
            // Conditional branch-to-LR (for example bgtlr) has a return edge and a
            // fall-through edge. Count the former without terminating the latter.
            if (opcode == 19 && ((word >> 1) & 0x3FF) == 16) {
                ++result.returns;
                work.push(pc + 4);
            } else
            if (opcode == 18) {
                const uint32_t target = (word & 2) ? (word & 0x03FFFFFC) : pc + SignExtend(word & 0x03FFFFFC, 26);
                if (word & 1) work.push(pc + 4);
                else if (target != result.start && directCallTargets.count(target)) ++result.externalBranches;
                else if (InSection(*section, target)) work.push(target);
                else ++result.externalBranches;
            } else if (opcode == 16) {
                const uint32_t target = (word & 2) ? (word & 0xFFFC) : pc + SignExtend(word & 0xFFFC, 16);
                if (InSection(*section, target)) work.push(target); else ++result.externalBranches;
                work.push(pc + 4);
            } else work.push(pc + 4);
        }
        result.allLabelsReachable = std::all_of(it->second.labels.begin(), it->second.labels.end(), [&visited](uint32_t label) { return visited.count(label) != 0; });
        // The compiler's manual range must skip alignment padding as well as reachable
        // instructions; retain terminal-control-flow evidence separately in the CSV.
        uint32_t configuredEnd = result.end;
        while (InSection(*section, configuredEnd) && ReadBE(section->data + configuredEnd - section->base) == 0)
            configuredEnd += 4;
        const bool verified = result.reachedSource && result.allLabelsReachable && result.returns > 0 && !result.unknownIndirect && result.inboundCalls > 0;
        const char* confidence = verified ? "VERIFIED_CANDIDATE" : "AMBIGUOUS";
        out << "0x" << std::hex << std::uppercase << source << ",0x" << result.start << ",0x" << configuredEnd << ",0x" << (source + 4) << ",0x" << result.tableEnd
            << std::dec << ',' << result.returns << ',' << result.externalBranches << ',' << result.inboundCalls << ',' << confidence
            << ",map-entry+reachable-cfg; terminal-end=0x" << std::hex << result.end << "; labels=" << (result.allLabelsReachable ? "all" : "incomplete") << "; bctr=" << (result.unknownIndirect ? "unknown" : "known") << "\n";
    }
}
