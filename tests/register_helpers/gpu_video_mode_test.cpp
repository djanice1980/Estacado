#include <rex/graphics/video_mode_util.h>

#include "runtime_video_mode.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
  using rex::graphics::video_mode_util::TryParseResolutionPreset;
  using rex::graphics::video_mode_util::IsNativeResolutionPreset;
  bool passed = true;
  int32_t width = 0;
  int32_t height = 0;

  passed &= Check(TryParseResolutionPreset("720p", width, height) &&
                      width == 1280 && height == 720,
                  "720p preset is incorrect");
  passed &= Check(TryParseResolutionPreset("1920x1080", width, height) &&
                      width == 1920 && height == 1080,
                  "explicit 1080p resolution is incorrect");
  passed &= Check(TryParseResolutionPreset("2560 X 1440", width, height) &&
                      width == 2560 && height == 1440,
                  "normalized explicit resolution is incorrect");
  passed &= Check(TryParseResolutionPreset("8192x8192", width, height) &&
                      width == 8192 && height == 8192,
                  "maximum supported custom resolution was rejected");
  passed &= Check(TryParseResolutionPreset("4K", width, height) &&
                      width == 3840 && height == 2160,
                  "4K preset is incorrect");
  passed &= Check(IsNativeResolutionPreset(" Native-Desktop "),
                  "normalized native desktop output is not recognized");
  passed &= Check(!TryParseResolutionPreset("native", width, height),
                  "native output must be resolved against a real host display");
  passed &= Check(!TryParseResolutionPreset("1920x", width, height),
                  "an incomplete resolution was accepted");
  passed &= Check(!TryParseResolutionPreset("0x1080", width, height),
                  "a zero-width resolution was accepted");
  passed &= Check(!TryParseResolutionPreset("8193x1080", width, height),
                  "an over-limit custom width was silently accepted");
  passed &= Check(!TryParseResolutionPreset("1920x8193", width, height),
                  "an over-limit custom height was silently accepted");
  passed &= Check(!TryParseResolutionPreset("native-ish", width, height),
                  "an unknown preset was accepted");

  passed &= Check(darkness::guest_video_mode::kDisplayWidth == 1280 &&
                      darkness::guest_video_mode::kDisplayHeight == 720 &&
                      darkness::guest_video_mode::kRefreshRateHz == 60.0 &&
                      darkness::guest_video_mode::kRefreshRateFloatBits ==
                          0x42700000u,
                  "the native guest video-mode contract changed");

  const std::string runtime_graphics = ReadFile(
      std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_graphics.cpp");
  const std::string imports = ReadFile(
      std::string(DARKNESS_SOURCE_ROOT) + "/runtime/verified_imports.cpp");
  const std::string plugin = ReadFile(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/plugin_main.cpp");
  const std::string graphics_system = ReadFile(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/graphics/graphics_system.cpp");
  const std::string pc_settings = ReadFile(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/runtime/runtime_pc_settings.cpp");
  passed &= Check(
      runtime_graphics.find(
          "info.refresh_rate_hz = darkness::guest_video_mode::kRefreshRateHz;") !=
          std::string::npos,
      "embedded guest VBlank no longer follows the native video-mode contract");
  passed &= Check(
      imports.find("darkness::guest_video_mode::kRefreshRateFloatBits") !=
          std::string::npos,
      "guest video-mode imports no longer follow the native refresh contract");
  passed &= Check(
      plugin.find("&embedded->memory, info->refresh_rate_hz,") !=
          std::string::npos,
      "the embedded plugin no longer forwards the host-owned guest refresh");
  passed &= Check(
      graphics_system.find(
          "std::chrono::duration<double>(1.0 / std::max(1.0, refresh_rate_hz))") !=
          std::string::npos,
      "embedded VBlank no longer derives its interval from the supplied refresh");
  passed &= Check(
      pc_settings.find("video_mode_refresh_rate") == std::string::npos,
      "the PC settings schema must not expose guest refresh before title timing is proven");

  if (passed) {
    std::cout << "PC output-resolution and guest-refresh ownership tests passed\n";
  }
  return passed ? 0 : 1;
}
