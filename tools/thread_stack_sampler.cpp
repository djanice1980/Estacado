// Bounded external call-stack sampler for one running process.
//
// Each sample suspends a selected thread only long enough to copy its
// register context and the top of its stack, resumes it, and then unwinds the
// copy with dbghelp (x64 .pdata). Addresses are attributed with linker maps,
// so no PDB is needed. Output is per-thread exclusive/inclusive function
// counts plus collapsed stacks. Sampling perturbs the target slightly; do not
// combine it with frame-timing acceptance.
//
// thread_stack_sampler --pid N --seconds S [--hz 1000] [--thread-name NAME]...
//     [--tid N]... [--top-cpu N] [--map module.dll=path.map]... [--timeline]
//     --out PREFIX
//
// --timeline also writes PREFIX.timeline.csv (qpc, tid, thread cycle count,
// stack id per sample) and PREFIX.stack_ids.txt, so off-CPU intervals of a
// thread (cycle delta well below the wall delta) can be placed in time and
// attributed to the stack at which the thread was not running.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Options {
  DWORD pid = 0;
  double seconds = 10.0;
  double hz = 1000.0;
  std::vector<std::wstring> thread_names;
  std::vector<DWORD> tids;
  uint32_t top_cpu = 0;
  bool timeline = false;
  std::vector<std::pair<std::string, std::string>> maps;
  std::string out = "stack_samples";
  uint32_t stack_bytes = 96 * 1024;
  uint32_t max_frames = 128;
  // Optional start trigger: wait until a named thread stays busy.
  std::wstring wait_thread;
  double wait_percent = 0.0;
  uint32_t wait_seconds = 0;
  uint32_t delay_seconds = 0;
  uint32_t wait_timeout_seconds = 1800;
  // Optional start trigger: wait for a global key press (virtual-key code),
  // e.g. the same F9 phase marker the game's diagnostic observer logs.
  uint32_t wait_key_vk = 0;
  // Optional: histogram exact leaf addresses (module+RVA) of functions whose
  // symbol contains this text, for instruction-level attribution.
  std::string leaf_detail;
};

struct ThreadTarget {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  std::wstring name;
  uint64_t stack_base = 0;
  uint64_t samples = 0;
  uint64_t failed = 0;
  uint64_t suspended_ticks = 0;
  uint64_t max_suspended_ticks = 0;
  uint64_t cpu_start_100ns = 0;
  uint64_t cpu_end_100ns = 0;
  std::unordered_map<std::string, uint64_t> stacks;  // key: packed addresses
};

struct MapSymbol {
  uint64_t rva;
  std::string name;
};

struct Module {
  uint64_t base = 0;
  uint64_t size = 0;
  std::string name;  // lower-case file name
  std::string path;
  std::vector<MapSymbol> symbols;  // sorted, from a linker map
};

typedef NTSTATUS(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG,
                                                     PULONG);

struct ThreadBasicInformation {
  NTSTATUS ExitStatus;
  PVOID TebBaseAddress;
  CLIENT_ID ClientId;
  KAFFINITY AffinityMask;
  LONG Priority;
  LONG BasePriority;
};

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()),
                                 nullptr, 0, nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(),
                      size, nullptr, nullptr);
  return result;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()),
                                 nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(),
                      size);
  return result;
}

std::string Lower(std::string text) {
  for (char& c : text) c = char(tolower((unsigned char)c));
  return text;
}

