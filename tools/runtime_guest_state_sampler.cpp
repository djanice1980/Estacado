#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
constexpr uint64_t kPreferredImageBase = 0x140000000ull;
// Preferred address of _tls_index in the runtime image currently produced by
// build/runtime_optimized/TheDarkness.map. This tool is intentionally
// tied to the inspected binary: using another link's address can return a
// plausible TLS slot containing unrelated per-thread state.
constexpr uint64_t kTlsIndexPreferred = 0x145D19D10ull;
constexpr size_t kTebTlsArrayOffset = 0x58;
constexpr size_t kActiveContextOffset = 0x08;
constexpr size_t kActiveBaseOffset = 0x10;
constexpr size_t kActiveFunctionOffset = 0x18;
constexpr size_t kActiveNonHelperOffset = 0x20;
constexpr size_t kGuestThreadIdOffset = 0x120;
constexpr size_t kContextR1Offset = 0x10;
constexpr size_t kContextR3Offset = 0x00;
constexpr size_t kContextR4Offset = 0x20;
constexpr size_t kContextR5Offset = 0x28;
constexpr size_t kContextR31Offset = 0xF8;
constexpr size_t kContextR30Offset = 0xF0;
constexpr size_t kContextLrOffset = 0x100;

struct ThreadBasicInformation {
    NTSTATUS exitStatus;
    void* tebBaseAddress;
    CLIENT_ID clientId;
    ULONG_PTR affinityMask;
    LONG priority;
    LONG basePriority;
};

using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, THREADINFOCLASS, void*, ULONG, ULONG*);

template <typename T>
bool Read(HANDLE process, uint64_t address, T* value) {
    SIZE_T read{};
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), value,
                             sizeof(T), &read) && read == sizeof(T);
}

uint64_t ImageBase(DWORD processId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const bool found = Module32FirstW(snapshot, &entry) != FALSE;
    CloseHandle(snapshot);
    return found ? reinterpret_cast<uint64_t>(entry.modBaseAddr) : 0;
}

std::vector<DWORD> Threads(DWORD processId) {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) result.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

struct GuestState {
    DWORD hostThread{};
    uint32_t guestThread{};
    uint32_t function{};
    uint32_t nonHelper{};
    uint64_t lr{};
    uint64_t r1{};
    uint64_t r3{};
    uint64_t r4{};
    uint64_t r5{};
    uint64_t r6{};
    uint64_t r7{};
    uint64_t r8{};
    uint64_t r9{};
    uint64_t r10{};
    uint64_t r11{};
    uint64_t r27{};
    uint64_t r31{};
    uint64_t r30{};
    uint64_t r13{};
    uint64_t context{};
    uint64_t base{};

    auto key() const {
        return std::tie(hostThread, guestThread, function, nonHelper, lr, r1, r3, r4, r5,
                        r6, r7, r8, r9, r10, r11, r27, r30, r31);
    }
    bool operator<(const GuestState& other) const { return key() < other.key(); }
};

bool CaptureFromTlsBlock(HANDLE process, DWORD hostThread, uint64_t tlsBlock,
                         GuestState* state) {
    uint64_t context{};
    uint64_t base{};
    uint32_t function{};
    uint32_t nonHelper{};
    uint32_t guestThread{};
    if (!tlsBlock ||
        !Read(process, tlsBlock + kActiveContextOffset, &context) ||
        !Read(process, tlsBlock + kActiveBaseOffset, &base) ||
        !Read(process, tlsBlock + kActiveFunctionOffset, &function) ||
        !Read(process, tlsBlock + kActiveNonHelperOffset, &nonHelper) ||
        !Read(process, tlsBlock + kGuestThreadIdOffset, &guestThread)) {
        return false;
    }
    if ((!guestThread && !function && !nonHelper) || guestThread > 0x10000u) return false;

    *state = {};
    state->hostThread = hostThread;
    state->guestThread = guestThread;
    state->function = function;
    state->nonHelper = nonHelper;
    state->context = context;
    state->base = base;
    if (context && base) {
        Read(process, context + kContextLrOffset, &state->lr);
        Read(process, context + kContextR1Offset, &state->r1);
        Read(process, context + kContextR3Offset, &state->r3);
        Read(process, context + kContextR4Offset, &state->r4);
        Read(process, context + kContextR5Offset, &state->r5);
        Read(process, context + 0x30, &state->r6);
        Read(process, context + 0x38, &state->r7);
        Read(process, context + 0x40, &state->r8);
        Read(process, context + 0x48, &state->r9);
        Read(process, context + 0x50, &state->r10);
        Read(process, context + 0x58, &state->r11);
        Read(process, context + 0xD8, &state->r27);
        Read(process, context + kContextR30Offset, &state->r30);
        Read(process, context + kContextR31Offset, &state->r31);
        Read(process, context + 0x68, &state->r13);
    }
    return true;
}

