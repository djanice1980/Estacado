// Time-based input injector for isolated gameplay tests (SendInput only; no
// virtual devices). Every action is scheduled on the QPC clock with a
// high-resolution waitable timer plus a short bounded spin, so a 100 ms tap is
// 100 ms of wall-clock time at any frame rate. Nothing is sent unless the
// foreground window belongs to --pid; if focus moves away the script stops and
// every held key is released. Each injected event is logged with its QPC time
// so the sampler/observer traces can be aligned exactly.
//
// input_script --pid N --script FILE --log FILE.csv
// Script lines (# comments):
//   wait MS                      idle
//   down KEY | up KEY            key edge (KEY: W A S D F9 SPACE RETURN ...)
//   tap KEY MS                   press, hold MS, release
//   mouse DX DY STEP_US COUNT    COUNT relative moves of (DX,DY), one per STEP_US
//   mark LABEL                   log a marker
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

LARGE_INTEGER g_frequency;
HANDLE g_timer = nullptr;
DWORD g_pid = 0;
FILE* g_log = nullptr;
std::vector<WORD> g_held;

int64_t Now() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// Sleep on the high-resolution timer until ~0.3 ms before the deadline, then
// spin the remainder (bounded: at most ~0.3 ms of spinning per event).
void WaitUntil(int64_t deadline) {
  const int64_t spin = g_frequency.QuadPart * 3 / 10000;
  for (;;) {
    const int64_t now = Now();
    if (now >= deadline) return;
    const int64_t remaining = deadline - now;
    if (remaining > spin && g_timer) {
      LARGE_INTEGER relative;
      relative.QuadPart = -std::max<int64_t>((remaining - spin) * 10000000 / g_frequency.QuadPart, 1);
      if (SetWaitableTimer(g_timer, &relative, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(g_timer, INFINITE);
        continue;
      }
    }
    YieldProcessor();
  }
}

bool TargetInForeground() {
  HWND hwnd = GetForegroundWindow();
  DWORD pid = 0;
  if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
  return pid == g_pid;
}

WORD KeyToVk(const std::string& key) {
  static const std::map<std::string, WORD> named = {
      {"SPACE", VK_SPACE}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN}, {"ESCAPE", VK_ESCAPE},
      {"TAB", VK_TAB},     {"SHIFT", VK_SHIFT},   {"UP", VK_UP},        {"DOWN", VK_DOWN},
      {"LEFT", VK_LEFT},   {"RIGHT", VK_RIGHT},   {"F9", VK_F9},        {"F3", VK_F3}};
  auto it = named.find(key);
  if (it != named.end()) return it->second;
  if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) {
    return WORD(key[0]);
  }
  return 0;
}

void Log(const char* kind, const std::string& detail) {
  fprintf(g_log, "%lld,%s,%s\n", Now(), kind, detail.c_str());
}

bool SendKey(WORD vk, bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
  if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  if (SendInput(1, &input, sizeof(input)) != 1) return false;
  if (down) {
    g_held.push_back(vk);
  } else {
    g_held.erase(std::remove(g_held.begin(), g_held.end(), vk), g_held.end());
  }
  return true;
}

bool SendMouse(int dx, int dy) {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = dx;
  input.mi.dy = dy;
  input.mi.dwFlags = MOUSEEVENTF_MOVE;  // relative, subject to no acceleration setting
  return SendInput(1, &input, sizeof(input)) == 1;
}

void ReleaseAll() {
  std::vector<WORD> held = g_held;
  for (WORD vk : held) {
    SendKey(vk, false);
    Log("release", std::to_string(vk));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string script_path, log_path;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--pid") g_pid = DWORD(strtoul(argv[i + 1], nullptr, 10));
    else if (a == "--script") script_path = argv[i + 1];
    else if (a == "--log") log_path = argv[i + 1];
  }
  if (!g_pid || script_path.empty() || log_path.empty()) {
    fprintf(stderr, "usage: input_script --pid N --script FILE --log FILE.csv\n");
    return 2;
  }
  std::ifstream script(script_path);
  if (!script) {
    fprintf(stderr, "cannot read %s\n", script_path.c_str());
    return 2;
  }
  g_log = fopen(log_path.c_str(), "w");
  if (!g_log) return 2;
  QueryPerformanceFrequency(&g_frequency);
  g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
  fprintf(g_log, "qpc,kind,detail\n");
  Log("start", "qpc_frequency=" + std::to_string(g_frequency.QuadPart));
  int status = 0;
  int64_t t = Now();
  std::string line;
  int line_number = 0;
  while (status == 0 && std::getline(script, line)) {
    ++line_number;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string op;
    if (!(in >> op)) continue;
    for (auto& c : op) c = char(tolower(c));
    auto focus_ok = [&]() {
      if (TargetInForeground()) return true;
      Log("abort", "foreground_lost line=" + std::to_string(line_number));
      status = 3;
      return false;
    };
    if (op == "wait") {
      double ms = 0;
      in >> ms;
      t += int64_t(ms * double(g_frequency.QuadPart) / 1000.0);
      WaitUntil(t);
    } else if (op == "down" || op == "up") {
      std::string key;
      in >> key;
      for (auto& c : key) c = char(toupper(c));
      const WORD vk = KeyToVk(key);
      if (!vk) { Log("error", "unknown_key " + key); status = 2; break; }
      if (op == "down" && !focus_ok()) break;
      t = Now();
      SendKey(vk, op == "down");
      Log(op.c_str(), key);
    } else if (op == "tap") {
      std::string key;
      double ms = 0;
      in >> key >> ms;
      for (auto& c : key) c = char(toupper(c));
      const WORD vk = KeyToVk(key);
      if (!vk) { Log("error", "unknown_key " + key); status = 2; break; }
      if (!focus_ok()) break;
      const int64_t down_at = Now();
      SendKey(vk, true);
      Log("down", key);
      t = down_at + int64_t(ms * double(g_frequency.QuadPart) / 1000.0);
      WaitUntil(t);
      SendKey(vk, false);
      Log("up", key + " held_us=" +
                    std::to_string((Now() - down_at) * 1000000 / g_frequency.QuadPart));
    } else if (op == "mouse") {
      int dx = 0, dy = 0, count = 0;
      double step_us = 0;
      in >> dx >> dy >> step_us >> count;
      if (!focus_ok()) break;
      const int64_t step = int64_t(step_us * double(g_frequency.QuadPart) / 1e6);
      Log("mouse_begin", std::to_string(dx) + " " + std::to_string(dy) + " " +
                             std::to_string(int(step_us)) + " " + std::to_string(count));
      t = Now();
      for (int k = 0; k < count; ++k) {
        if ((k & 63) == 0 && !TargetInForeground()) {
          Log("abort", "foreground_lost line=" + std::to_string(line_number));
          status = 3;
          break;
        }
        SendMouse(dx, dy);
        t += step;
        WaitUntil(t);
      }
      Log("mouse_end", "");
    } else if (op == "mark") {
      std::string label;
      std::getline(in, label);
      Log("mark", label);
    } else {
      Log("error", "unknown_op " + op);
      status = 2;
    }
  }
  ReleaseAll();
  Log("end", "status=" + std::to_string(status));
  fclose(g_log);
  if (g_timer) CloseHandle(g_timer);
  return status;
}
