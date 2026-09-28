#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <disasm.h>
#include <file.h>
#include <image.h>

namespace {

uint32_t ReadBigEndian(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

bool IsCodeAddress(const Image& image, uint32_t address) {
    for (const auto& section : image.sections) {
        if ((section.flags & SectionFlags_Code) && address >= section.base &&
            address < section.base + section.size) {
            return true;
        }
    }
    return false;
}

std::vector<std::vector<uint32_t>> ReadConfiguredTables(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("unable to open switch config: ") + path);

    const std::regex addressPattern{R"(0x([0-9A-Fa-f]+))"};
    std::vector<std::vector<uint32_t>> tables;
    std::vector<uint32_t> current;
    bool inLabels = false;
    std::string line;
    while (std::getline(input, line)) {
        size_t parseStart = 0;
        if (!inLabels) {
            const size_t labels = line.find("labels");
            if (labels == std::string::npos) continue;
            const size_t opening = line.find('[', labels);
            if (opening == std::string::npos) continue;
            inLabels = true;
            current.clear();
            parseStart = opening + 1;
        }

        const std::string values = line.substr(parseStart);
        for (std::sregex_iterator it(values.begin(), values.end(), addressPattern), end;
             it != end; ++it) {
            current.push_back(static_cast<uint32_t>(std::stoul((*it)[1].str(), nullptr, 16)));
        }
        if (line.find(']', parseStart) != std::string::npos) {
            if (!current.empty()) tables.push_back(current);
            current.clear();
            inLabels = false;
        }
    }
    return tables;
}

bool ContainsTable(const std::vector<std::vector<uint32_t>>& configured,
                   const std::vector<uint32_t>& candidate) {
    return std::find(configured.begin(), configured.end(), candidate) != configured.end();
}

bool IsRangeBranch(int id) {
    return id == PPC_INST_BGT || id == PPC_INST_BGTLR || id == PPC_INST_BLE ||
           id == PPC_INST_BLELR;
}

struct Candidate {
    uint32_t compare{};
    uint32_t branch{};
    uint32_t bctr{};
    uint32_t reg{};
    std::vector<uint32_t> targets;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: inline_switch_scan <default.xex> <preserved-switches.toml> "
                     "<overlay.toml>\n";
        return 2;
    }

    try {
        const auto file = LoadFile(argv[1]);
        const auto image = Image::ParseImage(file.data(), file.size());
        if (!image.data) throw std::runtime_error("unable to load XEX image");
        const auto preserved = ReadConfiguredTables(argv[2]);
        const auto overlay = ReadConfiguredTables(argv[3]);

        std::vector<Candidate> candidates;
        for (const auto& section : image.sections) {
            if (!(section.flags & SectionFlags_Code)) continue;
            for (uint32_t offset = 0; offset + 4 <= section.size; offset += 4) {
                const uint32_t bctr = static_cast<uint32_t>(section.base + offset);
                if (ReadBigEndian(section.data + offset) != 0x4E800420u) continue;

                uint32_t branchAddress{};
                uint32_t compareAddress{};
                uint32_t branchCr = UINT32_MAX;
                uint32_t reg{};
                uint32_t maximum{};
                for (uint32_t back = 0; back < 32 && back * 4 <= offset; ++back) {
                    const uint32_t instructionAddress = bctr - back * 4;
                    ppc_insn instruction{};
                    ppc::Disassemble(section.data + offset - back * 4,
                                     instructionAddress, instruction);
                    if (!instruction.opcode) continue;
                    if (branchCr == UINT32_MAX && IsRangeBranch(instruction.opcode->id)) {
                        branchCr = instruction.operands[0];
                        branchAddress = instructionAddress;
                    } else if (branchCr != UINT32_MAX &&
                               instruction.opcode->id == PPC_INST_CMPLWI &&
                               instruction.operands[0] == branchCr) {
                        compareAddress = instructionAddress;
                        reg = instruction.operands[1];
                        maximum = instruction.operands[2];
                        break;
                    }
                }
                uint32_t count{};
                if (compareAddress) {
                    // Large recognized tables are outside this narrow audit;
                    // truncating them would create false configuration
                    // mismatches. XenonAnalyse already handles their local
                    // range proof.
                    if (maximum < 1 || maximum >= 64) continue;
                    count = maximum + 1;
                } else {
                    // Some title helpers are only called with an already
                    // constrained index, so no local compare exists. Their
                    // inline tables still have a stronger signature than a
                    // generic indirect call: bctr is followed immediately by
                    // two or more aligned addresses into executable sections.
                    while (count < 64 && offset + 4 + (count + 1) * 4 <= section.size) {
                        const uint32_t target =
                            ReadBigEndian(section.data + offset + 4 + count * 4);
                        if ((target & 3u) || !IsCodeAddress(image, target)) break;
                        ++count;
                    }
                    if (count < 2) continue;
                }
                if (offset + 4 + count * 4 > section.size) continue;
                std::vector<uint32_t> targets;
                targets.reserve(count);
                bool valid = true;
                for (uint32_t index = 0; index < count; ++index) {
                    const uint32_t target = ReadBigEndian(section.data + offset + 4 + index * 4);
                    if ((target & 3u) || !IsCodeAddress(image, target)) {
                        valid = false;
                        break;
                    }
                    targets.push_back(target);
                }
                if (valid) candidates.push_back({compareAddress, branchAddress, bctr, reg,
                                                 std::move(targets)});
            }
        }

        size_t preservedCount{};
        size_t overlayCount{};
        size_t unconfiguredCount{};
        std::cout << std::hex << std::uppercase << std::setfill('0');
        for (const auto& candidate : candidates) {
            const bool inPreserved = ContainsTable(preserved, candidate.targets);
            const bool inOverlay = ContainsTable(overlay, candidate.targets);
            const char* classification = "UNCONFIGURED";
            if (inPreserved) {
                classification = "PRESERVED";
                ++preservedCount;
            } else if (inOverlay) {
                classification = "OVERLAY";
                ++overlayCount;
            } else {
                ++unconfiguredCount;
            }
            std::cout << "candidate bctr=0x" << std::setw(8) << candidate.bctr
                      << " compare=0x" << std::setw(8) << candidate.compare
                      << " branch=0x" << std::setw(8) << candidate.branch
                      << std::dec << " r=" << candidate.reg
                      << " count=" << candidate.targets.size()
                      << " classification=" << classification << " targets=";
            std::cout << std::hex;
            for (size_t index = 0; index < candidate.targets.size(); ++index) {
                if (index) std::cout << ',';
                std::cout << "0x" << std::setw(8) << candidate.targets[index];
            }
            std::cout << '\n';
        }
        std::cout << std::dec << "summary candidates=" << candidates.size()
                  << " preserved=" << preservedCount << " overlay=" << overlayCount
                  << " unconfigured=" << unconfiguredCount << '\n';
    } catch (const std::exception& error) {
        std::cerr << "inline_switch_scan: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
