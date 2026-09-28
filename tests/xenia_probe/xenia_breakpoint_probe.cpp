#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename T>
bool ReadRemote(HANDLE process, uint64_t address, T& value) {
  SIZE_T bytes_read = 0;
  return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                           &value, sizeof(value), &bytes_read) &&
         bytes_read == sizeof(value);
}

bool ReadGuestU32(HANDLE process, uint64_t membase, uint32_t address,
                  uint32_t& value) {
  uint32_t raw = 0;
  if (!ReadRemote(process, membase + address, raw)) return false;
  value = _byteswap_ulong(raw);
  return true;
}

bool ReadGuestU64(HANDLE process, uint64_t membase, uint32_t address,
                  uint64_t& value) {
  uint64_t raw = 0;
  if (!ReadRemote(process, membase + address, raw)) return false;
  value = _byteswap_uint64(raw);
  return true;
}

std::string ReadGuestStarbreezeString(HANDLE process, uint64_t membase,
                                      uint32_t object) {
  uint32_t storage = 0;
  if (!object || !ReadGuestU32(process, membase, object + 4u, storage) ||
      !storage) {
    return {};
  }
  std::string value;
  value.reserve(128);
  for (uint32_t offset = 0; offset < 512u; ++offset) {
    uint8_t character = 0;
    if (!ReadRemote(process, membase + storage + 2u + offset, character)) {
      return {};
    }
    if (!character) break;
    if (character < 0x20u || character > 0x7Eu) return {};
    value.push_back(static_cast<char>(character));
  }
  return value;
}

std::string ReadGuestCString(HANDLE process, uint64_t membase,
                             uint32_t address) {
  if (!address) return {};
  std::string value;
  value.reserve(128);
  for (uint32_t offset = 0; offset < 512u; ++offset) {
    uint8_t character = 0;
    if (!ReadRemote(process, membase + address + offset, character)) return {};
    if (!character) break;
    if (character < 0x20u || character > 0x7Eu) return {};
    value.push_back(static_cast<char>(character));
  }
  return value;
}