bool ParseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* value = nullptr;
    if (arg == "--pid" && (value = next())) {
      options.pid = DWORD(strtoul(value, nullptr, 10));
    } else if (arg == "--seconds" && (value = next())) {
      options.seconds = atof(value);
    } else if (arg == "--hz" && (value = next())) {
      options.hz = atof(value);
    } else if (arg == "--thread-name" && (value = next())) {
      options.thread_names.push_back(Widen(value));
    } else if (arg == "--tid" && (value = next())) {
      options.tids.push_back(DWORD(strtoul(value, nullptr, 10)));
    } else if (arg == "--timeline") {
      options.timeline = true;
    } else if (arg == "--top-cpu" && (value = next())) {
      options.top_cpu = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--map" && (value = next())) {
      std::string text = value;
      size_t eq = text.find('=');
      if (eq == std::string::npos) return false;
      options.maps.push_back({Lower(text.substr(0, eq)), text.substr(eq + 1)});
    } else if (arg == "--out" && (value = next())) {
      options.out = value;
    } else if (arg == "--stack-bytes" && (value = next())) {
      options.stack_bytes = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--max-frames" && (value = next())) {
      options.max_frames = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--wait-busy-thread" && (value = next())) {
      options.wait_thread = Widen(value);
    } else if (arg == "--wait-busy-percent" && (value = next())) {
      options.wait_percent = atof(value);
    } else if (arg == "--wait-busy-seconds" && (value = next())) {
      options.wait_seconds = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--delay-seconds" && (value = next())) {
      options.delay_seconds = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--wait-timeout-seconds" && (value = next())) {
      options.wait_timeout_seconds = uint32_t(strtoul(value, nullptr, 10));
    } else if (arg == "--wait-key-vk" && (value = next())) {
      options.wait_key_vk = uint32_t(strtoul(value, nullptr, 0));
    } else if (arg == "--leaf-detail" && (value = next())) {
      options.leaf_detail = value;
    } else {
      return false;
    }
  }
  return options.pid && options.seconds > 0 && options.hz > 0;
}

uint64_t ThreadCpu100ns(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
  auto to64 = [](const FILETIME& t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return to64(kernel) + to64(user);
}

std::vector<MapSymbol> ReadMap(const std::string& path, uint64_t& preferred_base) {
  std::vector<MapSymbol> symbols;
  std::ifstream input(path);
  std::string line;
  preferred_base = 0;
  while (std::getline(input, line)) {
    const std::string marker = "Preferred load address is ";
    size_t at = line.find(marker);
    if (at != std::string::npos) {
      preferred_base = strtoull(line.c_str() + at + marker.size(), nullptr, 16);
      continue;
    }
    // " 0001:000d2290       ?Name@... 00000001800d3290     object.obj"
    std::istringstream row(line);
    std::string section, name, va_text;
    if (!(row >> section >> name >> va_text)) continue;
    if (section.size() != 13 || section[4] != ':' || section.rfind("0001", 0) != 0) continue;
    if (va_text.size() != 16) continue;
    char* end = nullptr;
    uint64_t va = strtoull(va_text.c_str(), &end, 16);
    if (!end || *end || !preferred_base || va < preferred_base) continue;
    symbols.push_back({va - preferred_base, name});
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const MapSymbol& a, const MapSymbol& b) { return a.rva < b.rva; });
  return symbols;
}

std::string Undecorate(const std::string& name) {
  if (name.empty() || name[0] != '?') return name;
  char buffer[1024];
  if (UnDecorateSymbolName(name.c_str(), buffer, sizeof(buffer),
                           UNDNAME_NAME_ONLY)) {
    return buffer;
  }
  return name;
}

// Snapshot served to dbghelp while unwinding a copied stack.
struct Snapshot {
  uint64_t base = 0;
  std::vector<uint8_t> bytes;
  size_t valid = 0;
} g_snapshot;
HANDLE g_process = nullptr;

