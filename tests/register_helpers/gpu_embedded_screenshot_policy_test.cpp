#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <rex/graphics/embedded_screenshot_policy.h>

namespace {

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  using rex::graphics::embedded_screenshot_policy::IsCaptureRequest;
  using rex::ui::VirtualKey;

  bool passed = true;
  passed &= Check(IsCaptureRequest(VirtualKey::kF12, false),
                  "fresh F12 must request exactly one capture");
  passed &= Check(!IsCaptureRequest(VirtualKey::kF12, true),
                  "F12 repeats must not request captures");
  passed &= Check(!IsCaptureRequest(VirtualKey::kF11, false),
                  "unrelated physical keys must not request captures");

  const std::string root = DARKNESS_SOURCE_ROOT;
  const std::string plugin =
      Read(root + "/external/ReXGlue/src/graphics/plugin_main.cpp");
  const std::string plugin_header =
      Read(root + "/external/ReXGlue/include/rex/system/gpu_plugin.h");
  const std::string runtime = Read(root + "/runtime/runtime_graphics.cpp");
  const std::string main_source = Read(root + "/runtime/main.cpp");
  passed &= Check(!plugin.empty() && !plugin_header.empty() &&
                      !runtime.empty() && !main_source.empty(),
                  "screenshot lifecycle sources must be readable");

  const size_t service = plugin.find("void StartEmbeddedScreenshotService(");
  const size_t wait = plugin.find("screenshot_condition.wait", service);
  const size_t capture = plugin.find("CaptureGuestOutput(image)", wait);
  const size_t service_end =
      plugin.find("void StopEmbeddedScreenshotService(", service);
  const std::string service_body =
      service != std::string::npos && service_end != std::string::npos
          ? plugin.substr(service, service_end - service)
          : std::string();
  passed &= Check(service < wait && wait < capture && capture < service_end,
                  "capture readback must occur only after the sleeping worker wakes");
  passed &= Check(service_body.find("sleep_for") == std::string::npos,
                  "the screenshot worker must not poll at idle");
  passed &= Check(plugin.find("event.set_handled(true)") != std::string::npos &&
                      plugin.find("std::numeric_limits<size_t>::max()") !=
                          std::string::npos,
                  "F12 must be reserved above title keyboard input");
  const size_t physical_edge = plugin.find(
      "embedded_screenshot_policy::IsCaptureRequest(");
  const size_t state_request = plugin.find(
      "RequestEmbeddedGameplayCapture();", physical_edge);
  const size_t screenshot_request = plugin.find(
      "QueueEmbeddedScreenshot(embedded_);", physical_edge);
  passed &= Check(physical_edge < state_request &&
                      state_request < screenshot_request,
                  "the fresh physical F12 edge must pair the screenshot with a bounded GPU-state request");
  passed &= Check(plugin.find("window->RemoveInputListener(&embedded.screenshot_listener)") !=
                      std::string::npos,
                  "the screenshot listener must detach on the UI thread");
  passed &= Check(plugin.find("L\"wbx\"") != std::string::npos &&
                      plugin.find("\"wbx\"") != std::string::npos,
                  "screenshot creation must refuse to overwrite an existing file");

  const size_t destroy = plugin.find("rex_gpu_embedded_destroy");
  const size_t stop = plugin.find("StopEmbeddedScreenshotService", destroy);
  const size_t shutdown = plugin.find("embedded->graphics->Shutdown()", stop);
  passed &= Check(destroy < stop && stop < shutdown,
                  "the capture worker must stop before GPU shutdown");

  passed &= Check(plugin_header.find("kEmbeddedGpuAbiVersion = 12") !=
                          std::string::npos &&
                      runtime.find("kEmbeddedGpuAbiVersion = 12") !=
                          std::string::npos,
                  "host and plugin must agree on embedded ABI 12");
  passed &= Check(plugin_header.find("screenshot_root_utf8") !=
                          std::string::npos &&
                      runtime.find("info.screenshot_root_utf8") !=
                          std::string::npos,
                  "the managed screenshot root must cross the embedded ABI");

  const size_t configure = main_source.find(
      "ConfigureRuntimeGraphicsScreenshotRoot(");
  const size_t managed_root = main_source.find(
      "launchOptions.userDataRoot / L\"screenshots\"", configure);
  const size_t host_start = main_source.find("HOST_START", managed_root);
  passed &= Check(configure < managed_root && managed_root < host_start,
                  "the managed screenshot root must be fixed before title startup");

  if (!passed) {
    return 1;
  }
  std::cout << "Embedded screenshot ownership policy passed\n";
  return 0;
}