bool Capture(HANDLE process, NtQueryInformationThreadFn queryThread, DWORD hostThread,
             uint32_t tlsIndex, GuestState* state) {
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, hostThread);
    if (!thread) return false;
    bool suspended = false;
    bool captured = false;
    if (SuspendThread(thread) != DWORD(-1)) {
        suspended = true;
        ThreadBasicInformation basic{};
        if (queryThread(thread, static_cast<THREADINFOCLASS>(0), &basic, sizeof(basic), nullptr) >= 0) {
            uint64_t tlsArray{};
            uint64_t tlsBlock{};
            const uint64_t teb = reinterpret_cast<uint64_t>(basic.tebBaseAddress);
            if (Read(process, teb + kTebTlsArrayOffset, &tlsArray) && tlsArray &&
                Read(process, tlsArray + uint64_t(tlsIndex) * sizeof(uint64_t), &tlsBlock) &&
                tlsBlock) {
                captured = CaptureFromTlsBlock(process, hostThread, tlsBlock, state);
            }
            // Some staged title builds expose a linker _tls_index value which is
            // not a usable vector subscript to an external observer. Fall back to
            // scanning the 64 primary TLS vector entries for the runtime's
            // distinctive per-thread block. This is observational only and keeps
            // the inspected process suspended for the same bounded interval.
            if (!captured && tlsArray) {
                for (uint32_t slot = 0; slot < 64u && !captured; ++slot) {
                    uint64_t candidate{};
                    if (Read(process, tlsArray + uint64_t(slot) * sizeof(uint64_t),
                             &candidate)) {
                        captured = CaptureFromTlsBlock(process, hostThread, candidate, state);
                    }
                }
            }
        }
    }
    if (suspended) ResumeThread(thread);
    CloseHandle(thread);
    return captured;
}

uint32_t ByteSwap32(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000FF00u) |
           ((value << 8) & 0x00FF0000u) | (value << 24);
}

bool ReadGuestU32(HANDLE process, const GuestState& state, uint32_t address, uint32_t* value) {
    uint32_t native{};
    if (!state.base || !Read(process, state.base + address, &native)) return false;
    *value = ByteSwap32(native);
    return true;
}

uint64_t ByteSwap64(uint64_t value) {
    return (uint64_t(ByteSwap32(uint32_t(value))) << 32) | ByteSwap32(uint32_t(value >> 32));
}

bool ReadGuestU64(HANDLE process, const GuestState& state, uint32_t address, uint64_t* value) {
    uint64_t native{};
    if (!state.base || !Read(process, state.base + address, &native)) return false;
    *value = ByteSwap64(native);
    return true;
}

struct GpuWaitAggregate {
    uint64_t samples{};
    std::set<uint32_t> managers;
    std::set<uint32_t> observedReadPointers;
    std::set<uint32_t> currentReadPointers;
    std::set<uint32_t> owners;
    std::set<uint32_t> currentThreads;
    uint32_t firstTick{};
    uint32_t lastTick{};
    uint32_t firstStoredTick{};
    uint32_t lastStoredTick{};
    uint8_t flag{};
};