BOOL CALLBACK ReadSnapshotMemory(HANDLE process, DWORD64 address, PVOID buffer,
                                 DWORD size, LPDWORD read) {
  if (address >= g_snapshot.base &&
      address + size <= g_snapshot.base + g_snapshot.valid) {
    memcpy(buffer, g_snapshot.bytes.data() + (address - g_snapshot.base), size);
    if (read) *read = size;
    return TRUE;
  }
  SIZE_T done = 0;
  BOOL ok = ReadProcessMemory(process, LPCVOID(address), buffer, size, &done);
  if (read) *read = DWORD(done);
  return ok && done == size;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, options)) {
    fprintf(stderr,
            "usage: thread_stack_sampler --pid N --seconds S [--hz 1000] "
            "[--thread-name NAME]... [--top-cpu N] [--map mod=path]... --out PREFIX\n");
    return 2;
  }
  g_process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                          options.pid);
  if (!g_process) {
    fprintf(stderr, "OpenProcess failed %lu\n", GetLastError());
    return 1;
  }
  auto nt_query_thread = reinterpret_cast<NtQueryInformationThreadFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));

  if (!options.wait_thread.empty() && options.wait_seconds) {
    // Poll once per second until the named thread has been at least
    // wait_percent busy for wait_seconds consecutive seconds.
    uint32_t busy_run = 0;
    for (uint32_t elapsed_s = 0;; ++elapsed_s) {
      if (elapsed_s >= options.wait_timeout_seconds) {
        fprintf(stderr, "trigger timeout\n");
        return 3;
      }
      DWORD exit_code = 0;
      if (!GetExitCodeProcess(g_process, &exit_code) || exit_code != STILL_ACTIVE) {
        fprintf(stderr, "target exited while waiting\n");
        return 4;
      }
      HANDLE watched = nullptr;
      HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te{sizeof(te)};
      for (BOOL ok = Thread32First(threads, &te); ok && !watched;
           ok = Thread32Next(threads, &te)) {
        if (te.th32OwnerProcessID != options.pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &description)) && description) {
          if (options.wait_thread == description) watched = h;
          LocalFree(description);
        }
        if (watched != h) CloseHandle(h);
      }
      CloseHandle(threads);
      if (!watched) {
        busy_run = 0;
        Sleep(1000);
        continue;
      }
      const uint64_t cpu0 = ThreadCpu100ns(watched);
      Sleep(1000);
      const uint64_t cpu1 = ThreadCpu100ns(watched);
      CloseHandle(watched);
      const double percent = double(cpu1 - cpu0) / 1e5;
      busy_run = percent >= options.wait_percent ? busy_run + 1 : 0;
      if (busy_run >= options.wait_seconds) {
        fprintf(stdout, "trigger: %s busy %.1f%% for %u s after %u s\n",
                Narrow(options.wait_thread).c_str(), percent, busy_run, elapsed_s + 1);
        fflush(stdout);
        break;
      }
    }
    if (options.delay_seconds) Sleep(options.delay_seconds * 1000);
  }
  if (options.wait_key_vk) {
    bool was_down = (GetAsyncKeyState(int(options.wait_key_vk)) & 0x8000) != 0;
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(options.wait_timeout_seconds) * 1000;
    for (;;) {
      if (GetTickCount64() >= deadline) {
        fprintf(stderr, "key trigger timeout\n");
        return 3;
      }
      DWORD exit_code = 0;
      if (!GetExitCodeProcess(g_process, &exit_code) || exit_code != STILL_ACTIVE) {
        fprintf(stderr, "target exited while waiting for key\n");
        return 4;
      }
      const bool down = (GetAsyncKeyState(int(options.wait_key_vk)) & 0x8000) != 0;
      if (down && !was_down) break;
      was_down = down;
      Sleep(20);
    }
    fprintf(stdout, "trigger: key 0x%X pressed\n", options.wait_key_vk);
    fflush(stdout);
    if (options.delay_seconds) Sleep(options.delay_seconds * 1000);
  }

  // Enumerate threads with their descriptions and CPU time.
  struct Candidate {
    DWORD tid;
    HANDLE handle;
    std::wstring name;
    uint64_t cpu0;
    uint64_t cpu1;
  };
  std::vector<Candidate> candidates;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != options.pid) continue;
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (!handle) continue;
    PWSTR description = nullptr;
    std::wstring name;
    if (SUCCEEDED(GetThreadDescription(handle, &description)) && description) {
      name = description;
      LocalFree(description);
    }
    candidates.push_back({entry.th32ThreadID, handle, name, ThreadCpu100ns(handle), 0});
  }
  CloseHandle(snapshot);
  Sleep(1000);
  for (Candidate& c : candidates) c.cpu1 = ThreadCpu100ns(c.handle);

  std::vector<ThreadTarget> targets;
  std::unordered_set<DWORD> chosen;
  for (DWORD wanted : options.tids) {
    for (const Candidate& c : candidates) {
      if (c.tid == wanted && chosen.insert(c.tid).second) {
        targets.push_back({c.tid, c.handle, c.name});
      }
    }
  }
  for (const std::wstring& wanted : options.thread_names) {
    for (const Candidate& c : candidates) {
      if (c.name == wanted && !chosen.count(c.tid)) {
        chosen.insert(c.tid);
        targets.push_back({c.tid, c.handle, c.name});
      }
    }
  }
  if (options.top_cpu) {
    std::vector<const Candidate*> sorted;
    for (const Candidate& c : candidates) sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(), [](const Candidate* a, const Candidate* b) {
      return (a->cpu1 - a->cpu0) > (b->cpu1 - b->cpu0);
    });
    for (uint32_t i = 0; i < sorted.size() && i < options.top_cpu; ++i) {
      if (chosen.insert(sorted[i]->tid).second) {
        targets.push_back({sorted[i]->tid, sorted[i]->handle, sorted[i]->name});
      }
    }
  }
  if (targets.empty()) {
    fprintf(stderr, "no threads selected\n");
    return 1;
  }
  for (ThreadTarget& t : targets) {
    ThreadBasicInformation info{};
    if (nt_query_thread &&
        nt_query_thread(t.handle, 0, &info, sizeof(info), nullptr) >= 0) {
      NT_TIB tib{};
      SIZE_T done = 0;
      if (ReadProcessMemory(g_process, info.TebBaseAddress, &tib, sizeof(tib), &done)) {
        t.stack_base = uint64_t(tib.StackBase);
      }
    }
    t.cpu_start_100ns = ThreadCpu100ns(t.handle);
  }

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                SYMOPT_NO_PROMPTS);
  SymInitialize(g_process, nullptr, TRUE);

  // Sampling.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  LARGE_INTEGER frequency, begin, now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&begin);
  const uint64_t tsc_begin = __rdtsc();
  const int64_t period_100ns = int64_t(10'000'000.0 / options.hz);
  const uint64_t end_tick = begin.QuadPart + uint64_t(options.seconds * frequency.QuadPart);
  g_snapshot.bytes.resize(options.stack_bytes);
  uint64_t rounds = 0;
  std::vector<uint64_t> frames;
  frames.reserve(options.max_frames);
  struct TimelineSample {
    int64_t qpc;
    DWORD tid;
    uint64_t cycles;
    uint32_t stack_id;
  };
  std::vector<TimelineSample> timeline;
  std::unordered_map<std::string, uint32_t> stack_ids;
  std::vector<const std::string*> stack_by_id;
  if (options.timeline) {
    timeline.reserve(size_t(options.seconds * options.hz * targets.size()) + 1024);
  }
  for (;;) {
    QueryPerformanceCounter(&now);
    if (uint64_t(now.QuadPart) >= end_tick) break;
    ++rounds;
    for (ThreadTarget& t : targets) {
      LARGE_INTEGER s0, s1;
      QueryPerformanceCounter(&s0);
      if (SuspendThread(t.handle) == DWORD(-1)) {
        ++t.failed;
        continue;
      }
      CONTEXT context{};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      // SuspendThread is asynchronous; GetThreadContext returns only once the
      // thread has stopped, so its cycle count is final only after it.
      bool ok = GetThreadContext(t.handle, &context) != 0;
      ULONG64 thread_cycles = 0;
      if (options.timeline) QueryThreadCycleTime(t.handle, &thread_cycles);
      size_t copy = 0;
      if (ok) {
        uint64_t limit = t.stack_base > context.Rsp ? t.stack_base - context.Rsp
                                                     : options.stack_bytes;
        copy = size_t(std::min<uint64_t>(limit, options.stack_bytes));
        SIZE_T done = 0;
        if (!ReadProcessMemory(g_process, LPCVOID(context.Rsp), g_snapshot.bytes.data(),
                               copy, &done)) {
          copy = done;
        }
      }
      ResumeThread(t.handle);
      QueryPerformanceCounter(&s1);
      const uint64_t suspended = uint64_t(s1.QuadPart - s0.QuadPart);
      t.suspended_ticks += suspended;
      t.max_suspended_ticks = std::max(t.max_suspended_ticks, suspended);
      if (!ok) {
        ++t.failed;
        continue;
      }
      g_snapshot.base = context.Rsp;
      g_snapshot.valid = copy;
      STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
      frames.clear();
      while (frames.size() < options.max_frames &&
             StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, t.handle, &frame,
                         &context, ReadSnapshotMemory, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)) {
        if (!frame.AddrPC.Offset) break;
        frames.push_back(frame.AddrPC.Offset);
      }
      if (frames.empty()) frames.push_back(context.Rip);
      std::string key(reinterpret_cast<const char*>(frames.data()),
                      frames.size() * sizeof(uint64_t));
      ++t.stacks[key];
      ++t.samples;
      if (options.timeline) {
        auto id = stack_ids.try_emplace(key, uint32_t(stack_ids.size()));
        if (id.second) stack_by_id.push_back(&id.first->first);
        timeline.push_back({s0.QuadPart, t.tid, thread_cycles, id.first->second});
      }
    }
    LARGE_INTEGER due;
    due.QuadPart = -period_100ns;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
  }
  QueryPerformanceCounter(&now);
  const uint64_t tsc_end = __rdtsc();
  const double elapsed = double(now.QuadPart - begin.QuadPart) / double(frequency.QuadPart);
  // Thread cycle counts advance at the invariant TSC rate.
  const double tsc_hz = elapsed > 0 ? double(tsc_end - tsc_begin) / elapsed : 0.0;
  for (ThreadTarget& t : targets) t.cpu_end_100ns = ThreadCpu100ns(t.handle);

  // Modules and maps.
  std::vector<Module> modules;
  HMODULE handles[4096];
  DWORD needed = 0;
  if (EnumProcessModulesEx(g_process, handles, sizeof(handles), &needed, LIST_MODULES_ALL)) {
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
      MODULEINFO info{};
      wchar_t path[MAX_PATH];
      if (!GetModuleInformation(g_process, handles[i], &info, sizeof(info))) continue;
      GetModuleFileNameExW(g_process, handles[i], path, MAX_PATH);
      Module module;
      module.base = uint64_t(info.lpBaseOfDll);
      module.size = info.SizeOfImage;
      module.path = Narrow(path);
      size_t slash = module.path.find_last_of("\\/");
      module.name = Lower(slash == std::string::npos ? module.path : module.path.substr(slash + 1));
      for (const auto& map : options.maps) {
        if (map.first == module.name) {
          uint64_t preferred = 0;
          module.symbols = ReadMap(map.second, preferred);
        }
      }
      modules.push_back(std::move(module));
    }
  }
  std::sort(modules.begin(), modules.end(),
            [](const Module& a, const Module& b) { return a.base < b.base; });
  std::unordered_map<uint64_t, std::string> names;
  auto symbolize = [&](uint64_t address) -> const std::string& {
    auto found = names.find(address);
    if (found != names.end()) return found->second;
    std::string result;
    auto it = std::upper_bound(modules.begin(), modules.end(), address,
                               [](uint64_t a, const Module& m) { return a < m.base; });
    if (it != modules.begin()) {
      const Module& module = *(it - 1);
      if (address < module.base + module.size) {
        const uint64_t rva = address - module.base;
        if (!module.symbols.empty()) {
          auto s = std::upper_bound(module.symbols.begin(), module.symbols.end(), rva,
                                    [](uint64_t r, const MapSymbol& m) { return r < m.rva; });
          if (s != module.symbols.begin()) {
            result = module.name + "!" + Undecorate((s - 1)->name);
          }
        }
        if (result.empty()) {
          alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
          auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
          symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
          symbol->MaxNameLen = 511;
          DWORD64 displacement = 0;
          if (SymFromAddr(g_process, address, &displacement, symbol) &&
              displacement < 0x2000) {
            result = module.name + "!" + symbol->Name;
          } else {
            result = module.name;
          }
        }
      }
    }
    if (result.empty()) {
      char text[32];
      snprintf(text, sizeof(text), "0x%llx", (unsigned long long)address);
      result = text;
    }
    return names.emplace(address, std::move(result)).first->second;
  };

  if (!options.leaf_detail.empty()) {
    // Leaf address histogram for the selected function(s), all threads.
    std::map<uint64_t, uint64_t> leaf_counts;
    for (ThreadTarget& t : targets) {
      for (const auto& [key, count] : t.stacks) {
        const uint64_t leaf = *reinterpret_cast<const uint64_t*>(key.data());
        if (symbolize(leaf).find(options.leaf_detail) != std::string::npos) {
          leaf_counts[leaf] += count;
        }
      }
    }
    std::ofstream leaf_report(options.out + ".leaf.txt");
    for (const auto& [address, count] : leaf_counts) {
      auto it = std::upper_bound(modules.begin(), modules.end(), address,
                                 [](uint64_t a, const Module& m) { return a < m.base; });
      const Module& module = *(it - 1);
      char row[256];
      snprintf(row, sizeof(row), "%s+0x%llx %llu %s\n", module.name.c_str(),
               (unsigned long long)(address - module.base), (unsigned long long)count,
               symbolize(address).c_str());
      leaf_report << row;
    }
  }

  if (options.timeline) {
    std::ofstream timeline_out(options.out + ".timeline.csv");
    timeline_out << "qpc,tid,cycles,stack_id\n";
    char row[128];
    for (const TimelineSample& sample : timeline) {
      snprintf(row, sizeof(row), "%lld,%lu,%llu,%u\n", (long long)sample.qpc, sample.tid,
               (unsigned long long)sample.cycles, sample.stack_id);
      timeline_out << row;
    }
    std::ofstream ids_out(options.out + ".stack_ids.txt");
    for (size_t id = 0; id < stack_by_id.size(); ++id) {
      const std::string& key = *stack_by_id[id];
      const uint64_t* addresses = reinterpret_cast<const uint64_t*>(key.data());
      const size_t n = key.size() / sizeof(uint64_t);
      ids_out << id << " ";
      for (size_t i = n; i-- > 0;) {
        ids_out << symbolize(i ? addresses[i] - 1 : addresses[i]) << (i ? ";" : "");
      }
      ids_out << "\n";
    }
  }

  std::ofstream report(options.out + ".txt");
  std::ofstream collapsed(options.out + ".collapsed.txt");
  char line[512];
  snprintf(line, sizeof(line),
           "pid=%lu elapsed_s=%.3f rounds=%llu hz_target=%.0f qpc_begin=%lld qpc_end=%lld "
           "qpc_frequency=%lld tsc_hz=%.0f\n",
           options.pid, elapsed, (unsigned long long)rounds, options.hz,
           (long long)begin.QuadPart, (long long)now.QuadPart, (long long)frequency.QuadPart,
           tsc_hz);
  report << line;
  for (ThreadTarget& t : targets) {
    std::unordered_map<std::string, uint64_t> exclusive;
    std::unordered_map<std::string, uint64_t> inclusive;
    for (const auto& [key, count] : t.stacks) {
      const uint64_t* addresses = reinterpret_cast<const uint64_t*>(key.data());
      const size_t n = key.size() / sizeof(uint64_t);
      std::vector<std::string> path;
      path.reserve(n);
      std::unordered_set<std::string> seen;
      for (size_t i = 0; i < n; ++i) {
        // Return addresses point after the call; attribute to the call site.
        const std::string& name = symbolize(i ? addresses[i] - 1 : addresses[i]);
        path.push_back(name);
        if (seen.insert(name).second) inclusive[name] += count;
      }
      exclusive[path.front()] += count;
      collapsed << Narrow(t.name.empty() ? L"thread" : t.name) << "_" << t.tid;
      for (size_t i = n; i-- > 0;) collapsed << ";" << path[i];
      collapsed << " " << count << "\n";
    }
    const double cpu_s = double(t.cpu_end_100ns - t.cpu_start_100ns) / 1e7;
    snprintf(line, sizeof(line),
             "\n== thread %lu \"%s\" samples=%llu failed=%llu cpu_s=%.3f (%.1f%% of elapsed) "
             "mean_suspend_us=%.1f max_suspend_us=%.1f unique_stacks=%zu\n",
             t.tid, Narrow(t.name).c_str(), (unsigned long long)t.samples,
             (unsigned long long)t.failed, cpu_s, 100.0 * cpu_s / elapsed,
             t.samples ? 1e6 * double(t.suspended_ticks) / double(t.samples) /
                             double(frequency.QuadPart)
                       : 0.0,
             1e6 * double(t.max_suspended_ticks) / double(frequency.QuadPart),
             t.stacks.size());
    report << line;
    auto dump = [&](const char* title, std::unordered_map<std::string, uint64_t>& table,
                    size_t limit) {
      std::vector<std::pair<std::string, uint64_t>> rows(table.begin(), table.end());
      std::sort(rows.begin(), rows.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      report << "-- " << title << "\n";
      for (size_t i = 0; i < rows.size() && i < limit; ++i) {
        snprintf(line, sizeof(line), "%7.2f%% %8llu  %s\n",
                 100.0 * double(rows[i].second) / double(std::max<uint64_t>(t.samples, 1)),
                 (unsigned long long)rows[i].second, rows[i].first.c_str());
        report << line;
      }
    };
    dump("exclusive (leaf)", exclusive, 80);
    dump("inclusive", inclusive, 120);
  }
  SymCleanup(g_process);
  fprintf(stdout, "wrote %s.txt and %s.collapsed.txt (elapsed %.2fs, rounds %llu)\n",
          options.out.c_str(), options.out.c_str(), elapsed, (unsigned long long)rounds);
  return 0;
}
