#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t kPreferredImageBase = 0x0000000140000000ull;

struct Symbol {
    uint64_t address{};
    std::string name;
    std::string object;
};

struct SampleKey {
    DWORD threadId{};
    uint64_t instructionPointer{};

    bool operator<(const SampleKey& other) const noexcept {
        if (threadId != other.threadId) return threadId < other.threadId;
        return instructionPointer < other.instructionPointer;
    }
};

struct ModuleInfo {
    uint64_t base{};
    uint64_t size{};
    std::string name;
};

bool ParseUnsigned(const char* text, uint64_t* value, int base = 10) {
    if (!text || !*text || !value) return false;
    const char* end = text;
    while (*end) ++end;
    const auto result = std::from_chars(text, end, *value, base);
    return result.ec == std::errc{} && result.ptr == end;
}

std::vector<Symbol> ReadMap(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("unable to open linker map");

    std::vector<Symbol> symbols;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string sectionOffset;
        std::string name;
        std::string addressText;
        std::string object;
        if (!(row >> sectionOffset >> name >> addressText >> object)) continue;
        if (sectionOffset.size() != 13 || sectionOffset[4] != ':') continue;
        uint64_t address{};
        const auto conversion = std::from_chars(
            addressText.data(), addressText.data() + addressText.size(), address, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != addressText.data() + addressText.size()) {
            continue;
        }
        if (address < kPreferredImageBase) continue;
        symbols.push_back({address, std::move(name), std::move(object)});
    }
    std::sort(symbols.begin(), symbols.end(), [](const Symbol& left, const Symbol& right) {
        return left.address < right.address;
    });
    symbols.erase(std::unique(symbols.begin(), symbols.end(), [](const Symbol& left,
                                                                 const Symbol& right) {
        return left.address == right.address && left.name == right.name;
    }), symbols.end());
    return symbols;
}