void ObserveGpuWait(HANDLE process, const GuestState& state, GpuWaitAggregate* aggregate) {
    if ((!state.r31 && !state.r3) || !state.r13 || !state.base) return;
    const uint32_t waitState = static_cast<uint32_t>(state.r31 ? state.r31 : state.r3);
    uint32_t manager{};
    uint32_t observed{};
    uint32_t storedTick{};
    uint32_t readPointerAddress{};
    uint32_t currentReadPointer{};
    uint32_t owner{};
    uint32_t processor{};
    uint32_t tick{};
    uint32_t currentThread{};
    uint8_t flag{};
    if (!ReadGuestU32(process, state, waitState, &manager) || !manager ||
        !ReadGuestU32(process, state, waitState + 8, &observed) ||
        !ReadGuestU32(process, state, waitState + 12, &storedTick) ||
        !ReadGuestU32(process, state, manager + 10896, &readPointerAddress) ||
        !readPointerAddress ||
        !ReadGuestU32(process, state, readPointerAddress, &currentReadPointer) ||
        !ReadGuestU32(process, state, manager + 10888, &owner) ||
        !ReadGuestU32(process, state, static_cast<uint32_t>(state.r13) + 256, &processor) ||
        !processor || !ReadGuestU32(process, state, processor + 88, &tick) ||
        !ReadGuestU32(process, state, processor + 332, &currentThread) ||
        !Read(process, state.base + manager + 10941, &flag)) {
        return;
    }
    if (!aggregate->samples) {
        aggregate->firstTick = tick;
        aggregate->firstStoredTick = storedTick;
    }
    ++aggregate->samples;
    aggregate->managers.insert(manager);
    aggregate->observedReadPointers.insert(observed);
    aggregate->currentReadPointers.insert(currentReadPointer);
    aggregate->owners.insert(owner);
    aggregate->currentThreads.insert(currentThread);
    aggregate->lastTick = tick;
    aggregate->lastStoredTick = storedTick;
    aggregate->flag = flag;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: runtime_guest_state_sampler <pid> <samples> <interval-ms> "
                     "[--focus=<guest-function>] [thread-id ...]\n";
        return 2;
    }
    try {
        const DWORD processId = static_cast<DWORD>(std::stoul(argv[1], nullptr, 0));
        const uint32_t sampleCount = static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0));
        const uint32_t intervalMs = static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0));
        if (!processId || !sampleCount || sampleCount > 100000 || intervalMs > 60000) {
            throw std::runtime_error("invalid numeric argument");
        }
        std::vector<DWORD> threadIds;
        std::set<uint32_t> focusAddresses;
        for (int index = 4; index < argc; ++index) {
            const std::string argument(argv[index]);
            constexpr const char* kFocusPrefix = "--focus=";
            if (argument.rfind(kFocusPrefix, 0) == 0) {
                focusAddresses.insert(static_cast<uint32_t>(
                    std::stoul(argument.substr(std::char_traits<char>::length(kFocusPrefix)),
                               nullptr, 0)));
            } else {
                threadIds.push_back(static_cast<DWORD>(std::stoul(argument, nullptr, 0)));
            }
        }
        if (threadIds.empty()) threadIds = Threads(processId);

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (!process) throw std::runtime_error("unable to open process");
        const uint64_t imageBase = ImageBase(processId);
        if (!imageBase) throw std::runtime_error("unable to find image base");
        uint32_t tlsIndex{};
        if (!Read(process, imageBase + (kTlsIndexPreferred - kPreferredImageBase), &tlsIndex)) {
            throw std::runtime_error("unable to read TLS index");
        }
        auto queryThread = reinterpret_cast<NtQueryInformationThreadFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        if (!queryThread) throw std::runtime_error("unable to resolve NtQueryInformationThread");

        std::map<GuestState, uint64_t> counts;
        std::map<DWORD, uint64_t> failures;
        std::map<DWORD, GpuWaitAggregate> gpuWaits;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t sample = 0; sample < sampleCount; ++sample) {
            for (DWORD threadId : threadIds) {
                GuestState state{};
                if (Capture(process, queryThread, threadId, tlsIndex, &state)) {
                    ++counts[state];
                    if (state.function == 0x8285DEB8u || uint32_t(state.lr) == 0x8285DEC0u) {
                        ObserveGpuWait(process, state, &gpuWaits[threadId]);
                    }
                } else {
                    ++failures[threadId];
                }
            }
            if (intervalMs) std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "RUNTIME_GUEST_STATE_SAMPLE pid=" << processId << " image_base=0x" << std::hex
                  << imageBase << std::dec << " tls_index=" << tlsIndex << " samples="
                  << sampleCount << " interval_ms=" << intervalMs << " elapsed_ms=" << elapsed
                  << " threads=" << threadIds.size() << '\n';

        std::map<std::pair<DWORD, uint32_t>, std::vector<std::pair<GuestState, uint64_t>>> grouped;
        for (const auto& [state, count] : counts) grouped[{state.hostThread, state.guestThread}].push_back({state, count});
        for (auto& [thread, states] : grouped) {
            std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
            uint64_t total{};
            for (const auto& item : states) total += item.second;
            std::cout << "THREAD host=" << thread.first << " guest=" << thread.second
                      << " captured=" << total << " unique=" << states.size()
                      << " failed=" << failures[thread.first] << '\n';
            const size_t limit = std::min<size_t>(states.size(), 12);
            for (size_t index = 0; index < limit; ++index) {
                const auto& [state, count] = states[index];
                std::cout << "  count=" << count << " function=0x" << std::hex << std::setw(8)
                          << std::setfill('0') << state.function << " nonhelper=0x" << std::setw(8)
                          << state.nonHelper << " lr=0x" << std::setw(8) << uint32_t(state.lr)
                          << " r1=0x" << std::setw(8) << uint32_t(state.r1)
                          << " r3=0x" << std::setw(8) << uint32_t(state.r3)
                          << " r30=0x" << std::setw(8) << uint32_t(state.r30)
                          << " r31=0x" << std::setw(8) << uint32_t(state.r31) << std::dec
                          << std::setfill(' ') << '\n';
            }
            std::map<uint32_t, uint64_t> functionCounts;
            for (const auto& [state, count] : states) functionCounts[state.function] += count;
            std::vector<std::pair<uint32_t, uint64_t>> rankedFunctions(functionCounts.begin(),
                                                                       functionCounts.end());
            std::sort(rankedFunctions.begin(), rankedFunctions.end(), [](const auto& left,
                                                                         const auto& right) {
                if (left.second != right.second) return left.second > right.second;
                return left.first < right.first;
            });
            const size_t functionLimit = std::min<size_t>(rankedFunctions.size(), 512);
            std::cout << "  FUNCTION_FAMILIES unique=" << rankedFunctions.size()
                      << " reported=" << functionLimit << '\n';
            for (size_t index = 0; index < functionLimit; ++index) {
                std::cout << "    count=" << rankedFunctions[index].second << " function=0x"
                          << std::hex << std::setw(8) << std::setfill('0')
                          << rankedFunctions[index].first << std::dec << std::setfill(' ') << '\n';
            }
            if (!focusAddresses.empty()) {
                size_t focusedStates{};
                std::cout << "  FOCUS_STATES\n";
                for (const auto& [state, count] : states) {
                    if (!focusAddresses.contains(state.function)) continue;
                    if (focusedStates++ == 128) {
                        std::cout << "    ...\n";
                        break;
                    }
                    std::cout << "    count=" << count << " function=0x" << std::hex
                              << std::setw(8) << std::setfill('0') << state.function
                              << " nonhelper=0x" << std::setw(8) << state.nonHelper
                              << " lr=0x" << std::setw(8) << uint32_t(state.lr)
                              << " r1=0x" << std::setw(8) << uint32_t(state.r1)
                              << " r3=0x" << std::setw(8) << uint32_t(state.r3)
                              << " r4=0x" << std::setw(8) << uint32_t(state.r4)
                              << " r5=0x" << std::setw(8) << uint32_t(state.r5)
                              << " r6=0x" << std::setw(8) << uint32_t(state.r6)
                              << " r7=0x" << std::setw(8) << uint32_t(state.r7)
                              << " r8=0x" << std::setw(8) << uint32_t(state.r8)
                              << " r9=0x" << std::setw(8) << uint32_t(state.r9)
                              << " r10=0x" << std::setw(8) << uint32_t(state.r10)
                              << " r11=0x" << std::setw(8) << uint32_t(state.r11)
                              << " r27=0x" << std::setw(8) << uint32_t(state.r27)
                              << " r30=0x" << std::setw(8) << uint32_t(state.r30)
                              << " r31=0x" << std::setw(8) << uint32_t(state.r31)
                              << std::dec << std::setfill(' ') << '\n';
                }
            }
            const GuestState& topState = states.front().first;
            std::cout << "  MEMORY base=0x" << std::hex << topState.base
                      << " context=0x" << topState.context << std::dec << '\n';
            uint32_t stack = static_cast<uint32_t>(topState.r1);
            std::cout << "  STACK r1=0x" << std::hex << stack;
            for (size_t frame = 0; frame < 12 && stack; ++frame) {
                uint32_t previous{};
                if (!ReadGuestU32(process, topState, stack, &previous) ||
                    previous <= stack || previous - stack > 0x10000u) {
                    break;
                }
                uint32_t savedLr{};
                if (!ReadGuestU32(process, topState, previous - 8, &savedLr)) break;
                std::cout << " -> {sp=0x" << previous << ",lr=0x" << savedLr;
                for (uint32_t reg = 26; reg <= 31; ++reg) {
                    const uint32_t offset = 56u - (reg - 26u) * 8u;
                    uint64_t saved{};
                    if (ReadGuestU64(process, topState, previous - offset, &saved)) {
                        std::cout << ",r" << std::dec << reg << "=0x" << std::hex
                                  << uint32_t(saved);
                        if ((reg == 30 || reg == 31) && uint32_t(saved) >= 0x10000u) {
                            uint32_t vtable{};
                            uint32_t slot80{};
                            if (ReadGuestU32(process, topState, uint32_t(saved), &vtable) &&
                                vtable >= 0x82000000u && vtable < 0x83000000u &&
                                ReadGuestU32(process, topState, vtable + 80u, &slot80)) {
                                std::cout << "[vt=0x" << vtable << ",vt80=0x" << slot80 << ']';
                            }
                        }
                    }
                }
                std::cout << '}';
                stack = previous;
            }
            std::cout << std::dec << '\n';
        }
        for (const auto& [hostThread, wait] : gpuWaits) {
            if (!wait.samples) continue;
            std::cout << "GPU_WAIT host=" << hostThread << " samples=" << wait.samples
                      << " managers=" << wait.managers.size()
                      << " observed_rptr_values=" << wait.observedReadPointers.size()
                      << " current_rptr_values=" << wait.currentReadPointers.size()
                      << " owner_values=" << wait.owners.size()
                      << " current_thread_values=" << wait.currentThreads.size()
                      << " tick_first=0x" << std::hex << wait.firstTick
                      << " tick_last=0x" << wait.lastTick
                      << " stored_tick_first=0x" << wait.firstStoredTick
                      << " stored_tick_last=0x" << wait.lastStoredTick
                      << " flag=0x" << unsigned(wait.flag) << std::dec << '\n';
            auto printValues = [](const char* label, const std::set<uint32_t>& values) {
                std::cout << "  " << label << '=';
                size_t emitted{};
                for (uint32_t value : values) {
                    if (emitted++ == 8) {
                        std::cout << ",...";
                        break;
                    }
                    if (emitted > 1) std::cout << ',';
                    std::cout << "0x" << std::hex << value << std::dec;
                }
                std::cout << '\n';
            };
            printValues("manager", wait.managers);
            printValues("observed_rptr", wait.observedReadPointers);
            printValues("current_rptr", wait.currentReadPointers);
            printValues("owner", wait.owners);
            printValues("current_thread", wait.currentThreads);
        }
        CloseHandle(process);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime_guest_state_sampler: " << error.what() << '\n';
        return 1;
    }
}
