#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct PpcContextPrefix {
  std::uint64_t thread_state;
  std::uint64_t virtual_membase;
  std::uint64_t lr;
  std::uint64_t ctr;
  std::uint64_t r[32];
};

std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  unsigned backslashes = 0;
  for (wchar_t ch : value) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(ch);
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(ch);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

bool ReadRemote(HANDLE process, std::uint64_t address, void* output,
                std::size_t size) {
  SIZE_T read = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                           output, size, &read) &&
         read == size;
}

std::uint32_t ByteSwap32(std::uint32_t value) {
  return _byteswap_ulong(value);
}

bool ReadGuestU32(HANDLE process, const PpcContextPrefix& ppc,
                  std::uint32_t address, std::uint32_t& value) {
  std::uint32_t raw = 0;
  if (!ReadRemote(process, ppc.virtual_membase + address, &raw, sizeof(raw))) {
    return false;
  }
  value = ByteSwap32(raw);
  return true;
}

std::string ReadGuestNarrowStringObject(HANDLE process,
                                        const PpcContextPrefix& ppc,
                                        std::uint32_t object) {
  std::uint32_t storage = 0;
  if (!object || !ReadGuestU32(process, ppc, object + 4, storage) ||
      !storage) {
    return {};
  }

  std::string value;
  value.reserve(128);
  for (std::uint32_t offset = 0; offset < 512; ++offset) {
    std::uint8_t character = 0;
    if (!ReadRemote(process, ppc.virtual_membase + storage + 2 + offset,
                    &character, sizeof(character)) ||
        !character) {
      break;
    }
    if (character < 0x20 || character > 0x7E) {
      return {};
    }
    value.push_back(static_cast<char>(character));
  }
  return value;
}

std::string Hex(std::uint64_t value, unsigned width = 8) {
  std::ostringstream stream;
  stream << "0x" << std::uppercase << std::hex << std::setfill('0')
         << std::setw(width) << value;
  return stream.str();
}

void DumpGuestWords(std::ofstream& log, HANDLE process,
                    const PpcContextPrefix& ppc, std::uint32_t address,
                    std::size_t word_count, const char* label) {
  log << label << "=" << Hex(address);
  for (std::size_t i = 0; i < word_count; ++i) {
    std::uint32_t value = 0;
    if (!ReadGuestU32(process, ppc,
                      address + static_cast<std::uint32_t>(i * 4), value)) {
      log << " unreadable@+" << Hex(i * 4, 2);
      break;
    }
    log << " +" << Hex(i * 4, 2) << "=" << Hex(value);
  }
  log << '\n';
}

