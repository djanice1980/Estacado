// Read-only local-player state sampler for one isolated game process.
//
// Follows the pointer chain verified by logs/capture_v267_movement.ps1: the
// client world object (vtable 0x820807E0) -> local entity table -> actor, and
// the prediction chain selected like 0x8211B688. Every sample re-validates the
// world vtable and the actor identity; nothing is ever written to the target.
// Guest memory is big-endian; host address = guest + (physical_base -
// 0xA0000000), with physical_base from the run's stdout startup line.
//
// actor_state_sampler --pid N --stdout PATH (--world HEX | --find-world)
//     [--interval-us 1000] [--seconds 10] [--stop-file PATH] --out FILE.csv
// --find-world scans committed guest pages in [0xA0000000, 0xC0000000) for
// the world vtable and keeps the first candidate whose local actor chain
// validates (the world allocation address differs between runs). With
// --find-world the scan is repeated (at most once per second) while samples
// stay invalid, so a long player session survives level transitions.
// Waits use a high-resolution waitable timer (no spinning); sampling ends at
// --seconds, when the game exits, or when the stop file exists. stdout gets
// qpc_start/qpc_frequency so rows align with the swap-interval observer ticks.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
  DWORD pid = 0;
  std::string stdout_path;
  uint32_t world = 0;
  bool find_world = false;
  uint32_t interval_us = 1000;
  double seconds = 10.0;
  std::string stop_file;
  std::string out;
};

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
float BEF(const uint8_t* p) {
  const uint32_t bits = BE32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
double BED(const uint8_t* p) {
  const uint64_t bits = (uint64_t(BE32(p)) << 32) | BE32(p + 4);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

class Guest {
 public:
  Guest(HANDLE process, uint64_t base) : process_(process), base_(base) {}
  bool Read(uint32_t address, void* out, size_t bytes) const {
    if (!address || uint64_t(address) + bytes > 0x100000000ull) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(base_ + address), out,
                             bytes, &got) && got == bytes;
  }

 private:
  HANDLE process_;
  uint64_t base_;
};

struct ActorView {
  uint32_t actor = 0;
  float position[3]{};
  float forward[3]{};
  uint32_t state_sequence = 0;
  float state_fraction = 0;
};

bool ReadActor(const Guest& guest, uint32_t actor, ActorView& view, uint8_t* raw /*0x260*/) {
  if (!guest.Read(actor, raw, 0x260)) return false;
  view.actor = actor;
  for (int i = 0; i < 3; ++i) {
    view.forward[i] = BEF(raw + 0x50 + 4 * i);
    view.position[i] = BEF(raw + 0x80 + 4 * i);
  }
  uint8_t state[0x38];
  if (guest.Read(BE32(raw + 0x18C), state, sizeof(state))) {
    view.state_sequence = BE32(state + 0x18);
    view.state_fraction = BEF(state + 0x28);
  }
  return true;
}

double Heading(const float* forward) {
  return std::atan2(double(forward[1]), double(forward[0])) * 180.0 / 3.14159265358979323846;
}
double Pitch(const float* forward) {
  double z = std::fmax(-1.0, std::fmin(1.0, double(forward[2])));
  return std::asin(z) * 180.0 / 3.14159265358979323846;
}

bool ParseOptions(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--pid" && (v = next())) o.pid = DWORD(strtoul(v, nullptr, 10));
    else if (a == "--stdout" && (v = next())) o.stdout_path = v;
    else if (a == "--world" && (v = next())) o.world = uint32_t(strtoul(v, nullptr, 16));
    else if (a == "--find-world") o.find_world = true;
    else if (a == "--interval-us" && (v = next())) o.interval_us = uint32_t(strtoul(v, nullptr, 10));
    else if (a == "--seconds" && (v = next())) o.seconds = atof(v);
    else if (a == "--stop-file" && (v = next())) o.stop_file = v;
    else if (a == "--out" && (v = next())) o.out = v;
    else return false;
  }
  return o.pid && !o.stdout_path.empty() && (o.world || o.find_world) && !o.out.empty() &&
         o.seconds > 0;
}

bool ValidateWorld(const Guest& guest, uint32_t world_address) {
  std::vector<uint8_t> world(0x22A8);
  if (!guest.Read(world_address, world.data(), world.size()) ||
      BE32(world.data()) != 0x820807E0u) {
    return false;
  }
  const uint8_t* w = world.data();
  const uint32_t id = BE32(w + 0x218);
  const uint32_t threshold = BE32(w + 0xF74);
  const uint32_t table = id >= threshold ? BE32(w + 0xF70) : BE32(w + 0xF64);
  const uint32_t index = id >= threshold ? id - threshold : id;
  uint8_t list[0x1C];
  uint8_t slot_bytes[4];
  std::vector<uint8_t> raw(0x260);
  ActorView actor;
  return guest.Read(table, list, sizeof(list)) && index < BE32(list + 4) &&
         guest.Read(BE32(list + 0x18) + index * 4, slot_bytes, 4) &&
         ReadActor(guest, BE32(slot_bytes), actor, raw.data()) &&
         BE16(raw.data() + 0x170) == id && BE16(raw.data() + 0x172) != 0;
}