std::wstring Quote(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 6) {
    std::wcerr << L"usage: xenia_breakpoint_probe <xenia.exe> <default.xex> "
                  L"<output.log> <guest-breakpoint> <timeout-seconds>\n";
    return 2;
  }

  const std::filesystem::path xenia_path = argv[1];
  const std::filesystem::path xex_path = argv[2];
  const std::filesystem::path output_path = argv[3];
  const std::wstring breakpoint = argv[4];
  const uint32_t guest_breakpoint =
      static_cast<uint32_t>(std::stoul(breakpoint, nullptr, 0));
  const bool generic_register_mode = guest_breakpoint != 0x8220E070u;
  const bool title_frame_mode = guest_breakpoint == 0x827A62E0u;
  const bool allocator_snapshot_mode =
      guest_breakpoint == 0x82215110u ||
      guest_breakpoint == 0x821F2934u ||
      guest_breakpoint == 0x822159E8u ||
      guest_breakpoint == 0x82216068u;
  const uint32_t timeout_seconds = std::stoul(argv[5]);
  std::filesystem::create_directories(output_path.parent_path());
  std::ofstream out(output_path, std::ios::trunc);
  if (!out) {
    std::wcerr << L"unable to open output log: " << output_path << L"\n";
    return 3;
  }

  std::wstring command_line = Quote(xenia_path.wstring()) + L" " +
                              Quote(xex_path.wstring()) +
                              L" --break_on_instruction=" + breakpoint +
                              L" --headless=true";
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring working_directory = xenia_path.parent_path().wstring();
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_PROCESS_GROUP,
                      nullptr, working_directory.c_str(), &startup, &process)) {
    out << "CREATE_PROCESS_FAILED error=" << GetLastError() << '\n';
    return 4;
  }

  out << "XENIA_PROBE pid=" << process.dwProcessId << " breakpoint="
      << std::string(breakpoint.begin(), breakpoint.end())
      << " timeout_seconds=" << timeout_seconds << '\n';
  out.flush();
  CloseHandle(process.hThread);

  const auto started = std::chrono::steady_clock::now();
  uint32_t breakpoint_events = 0;
  uint32_t captures = 0;
  bool exited = false;

  while (!exited) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started);
    if (elapsed.count() >= timeout_seconds) {
      out << "TIMEOUT captures=" << captures << '\n';
      out.flush();
      TerminateProcess(process.hProcess, 0x102u);
    }

    DEBUG_EVENT event{};
    if (!WaitForDebugEvent(&event, 1000)) {
      if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
      out << "WAIT_FAILED error=" << GetLastError() << '\n';
      break;
    }

    DWORD continue_status = DBG_CONTINUE;
    if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const auto& exception = event.u.Exception.ExceptionRecord;
      if (exception.ExceptionCode == EXCEPTION_BREAKPOINT) {
        ++breakpoint_events;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, event.dwThreadId);
        CONTEXT host_context{};
        host_context.ContextFlags = CONTEXT_FULL;
        bool captured = false;
        if (thread && GetThreadContext(thread, &host_context)) {
          uint64_t membase = 0;
          uint64_t guest_lr = 0;
          uint64_t guest_r3 = 0;
          uint64_t guest_r4 = 0;
          uint64_t guest_r5 = 0;
          uint64_t guest_r6 = 0;
          if (host_context.Rsi > 0x10000u &&
              ReadRemote(process.hProcess, host_context.Rsi + 0x08u, membase) &&
              ReadRemote(process.hProcess, host_context.Rsi + 0x10u, guest_lr) &&
              ReadRemote(process.hProcess, host_context.Rsi + 0x38u, guest_r3) &&
              ReadRemote(process.hProcess, host_context.Rsi + 0x40u, guest_r4) &&
              ReadRemote(process.hProcess, host_context.Rsi + 0x48u, guest_r5) &&
               ReadRemote(process.hProcess, host_context.Rsi + 0x50u, guest_r6) &&
               membase > 0x10000u && guest_r3 <= UINT32_MAX) {
            if (generic_register_mode) {
              uint64_t guest_r18 = 0;
              uint64_t guest_r20 = 0;
              uint64_t guest_r21 = 0;
              uint64_t guest_r23 = 0;
              uint64_t guest_r24 = 0;
              uint64_t guest_r25 = 0;
              uint64_t guest_r28 = 0;
              uint64_t guest_r30 = 0;
              ReadRemote(process.hProcess, host_context.Rsi + 0xB0u, guest_r18);
              ReadRemote(process.hProcess, host_context.Rsi + 0xC0u, guest_r20);
              ReadRemote(process.hProcess, host_context.Rsi + 0xC8u, guest_r21);
              ReadRemote(process.hProcess, host_context.Rsi + 0xD8u, guest_r23);
              ReadRemote(process.hProcess, host_context.Rsi + 0xE0u, guest_r24);
              ReadRemote(process.hProcess, host_context.Rsi + 0xE8u, guest_r25);
              ReadRemote(process.hProcess, host_context.Rsi + 0x100u, guest_r28);
              ReadRemote(process.hProcess, host_context.Rsi + 0x110u, guest_r30);

              const uint32_t wrapper = static_cast<uint32_t>(guest_r23);
              const uint32_t provider = static_cast<uint32_t>(guest_r25);
              uint32_t wrapper_stream = 0;
              uint32_t wrapper_type = 0;
              uint32_t provider_vtable = 0;
              if (wrapper) {
                ReadGuestU32(process.hProcess, membase, wrapper + 16u,
                             wrapper_stream);
                ReadGuestU32(process.hProcess, membase, wrapper + 20u,
                             wrapper_type);
              }
              if (provider) {
                ReadGuestU32(process.hProcess, membase, provider,
                             provider_vtable);
              }
              out << "CAPTURE sequence=" << captures++ << " host_thread="
                  << event.dwThreadId << " breakpoint=0x" << std::hex
                  << guest_breakpoint << " guest_lr=0x"
                  << static_cast<uint32_t>(guest_lr) << " r3=0x"
                  << static_cast<uint32_t>(guest_r3) << " r4=0x"
                  << static_cast<uint32_t>(guest_r4) << " r5=0x"
                  << static_cast<uint32_t>(guest_r5) << " r6=0x"
                  << static_cast<uint32_t>(guest_r6) << " r18=0x"
                  << static_cast<uint32_t>(guest_r18) << " r20=0x"
                  << static_cast<uint32_t>(guest_r20) << " r21=0x"
                  << static_cast<uint32_t>(guest_r21) << " r23=0x" << wrapper
                  << " r24=0x" << static_cast<uint32_t>(guest_r24)
                  << " r25=0x" << provider << " provider_vtable=0x"
                  << provider_vtable << " wrapper_stream=0x" << wrapper_stream
                  << " wrapper_type=0x" << wrapper_type << " path=\""
                  << ReadGuestStarbreezeString(
                         process.hProcess, membase,
                         static_cast<uint32_t>(guest_r18))
                  << "\" r30=0x" << static_cast<uint32_t>(guest_r30);
              if (guest_breakpoint == 0x8220C02Cu ||
                  guest_breakpoint == 0x821F3E50u) {
                const uint32_t lhs_object = guest_breakpoint == 0x821F3E50u
                                                ? static_cast<uint32_t>(guest_r3)
                                                : static_cast<uint32_t>(guest_r28);
                out << " lhs=\""
                    << ReadGuestStarbreezeString(
                           process.hProcess, membase, lhs_object)
                    << "\" rhs=\""
                    << ReadGuestCString(
                           process.hProcess, membase,
                           static_cast<uint32_t>(guest_r4))
                    << "\" r28=0x" << static_cast<uint32_t>(guest_r28);
              }
              if (title_frame_mode) {
                const uint32_t main = static_cast<uint32_t>(guest_r30);
                uint32_t root = 0;
                uint32_t producer = 0;
                uint32_t state2720 = 0;
                uint32_t state2724 = 0;
                uint32_t state2728 = 0;
                uint32_t state2732 = 0;
                uint32_t service2752 = 0;
                uint32_t service2764 = 0;
                uint32_t scene = 0;
                uint32_t scene_list = 0;
                uint32_t scene_count = 0;
                uint32_t update_list = 0;
                uint32_t update_count = 0;
                if (main) {
                  ReadGuestU32(process.hProcess, membase, main + 24u, root);
                  ReadGuestU32(process.hProcess, membase, main + 108u,
                               producer);
                  ReadGuestU32(process.hProcess, membase, main + 2720u,
                               state2720);
                  ReadGuestU32(process.hProcess, membase, main + 2724u,
                               state2724);
                  ReadGuestU32(process.hProcess, membase, main + 2728u,
                               state2728);
                  ReadGuestU32(process.hProcess, membase, main + 2732u,
                               state2732);
                  ReadGuestU32(process.hProcess, membase, main + 2752u,
                               service2752);
                  ReadGuestU32(process.hProcess, membase, main + 2764u,
                               service2764);
                  ReadGuestU32(process.hProcess, membase, main + 2788u, scene);
                }
                if (scene) {
                  ReadGuestU32(process.hProcess, membase, scene + 3668u,
                               scene_list);
                }
                if (scene_list) {
                  ReadGuestU32(process.hProcess, membase, scene_list + 4u,
                               scene_count);
                }
                if (root) {
                  ReadGuestU32(process.hProcess, membase, root + 156u,
                               update_list);
                }
                if (update_list) {
                  ReadGuestU32(process.hProcess, membase, update_list + 4u,
                               update_count);
                }
                out << " frame_main=0x" << main << " root=0x" << root
                    << " producer=0x" << producer << " state2720=0x"
                    << state2720 << " state2724=0x" << state2724
                    << " state2728=0x" << state2728 << " state2732=0x"
                    << state2732 << " service2752=0x" << service2752
                    << " service2764=0x" << service2764 << " scene2788=0x"
                    << scene << " scene_list3668=0x" << scene_list
                    << " scene_count=0x" << scene_count << " update_list=0x"
                    << update_list << " update_count=0x" << update_count;
              }
              if (allocator_snapshot_mode) {
                const uint32_t allocator =
                    guest_breakpoint == 0x82215110u ||
                            guest_breakpoint == 0x821F2934u
                        ? static_cast<uint32_t>(guest_r3)
                        : static_cast<uint32_t>(guest_r24);
                constexpr uint32_t kAllocatorOffsets[] = {
                    0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u,
                    0x1Cu, 0x20u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u,
                    0x38u, 0x3Cu, 0x40u, 0x44u, 0xB8u, 0xBCu, 0xC0u,
                    0xC4u, 0xC8u, 0xCCu, 0xD4u, 0xD8u, 0xE4u};
                out << " allocator=0x" << allocator;
                for (const uint32_t offset : kAllocatorOffsets) {
                  uint32_t value = 0;
                  const bool valid = allocator && ReadGuestU32(
                      process.hProcess, membase, allocator + offset, value);
                  out << " a" << std::setw(2) << std::setfill('0') << offset
                      << "=0x" << (valid ? value : 0u);
                }
                out << std::setfill(' ');
              }
              out << std::dec << '\n';
              out.flush();
              captured = true;
            } else {
            const uint32_t worker = static_cast<uint32_t>(guest_r3);
            uint32_t worker_vtable = 0;
            if (ReadGuestU32(process.hProcess, membase, worker,
                             worker_vtable) &&
                worker_vtable == 0x820659A0u) {
              uint32_t worker_flags = 0;
              uint32_t worker_last_chunk = 0;
              uint32_t worker_cursor = 0;
              uint64_t worker_base_position = 0;
              uint32_t worker_active_buffer = 0;
              uint32_t queue_d4 = 0;
              uint32_t queue_d8 = 0;
              uint32_t queue_dc = 0;
              uint32_t queue_e0 = 0;
              uint32_t root = 0;
              uint32_t manager = 0;
              uint32_t manager_thread = 0;
              uint32_t manager_worker = 0;
              ReadGuestU32(process.hProcess, membase, worker + 4u,
                           worker_flags);
              ReadGuestU32(process.hProcess, membase, worker + 76u,
                           worker_last_chunk);
              ReadGuestU32(process.hProcess, membase, worker + 88u,
                           worker_cursor);
              ReadGuestU64(process.hProcess, membase, worker + 304u,
                           worker_base_position);
              ReadGuestU32(process.hProcess, membase, worker + 312u,
                           worker_active_buffer);
              ReadGuestU32(process.hProcess, membase, worker + 0xD4u, queue_d4);
              ReadGuestU32(process.hProcess, membase, worker + 0xD8u, queue_d8);
              ReadGuestU32(process.hProcess, membase, worker + 0xDCu, queue_dc);
              ReadGuestU32(process.hProcess, membase, worker + 0xE0u, queue_e0);
              if (ReadGuestU32(process.hProcess, membase, 0x82A690F8u, root) &&
                  root) {
                ReadGuestU32(process.hProcess, membase, root + 12u, manager);
                if (manager) {
                  ReadGuestU32(process.hProcess, membase, manager + 156u,
                               manager_thread);
                  ReadGuestU32(process.hProcess, membase, manager + 164u,
                               manager_worker);
                }
              }
              out << "CAPTURE sequence=" << captures++ << " host_thread="
                  << event.dwThreadId << " guest_lr=0x" << std::hex
                  << static_cast<uint32_t>(guest_lr) << " worker=0x" << worker
                  << " requested_position=0x" << guest_r4 << " output=0x"
                  << static_cast<uint32_t>(guest_r5) << " bytes=0x"
                  << static_cast<uint32_t>(guest_r6) << " worker_flags=0x"
                  << worker_flags << " worker_last_chunk=0x"
                  << worker_last_chunk << " worker_cursor=0x" << worker_cursor
                  << " worker_base_position=0x" << worker_base_position
                  << " worker_active_buffer=0x" << worker_active_buffer
                  << " queue_d4=0x" << queue_d4 << " queue_d8=0x" << queue_d8
                  << " queue_dc=0x" << queue_dc << " queue_e0=0x" << queue_e0
                  << " root=0x" << root << " manager=0x" << manager
                  << " manager_thread=0x" << manager_thread
                  << " manager_worker=0x" << manager_worker << std::dec << '\n';
              out.flush();
              captured = true;
            }
            }
          }
        }
        if (thread) CloseHandle(thread);

        if (!captured && breakpoint_events > 1) {
          out << "UNMATCHED_BREAKPOINT sequence=" << breakpoint_events
              << " host_address=0x" << std::hex
              << reinterpret_cast<uint64_t>(exception.ExceptionAddress)
              << std::dec << '\n';
          out.flush();
          continue_status = DBG_EXCEPTION_NOT_HANDLED;
        }
      } else {
        continue_status = DBG_EXCEPTION_NOT_HANDLED;
      }
    } else if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
      if (event.u.CreateProcessInfo.hFile) {
        CloseHandle(event.u.CreateProcessInfo.hFile);
      }
    } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
      if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
    } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      out << "EXIT code=0x" << std::hex << event.u.ExitProcess.dwExitCode
          << std::dec << " captures=" << captures << '\n';
      out.flush();
      exited = true;
    }

    ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continue_status);
  }

  CloseHandle(process.hProcess);
  return captures ? 0 : 5;
}
