#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct ModuleRange {
    DWORD64 base{};
    DWORD size{};
    std::string name;
    std::string path;
};

uint64_t FileTimeToUint64(const FILETIME& value) {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

std::vector<ModuleRange> EnumerateModules(DWORD process_id) {
    std::vector<ModuleRange> modules;
    const HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return modules;
    }

    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry)) {
        do {
            modules.push_back(ModuleRange{
                reinterpret_cast<DWORD64>(entry.modBaseAddr), entry.modBaseSize,
                entry.szModule, entry.szExePath});
        } while (Module32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

const ModuleRange* FindModule(const std::vector<ModuleRange>& modules,
                              DWORD64 address) {
    for (const auto& module : modules) {
        if (address >= module.base && address < module.base + module.size) {
            return &module;
        }
    }
    return nullptr;
}

void PrintAddress(HANDLE process, const std::vector<ModuleRange>& modules,
                  DWORD64 address) {
    alignas(SYMBOL_INFO) char symbol_storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;

    std::cout << "0x" << std::hex << std::setw(16) << std::setfill('0')
              << address << std::setfill(' ');
    if (const auto* module = FindModule(modules, address)) {
        std::cout << " " << module->name << "+0x" << (address - module->base);
    }
    if (SymFromAddr(process, address, &displacement, symbol)) {
        std::cout << " " << symbol->Name << "+0x" << displacement;
    }
    std::cout << std::dec;
}

void SampleThread(HANDLE process, DWORD process_id, DWORD thread_id,
                  const std::vector<ModuleRange>& modules) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                                   THREAD_SUSPEND_RESUME,
                               FALSE, thread_id);
    if (!thread) {
        std::cout << "THREAD id=" << thread_id
                  << " open_error=" << GetLastError() << "\n";
        return;
    }

    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(thread, &created, &exited, &kernel, &user);
    const DWORD suspend_result = SuspendThread(thread);
    if (suspend_result == static_cast<DWORD>(-1)) {
        std::cout << "THREAD id=" << thread_id
                  << " suspend_error=" << GetLastError() << "\n";
        CloseHandle(thread);
        return;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    const BOOL got_context = GetThreadContext(thread, &context);
    std::cout << "THREAD id=" << thread_id << " process=" << process_id
              << " kernel_100ns=" << FileTimeToUint64(kernel)
              << " user_100ns=" << FileTimeToUint64(user);
    if (!got_context) {
        std::cout << " context_error=" << GetLastError() << "\n";
        ResumeThread(thread);
        CloseHandle(thread);
        return;
    }
    std::cout << " rip=";
    PrintAddress(process, modules, context.Rip);
    std::cout << " rsp=0x" << std::hex << context.Rsp << std::dec << "\n";

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (unsigned depth = 0; depth < 64; ++depth) {
        if (depth != 0 &&
            !StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                         &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
            break;
        }
        if (!frame.AddrPC.Offset) {
            break;
        }
        std::cout << "  FRAME depth=" << depth << " address=";
        PrintAddress(process, modules, frame.AddrPC.Offset);
        std::cout << "\n";
    }

    ResumeThread(thread);
    CloseHandle(thread);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: runtime_stack_probe <process-id>\n";
        return 2;
    }
    const DWORD process_id = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 0));
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, process_id);
    if (!process) {
        std::cerr << "OpenProcess failed: " << GetLastError() << "\n";
        return 1;
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, FALSE);
    const auto modules = EnumerateModules(process_id);
    for (const auto& module : modules) {
        SymLoadModuleEx(process, nullptr, module.path.c_str(), module.name.c_str(),
                        module.base, module.size, nullptr, 0);
    }

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "thread snapshot failed: " << GetLastError() << "\n";
        SymCleanup(process);
        CloseHandle(process);
        return 1;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == process_id) {
                SampleThread(process, process_id, entry.th32ThreadID, modules);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    SymCleanup(process);
    CloseHandle(process);
    return 0;
}