uint32_t FindWorld(HANDLE process, const Guest& guest, uint64_t base) {
  const uint64_t begin = base + 0xA0000000ull;
  const uint64_t end = base + 0xC0000000ull;
  std::vector<uint8_t> chunk;
  for (uint64_t address = begin; address < end;) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &info, sizeof(info))) {
      break;
    }
    const uint64_t region_end =
        std::min<uint64_t>(end, uint64_t(info.BaseAddress) + info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      for (uint64_t at = address; at < region_end; at += 1 << 20) {
        const size_t bytes = size_t(std::min<uint64_t>(1 << 20, region_end - at));
        chunk.resize(bytes);
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(at), chunk.data(), bytes,
                               &got)) {
          continue;
        }
        for (size_t i = 0; i + 4 <= got; i += 4) {
          if (BE32(chunk.data() + i) == 0x820807E0u) {
            const uint32_t candidate = uint32_t(at + i - base);
            if (ValidateWorld(guest, candidate)) return candidate;
          }
        }
      }
    }
    address = region_end;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!ParseOptions(argc, argv, o)) {
    fprintf(stderr,
            "usage: actor_state_sampler --pid N --stdout PATH --world HEX "
            "[--interval-us 1000] [--seconds 10] --out FILE.csv\n");
    return 2;
  }
  std::ifstream log(o.stdout_path);
  std::stringstream text;
  text << log.rdbuf();
  std::smatch match;
  const std::string content = text.str();
  if (!std::regex_search(content, match, std::regex("physical_base=0x([0-9a-fA-F]+)"))) {
    fprintf(stderr, "physical_base not found in %s\n", o.stdout_path.c_str());
    return 1;
  }
  const uint64_t base = std::stoull(match[1].str(), nullptr, 16) - 0xA0000000ull;
  HANDLE process = OpenProcess(
      PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, o.pid);
  if (!process) {
    fprintf(stderr, "OpenProcess failed %lu\n", GetLastError());
    return 1;
  }
  Guest guest(process, base);
  if (o.find_world) {
    // Not found yet (menus, loading) is not fatal: the loop keeps rescanning.
    o.world = FindWorld(process, guest, base);
    fprintf(stdout, "world=0x%08X t_us=0\n", o.world);
    fflush(stdout);
  }
  std::vector<uint8_t> world(0x22A8);
  std::vector<uint8_t> raw(0x260);
  std::ofstream out(o.out, std::ios::out | std::ios::trunc);
  out << "t_us,valid,client_step,world_frame,world_step,command_clock,"
         "x,y,z,heading,pitch,state_seq,pred_depth,px,py,pz,pheading,ppitch,pred_seq,"
         "in0,in1,in2,in3,in4,in5,buttons,cmd_seq,new_cmds,new_cmd_ms,new_retired\n";
  // Client input queue (world+0xB08, as read by the V267 capture): q+0x30 is
  // the cumulative command sequence, q+0x10 the ring write index; each 36-byte
  // ring entry holds kind (0xFF once retired), size and a duration in ms.
  uint32_t last_cmd_seq = 0;
  bool have_cmd_seq = false;
  std::vector<uint8_t> ring_entry(36);
  LARGE_INTEGER frequency, start, now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  fprintf(stdout, "qpc_start=%lld qpc_frequency=%lld\n", start.QuadPart, frequency.QuadPart);
  fflush(stdout);
  const int64_t end = start.QuadPart + int64_t(o.seconds * double(frequency.QuadPart));
  const int64_t step = int64_t(double(o.interval_us) * double(frequency.QuadPart) / 1e6);
  int64_t due = start.QuadPart;
  int64_t last_find = start.QuadPart;
  int64_t last_housekeeping = start.QuadPart;
  uint64_t samples = 0, invalid = 0;
  // High-resolution timer: sub-millisecond waits without a busy spin, so the
  // sampler stays cheap next to a running game. Fallback: coarse Sleep + spin.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) timeBeginPeriod(1);
  char row[768];
  for (;;) {
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= end) break;
    if (now.QuadPart < due) {
      const int64_t wait_100ns = (due - now.QuadPart) * 10000000 / frequency.QuadPart;
      if (timer) {
        LARGE_INTEGER relative;
        relative.QuadPart = -std::max<int64_t>(wait_100ns, 1);
        if (SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE)) {
          WaitForSingleObject(timer, INFINITE);
        }
      } else if (wait_100ns > 15000) {
        Sleep(DWORD(wait_100ns / 10000 - 1));
      }
      continue;
    }
    // No catch-up bursts after a stall (for example a world rescan).
    due += step;
    if (due <= now.QuadPart) due = now.QuadPart + step;
    if (now.QuadPart - last_housekeeping >= frequency.QuadPart) {
      last_housekeeping = now.QuadPart;
      out.flush();
      if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
      if (!o.stop_file.empty() &&
          GetFileAttributesA(o.stop_file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        break;
      }
    }
    const double t_us = double(now.QuadPart - start.QuadPart) * 1e6 / double(frequency.QuadPart);
    bool valid = guest.Read(o.world, world.data(), world.size()) &&
                 BE32(world.data()) == 0x820807E0u;
    ActorView actor, predicted;
    uint32_t pred_depth = 0;
    if (valid) {
      const uint8_t* w = world.data();
      const uint32_t id = BE32(w + 0x218);
      const uint32_t threshold = BE32(w + 0xF74);
      const uint32_t table = id >= threshold ? BE32(w + 0xF70) : BE32(w + 0xF64);
      const uint32_t index = id >= threshold ? id - threshold : id;
      uint8_t list[0x1C];
      uint8_t slot_bytes[4];
      valid = guest.Read(table, list, sizeof(list)) && index < BE32(list + 4) &&
              guest.Read(BE32(list + 0x18) + index * 4, slot_bytes, 4) &&
              ReadActor(guest, BE32(slot_bytes), actor, raw.data()) &&
              BE16(raw.data() + 0x170) == id && BE16(raw.data() + 0x172) != 0;
      if (valid) {
        predicted = actor;
        uint32_t next = BE32(raw.data() + 0x248);
        std::vector<uint8_t> next_raw(0x260);
        while (next && pred_depth < 20) {
          ActorView candidate;
          if (!ReadActor(guest, next, candidate, next_raw.data())) break;
          uint8_t next_state[0x38];
          if (!guest.Read(BE32(next_raw.data() + 0x18C), next_state, sizeof(next_state)) ||
              !(BE32(next_state + 0x14) & 0x08000000u)) {
            break;
          }
          predicted = candidate;
          ++pred_depth;
          next = BE32(next_raw.data() + 0x248);
        }
      }
    }
    ++samples;
    if (!valid) {
      ++invalid;
      snprintf(row, sizeof(row), "%.1f,0\n", t_us);
      out << row;
      if (o.find_world && now.QuadPart - last_find >= 2 * frequency.QuadPart) {
        const uint32_t found = FindWorld(process, guest, base);
        QueryPerformanceCounter(&now);
        last_find = now.QuadPart;
        if (found && found != o.world) {
          o.world = found;
          have_cmd_seq = false;
          fprintf(stdout, "world=0x%08X t_us=%.1f\n", found, t_us);
          fflush(stdout);
        }
      }
      continue;
    }
    const uint8_t* w = world.data();
    uint32_t cmd_seq = 0, new_cmds = 0, new_cmd_ms = 0, new_retired = 0;
    {
      uint8_t q[0x34];
      uint8_t storage[0x1C];
      if (guest.Read(BE32(w + 0xB08), q, sizeof(q)) &&
          guest.Read(BE32(q + 0xC), storage, sizeof(storage))) {
        const uint32_t capacity = BE32(storage + 4);
        const uint32_t ring = BE32(storage + 0x18);
        cmd_seq = BE32(q + 0x30);
        const uint32_t write = BE32(q + 0x10);
        const uint32_t n = cmd_seq - last_cmd_seq;
        if (have_cmd_seq && n && capacity >= 2 && capacity <= 4096 && n <= capacity) {
          new_cmds = n;
          for (uint32_t k = 0; k < n; ++k) {
            const uint32_t slot = (write + capacity - n + k) % capacity;
            if (!guest.Read(ring + slot * 36, ring_entry.data(), 36)) break;
            if (ring_entry[0] == 0xFF) {
              ++new_retired;
            } else {
              new_cmd_ms += ring_entry[2];
            }
          }
        }
        last_cmd_seq = cmd_seq;
        have_cmd_seq = true;
      }
    }
    snprintf(row, sizeof(row),
             "%.1f,1,%u,%u,%.9g,%.9f,%.4f,%.4f,%.4f,%.5f,%.5f,%u,%u,%.4f,%.4f,%.4f,%.5f,%.5f,%u,"
             "%.5g,%.5g,%.5g,%.5g,%.5g,%.5g,%u,%u,%u,%u,%u\n",
             t_us, BE32(w + 0xCA0), BE32(w + 0x198), BEF(w + 0x19C), BED(w + 0xC18),
             actor.position[0], actor.position[1], actor.position[2], Heading(actor.forward),
             Pitch(actor.forward), actor.state_sequence, pred_depth, predicted.position[0],
             predicted.position[1], predicted.position[2], Heading(predicted.forward),
             Pitch(predicted.forward), predicted.state_sequence, BEF(w + 0x1C84),
             BEF(w + 0x1C88), BEF(w + 0x1C8C), BEF(w + 0x1C90), BEF(w + 0x1C94),
             BEF(w + 0x1C98), BE32(w + 0x1CB4), cmd_seq, new_cmds, new_cmd_ms, new_retired);
    out << row;
  }
  if (timer) {
    CloseHandle(timer);
  } else {
    timeEndPeriod(1);
  }
  out.flush();
  CloseHandle(process);
  fprintf(stdout, "samples=%llu invalid=%llu out=%s\n", (unsigned long long)samples,
          (unsigned long long)invalid, o.out.c_str());
  return invalid == samples ? 3 : 0;
}