std::vector<ModuleInfo> ReadModules(DWORD processId) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                     processId);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    std::vector<ModuleInfo> modules;
    if (Module32FirstW(snapshot, &module)) {
        do {
            std::string name;
            for (const wchar_t character : std::wstring(module.szModule)) {
                name.push_back(character <= 0x7F ? static_cast<char>(character) : '?');
            }
            modules.push_back({reinterpret_cast<uint64_t>(module.modBaseAddr),
                               module.modBaseSize, std::move(name)});
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    std::sort(modules.begin(), modules.end(), [](const ModuleInfo& left,
                                                 const ModuleInfo& right) {
        return left.base < right.base;
    });
    return modules;
}

const ModuleInfo* FindModule(const std::vector<ModuleInfo>& modules, uint64_t address) {
    const auto it = std::upper_bound(
        modules.begin(), modules.end(), address,
        [](uint64_t value, const ModuleInfo& module) { return value < module.base; });
    if (it == modules.begin()) return nullptr;
    const ModuleInfo& module = *std::prev(it);
    return address - module.base < module.size ? &module : nullptr;
}

const Symbol* FindSymbol(const std::vector<Symbol>& symbols, uint64_t preferredAddress) {
    const auto it = std::upper_bound(
        symbols.begin(), symbols.end(), preferredAddress,
        [](uint64_t address, const Symbol& symbol) { return address < symbol.address; });
    return it == symbols.begin() ? nullptr : &*std::prev(it);
}

std::string ResolveSymbol(const std::vector<Symbol>& symbols, uint64_t preferredAddress) {
    const Symbol* symbol = FindSymbol(symbols, preferredAddress);
    if (!symbol) return "<before-image-symbols>";
    std::ostringstream output;
    output << symbol->name << "+0x" << std::hex << (preferredAddress - symbol->address)
           << " [" << symbol->object << ']';
    return output.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: runtime_thread_sampler <pid> <map> <samples> <interval-ms> "
                     "<thread-id> [thread-id ...]\n";
        return 2;
    }

    try {
        uint64_t parsedPid{};
        uint64_t parsedSamples{};
        uint64_t parsedInterval{};
        if (!ParseUnsigned(argv[1], &parsedPid) || parsedPid > UINT32_MAX ||
            !ParseUnsigned(argv[3], &parsedSamples) || !parsedSamples || parsedSamples > 100000 ||
            !ParseUnsigned(argv[4], &parsedInterval) || parsedInterval > 60000) {
            throw std::runtime_error("invalid numeric argument");
        }

        std::vector<DWORD> threadIds;
        for (int index = 5; index < argc; ++index) {
            uint64_t threadId{};
            if (!ParseUnsigned(argv[index], &threadId) || threadId > UINT32_MAX) {
                throw std::runtime_error("invalid thread id");
            }
            threadIds.push_back(static_cast<DWORD>(threadId));
        }

        const auto symbols = ReadMap(argv[2]);
        const auto modules = ReadModules(static_cast<DWORD>(parsedPid));
        if (modules.empty()) throw std::runtime_error("unable to locate target modules");
        const ModuleInfo& image = modules.front();
        const uint64_t imageBase = image.base;

        std::map<SampleKey, uint64_t> counts;
        std::unordered_map<DWORD, uint64_t> failures;
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t sample = 0; sample < parsedSamples; ++sample) {
            for (const DWORD threadId : threadIds) {
                const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                                                     THREAD_QUERY_LIMITED_INFORMATION,
                                                 FALSE, threadId);
                if (!thread) {
                    ++failures[threadId];
                    continue;
                }
                bool suspended = false;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(thread) != DWORD(-1)) {
                    suspended = true;
                    if (GetThreadContext(thread, &context)) {
                        ++counts[{threadId, context.Rip}];
                    } else {
                        ++failures[threadId];
                    }
                } else {
                    ++failures[threadId];
                }
                if (suspended) ResumeThread(thread);
                CloseHandle(thread);
            }
            if (parsedInterval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(parsedInterval));
            }
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        std::cout << "RUNTIME_THREAD_SAMPLE pid=" << parsedPid << " image_base=0x" << std::hex
                  << imageBase << std::dec << " samples=" << parsedSamples
                  << " interval_ms=" << parsedInterval << " elapsed_ms=" << elapsedMs
                  << " symbols=" << symbols.size() << '\n';

        std::unordered_map<std::string, uint64_t> categoryCounts;
        for (const auto& [key, count] : counts) {
            const ModuleInfo* module = FindModule(modules, key.instructionPointer);
            std::string category = "<unmapped>";
            if (module && module->base == imageBase) {
                const uint64_t preferredAddress =
                    kPreferredImageBase + (key.instructionPointer - imageBase);
                const Symbol* symbol = FindSymbol(symbols, preferredAddress);
                category = symbol ? symbol->object : "<main-image-unresolved>";
            } else if (module) {
                category = module->name;
            }
            categoryCounts[category] += count;
        }
        std::vector<std::pair<std::string, uint64_t>> categories(categoryCounts.begin(),
                                                                  categoryCounts.end());
        std::sort(categories.begin(), categories.end(), [](const auto& left, const auto& right) {
            return left.second > right.second;
        });
        uint64_t allCaptured{};
        for (const auto& [category, count] : categories) allCaptured += count;
        std::cout << "CATEGORY_SUMMARY captured=" << allCaptured << '\n';
        for (const auto& [category, count] : categories) {
            std::cout << "  count=" << count << " percent=" << std::fixed
                      << std::setprecision(1)
                      << (allCaptured ? (100.0 * count / allCaptured) : 0.0)
                      << " category=" << category << '\n';
        }

        for (const DWORD threadId : threadIds) {
            std::vector<std::pair<uint64_t, uint64_t>> rows;
            uint64_t total{};
            for (const auto& [key, count] : counts) {
                if (key.threadId != threadId) continue;
                rows.emplace_back(key.instructionPointer, count);
                total += count;
            }
            std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
            std::cout << "THREAD id=" << threadId << " captured=" << total
                      << " failed=" << failures[threadId] << '\n';
            const size_t limit = std::min<size_t>(rows.size(), 16);
            for (size_t index = 0; index < limit; ++index) {
                const auto [rip, count] = rows[index];
                std::cout << "  count=" << count << " rip=0x" << std::hex << rip << std::dec;
                const ModuleInfo* module = FindModule(modules, rip);
                if (module && module->base == imageBase) {
                    const uint64_t preferredAddress = kPreferredImageBase + (rip - imageBase);
                    std::cout << " preferred=0x" << std::hex << preferredAddress << std::dec
                              << " symbol=" << ResolveSymbol(symbols, preferredAddress);
                } else if (module) {
                    std::cout << " symbol=" << module->name << "+0x" << std::hex
                              << (rip - module->base) << std::dec;
                } else {
                    std::cout << " symbol=<unmapped>";
                }
                std::cout << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "runtime_thread_sampler: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
