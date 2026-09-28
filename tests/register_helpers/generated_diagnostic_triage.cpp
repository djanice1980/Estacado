#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>

struct OpcodeRecord
{
    size_t occurrences{};
    std::string firstAddress{};
    std::set<std::string> functions{};
};

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: generated_diagnostic_triage <recompiler-log> <generated-source-directory>\n";
        return 2;
    }

    std::ifstream log(argv[1]);
    if (!log)
    {
        std::cerr << "Unable to read recompiler log.\n";
        return 1;
    }

    std::map<std::string, OpcodeRecord> records;
    const std::regex unrecognized{ R"(^Unrecognized instruction at 0x([0-9A-F]+): ([A-Za-z0-9_.]+)$)" };
    std::string line;
    while (std::getline(log, line))
    {
        std::smatch match;
        if (!std::regex_match(line, match, unrecognized))
            continue;
        auto& record = records[match[2].str()];
        ++record.occurrences;
        if (record.firstAddress.empty())
            record.firstAddress = "0x" + match[1].str();
    }

    const std::filesystem::path generatedDirectory{ argv[2] };
    const std::regex functionHeader{ R"(PPC_FUNC_IMPL\((?:__imp__)?sub_([0-9A-F]+)\))" };
    const std::regex instructionComment{ R"(^\s*//\s*([A-Za-z0-9_.]+)(?:\s|$))" };
    for (const auto& entry : std::filesystem::directory_iterator(generatedDirectory))
    {
        const auto filename = entry.path().filename().string();
        if (!entry.is_regular_file() || filename.rfind("ppc_recomp.", 0) != 0 || entry.path().extension() != ".cpp")
            continue;

        std::ifstream source(entry.path());
        std::string function{};
        while (std::getline(source, line))
        {
            std::smatch match;
            if (std::regex_search(line, match, functionHeader))
            {
                function = "0x" + match[1].str();
                continue;
            }
            if (function.empty() || !std::regex_search(line, match, instructionComment))
                continue;
            const auto found = records.find(match[1].str());
            if (found != records.end())
                found->second.functions.insert(function);
        }
    }

    std::cout << "opcode,occurrences,function_count,first_example_address\n";
    std::vector<std::pair<std::string, OpcodeRecord>> sorted(records.begin(), records.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right)
    {
        return left.second.occurrences != right.second.occurrences
            ? left.second.occurrences > right.second.occurrences
            : left.first < right.first;
    });
    for (const auto& [opcode, record] : sorted)
        std::cout << opcode << ',' << record.occurrences << ',' << record.functions.size() << ',' << record.firstAddress << '\n';
}