void DumpTargetState(std::ofstream& log, HANDLE process,
                     const PpcContextPrefix& ppc, std::uint64_t host_rip,
                     unsigned hit) {
  const auto guest_r3 = static_cast<std::uint32_t>(ppc.r[3]);
  const auto guest_r4 = static_cast<std::uint32_t>(ppc.r[4]);
  const auto guest_r5 = static_cast<std::uint32_t>(ppc.r[5]);
  log << "hit=" << hit << " host_rip=" << Hex(host_rip, 16)
      << " context=" << Hex(reinterpret_cast<std::uint64_t>(&ppc), 16)
      << " membase=" << Hex(ppc.virtual_membase, 16)
      << " lr=" << Hex(static_cast<std::uint32_t>(ppc.lr))
      << " r1=" << Hex(static_cast<std::uint32_t>(ppc.r[1]))
      << " r3=" << Hex(guest_r3) << " r4=" << Hex(guest_r4)
      << " r5=" << Hex(guest_r5)
      << " r12=" << Hex(static_cast<std::uint32_t>(ppc.r[12]))
      << " r20=" << Hex(static_cast<std::uint32_t>(ppc.r[20]))
      << " r24=" << Hex(static_cast<std::uint32_t>(ppc.r[24]))
      << " r25=" << Hex(static_cast<std::uint32_t>(ppc.r[25]))
      << " r27=" << Hex(static_cast<std::uint32_t>(ppc.r[27]))
      << " r29=" << Hex(static_cast<std::uint32_t>(ppc.r[29]))
      << " r30=" << Hex(static_cast<std::uint32_t>(ppc.r[30]))
      << " r31=" << Hex(static_cast<std::uint32_t>(ppc.r[31])) << '\n';

  if (guest_r3) {
    DumpGuestWords(log, process, ppc, guest_r3, 4, "r3_object");
    std::uint32_t control = 0;
    if (ReadGuestU32(process, ppc, guest_r3 + 4, control) && control) {
      DumpGuestWords(log, process, ppc, control, 7, "shared_control");
      std::uint32_t backing = 0;
      if (ReadGuestU32(process, ppc, control + 24, backing) && backing) {
        DumpGuestWords(log, process, ppc, backing, 8, "backing_prefix");
      }
    }
  }
  const auto guest_r1 = static_cast<std::uint32_t>(ppc.r[1]);
  if (guest_r1) {
    DumpGuestWords(log, process, ppc, guest_r1 + 80, 20, "stack_plus_80");
    log << "stack_string_88=\""
        << ReadGuestNarrowStringObject(process, ppc, guest_r1 + 88)
        << "\"\n";
  }
  log << "r29_string=\""
      << ReadGuestNarrowStringObject(
             process, ppc, static_cast<std::uint32_t>(ppc.r[29]))
      << "\"\n";
  log.flush();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 6) {
    std::wcerr << L"usage: xenia_guest_probe <xenia.exe> <default.xex> "
                  L"<guest-break-address> <probe.log> <max-hits> [seconds] "
                  L"[guest-lr-filter] [guest-gpr-filter] [guest-gpr-value]\n";
    return 2;
  }

  const std::filesystem::path xenia_path =
      std::filesystem::absolute(std::filesystem::path(argv[1]));
  const std::filesystem::path xex_path =
      std::filesystem::absolute(std::filesystem::path(argv[2]));
  const std::uint64_t guest_break = std::wcstoull(argv[3], nullptr, 0);
  const std::filesystem::path probe_path =
      std::filesystem::absolute(std::filesystem::path(argv[4]));
  const unsigned max_hits = static_cast<unsigned>(std::wcstoul(argv[5], nullptr, 0));
  const unsigned timeout_seconds =
      argc >= 7 ? static_cast<unsigned>(std::wcstoul(argv[6], nullptr, 0)) : 300;
  const std::uint32_t guest_lr_filter =
      argc >= 8 ? static_cast<std::uint32_t>(std::wcstoull(argv[7], nullptr, 0))
                : 0;
  const int guest_gpr_filter = argc >= 9 ? std::wcstol(argv[8], nullptr, 0) : -1;
  const std::uint64_t guest_gpr_value =
      argc >= 10 ? std::wcstoull(argv[9], nullptr, 0) : 0;
  const std::filesystem::path xenia_log_path =
      probe_path.parent_path() / (probe_path.stem().wstring() + L"_xenia.log");

  std::ofstream log(probe_path, std::ios::out | std::ios::trunc);
  if (!log) {
    std::wcerr << L"unable to open probe log " << probe_path << L"\n";
    return 3;
  }

  std::wstring command_line =
      Quote(xenia_path.wstring()) + L" " + Quote(xex_path.wstring()) +
      L" --break_on_instruction=" + std::to_wstring(guest_break) +
      L" --break_on_debugbreak=false --log_to_stdout=false --log_file=" +
      Quote(xenia_log_path.wstring());
  if (guest_gpr_filter >= 0 && guest_gpr_filter < 32) {
    command_line += L" --break_condition_gpr=" +
                    std::to_wstring(guest_gpr_filter) +
                    L" --break_condition_value=" +
                    std::to_wstring(guest_gpr_value) +
                    L" --break_condition_op=eq --break_condition_truncate=true";
  }
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process_info{};
  const std::wstring working_directory = xenia_path.parent_path().wstring();
  if (!CreateProcessW(xenia_path.c_str(), mutable_command.data(), nullptr,
                      nullptr, FALSE,
                      DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_PROCESS_GROUP,
                      nullptr, working_directory.c_str(), &startup,
                      &process_info)) {
    log << "CreateProcessW failed error=" << GetLastError() << '\n';
    return 4;
  }

  log << "pid=" << process_info.dwProcessId
      << " guest_break=" << Hex(guest_break)
      << " max_hits=" << max_hits << " timeout_seconds=" << timeout_seconds
      << " guest_lr_filter=" << Hex(guest_lr_filter)
      << " guest_gpr_filter=" << guest_gpr_filter
      << " guest_gpr_value=" << Hex(guest_gpr_value, 16)
      << " xenia_log=" << xenia_log_path.string() << '\n';
  log.flush();

  const auto started = std::chrono::steady_clock::now();
  unsigned hits = 0;
  bool running = true;
  while (running) {
    DEBUG_EVENT event{};
    if (!WaitForDebugEvent(&event, 1000)) {
      if (GetLastError() != ERROR_SEM_TIMEOUT) {
        log << "WaitForDebugEvent failed error=" << GetLastError() << '\n';
        break;
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      if (static_cast<unsigned>(elapsed) >= timeout_seconds) {
        log << "timeout elapsed_seconds=" << elapsed << '\n';
        TerminateProcess(process_info.hProcess, 0xDEAD);
      }
      continue;
    }

    DWORD continue_status = DBG_CONTINUE;
    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const auto& exception = event.u.Exception.ExceptionRecord;
      if (exception.ExceptionCode == EXCEPTION_BREAKPOINT) {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, event.dwThreadId);
        CONTEXT host{};
        host.ContextFlags = CONTEXT_FULL;
        PpcContextPrefix ppc{};
        if (thread && GetThreadContext(thread, &host) && host.Rsi &&
            ReadRemote(process_info.hProcess, host.Rsi, &ppc, sizeof(ppc)) &&
            (static_cast<std::uint32_t>(ppc.lr) & 0xFF000000u) == 0x82000000u &&
            ppc.virtual_membase &&
            (!guest_lr_filter ||
             static_cast<std::uint32_t>(ppc.lr) == guest_lr_filter) &&
            (guest_gpr_filter < 0 || guest_gpr_filter >= 32 ||
             ppc.r[guest_gpr_filter] == guest_gpr_value)) {
          ++hits;
          log << "ppc_context_remote=" << Hex(host.Rsi, 16) << ' ';
          DumpTargetState(log, process_info.hProcess, ppc, host.Rip, hits);
          if (hits >= max_hits) {
            log << "hit_limit_reached\n";
            TerminateProcess(process_info.hProcess, 0xBEEF);
          }
        } else {
          log << "host_break thread=" << event.dwThreadId
              << " address="
              << Hex(reinterpret_cast<std::uint64_t>(exception.ExceptionAddress),
                     16)
              << '\n';
          log.flush();
        }
        if (thread) {
          CloseHandle(thread);
        }
      } else {
        continue_status = DBG_EXCEPTION_NOT_HANDLED;
      }
    } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      log << "exit_process code=" << event.u.ExitProcess.dwExitCode
          << " hits=" << hits << '\n';
      running = false;
    }

    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continue_status);
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return hits ? 0 : 5;
}
