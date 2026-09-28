#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

}  // namespace

int main() {
  const std::string source_path =
      std::string(REXGLUE_SOURCE_ROOT) + "/src/graphics/plugin_main.cpp";
  const std::string source = ReadFile(source_path);
  if (source.empty()) {
    std::cerr << "embedded graphics plugin source was not readable\n";
    return 1;
  }

  const size_t function = source.find("bool StartEmbeddedPresentation(");
  const size_t create = source.find("window = rex::ui::Window::Create(", function);
  const size_t policy = source.find("ShouldStartBorderless(", create);
  const size_t set_fullscreen = source.find("window->SetFullscreen(true)", policy);
  const size_t open = source.find("window->Open()", set_fullscreen);
  bool passed = Check(function != std::string::npos &&
                          create != std::string::npos &&
                          policy != std::string::npos &&
                          set_fullscreen != std::string::npos &&
                          open != std::string::npos &&
                          function < create && create < policy &&
                          policy < set_fullscreen && set_fullscreen < open,
                      "embedded title window no longer applies the configured "
                      "fullscreen policy before opening");

  const std::string window_source = ReadFile(
      std::string(REXGLUE_SOURCE_ROOT) + "/src/ui/window_sdl.cpp");
  passed &= Check(!window_source.empty(),
                  "SDL window implementation source was not readable");
  const size_t native_size =
      window_source.find("bool TryResolveNativeOutputSize(");
  const size_t native_monitor =
      window_source.find("REXCVAR_GET(monitor)", native_size);
  const size_t native_desktop_mode =
      window_source.find("SDL_GetDesktopDisplayMode(display)", native_monitor);
  passed &= Check(native_size != std::string::npos &&
                      native_monitor != std::string::npos &&
                      native_desktop_mode != std::string::npos &&
                      native_size < native_monitor &&
                      native_monitor < native_desktop_mode,
                  "native output sizing no longer follows the selected monitor");

  const size_t open_impl = window_source.find("bool WindowSDL::OpenImpl()");
  const size_t physical_output =
      window_source.find("output_resolution_is_physical", open_impl);
  const size_t physical_width =
      window_source.find("? GetDesiredLogicalWidth()", physical_output);
  const size_t physical_height =
      window_source.find("? GetDesiredLogicalHeight()", physical_width);
  const size_t create_window =
      window_source.find("SDL_CreateWindow(", physical_height);
  passed &= Check(open_impl != std::string::npos &&
                      physical_output != std::string::npos &&
                      physical_width != std::string::npos &&
                      physical_height != std::string::npos &&
                      create_window != std::string::npos &&
                      open_impl < physical_output &&
                      physical_output < physical_width &&
                      physical_width < physical_height &&
                      physical_height < create_window,
                  "physical output resolution is multiplied by host DPI or no longer feeds window creation");
  const size_t startup_monitor =
      window_source.find("REXCVAR_GET(monitor)", open_impl);
  const size_t position =
      window_source.find("SDL_SetWindowPosition(", startup_monitor);
  const size_t fullscreen =
      window_source.find("SDL_SetWindowFullscreen(sdl_window_, true)", position);
  passed &= Check(open_impl != std::string::npos &&
                      startup_monitor != std::string::npos &&
                      position != std::string::npos &&
                      fullscreen != std::string::npos &&
                      open_impl < startup_monitor &&
                      startup_monitor < position && position < fullscreen,
                  "selected monitor is no longer applied before borderless startup");

  if (passed) {
    std::cout << "Embedded title window/monitor startup policy regression passed\n";
  }
  return passed ? 0 : 1;
}
