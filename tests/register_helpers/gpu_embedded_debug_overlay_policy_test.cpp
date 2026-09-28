#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <rex/graphics/embedded_debug_overlay_policy.h>

namespace {

std::string Read(const std::string& path) {
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
  using rex::graphics::embedded_debug_overlay_policy::IsToggleRequest;
  using rex::ui::VirtualKey;

  bool passed = true;
  passed &= Check(IsToggleRequest(VirtualKey::kF3, false),
                  "fresh F3 must toggle the embedded overlay");
  passed &= Check(!IsToggleRequest(VirtualKey::kF3, true),
                  "held F3 must not toggle repeatedly");
  passed &= Check(!IsToggleRequest(VirtualKey::kF12, false),
                  "the overlay must not consume the screenshot key");

  const std::string root = DARKNESS_SOURCE_ROOT;
  const std::string plugin =
      Read(root + "/external/ReXGlue/src/graphics/plugin_main.cpp");
  const std::string command_processor =
      Read(root + "/external/ReXGlue/src/graphics/command_processor.cpp");
  const std::string cmake =
      Read(root + "/external/ReXGlue/CMakeLists.txt");
  passed &= Check(!plugin.empty() && !command_processor.empty() && !cmake.empty(),
                  "embedded overlay lifecycle sources must be readable");

  const size_t ensure = plugin.find("bool EmbeddedDebugOverlayListener::EnsureDrawer()");
  const size_t create_drawer = plugin.find("CreateImmediateDrawer()", ensure);
  const size_t key = plugin.find("void EmbeddedDebugOverlayListener::OnKeyDown", ensure);
  const size_t create_dialog = plugin.find("make_unique<rex::ui::DebugOverlayDialog>", key);
  passed &= Check(ensure < create_drawer && create_drawer < key && key < create_dialog,
                  "overlay rendering resources must be created lazily on F3");
  passed &= Check(plugin.find("event.set_handled(true)", key) != std::string::npos &&
                      plugin.find("std::numeric_limits<size_t>::max() - 1") !=
                          std::string::npos,
                  "F3 must be host-reserved above the title keyboard bridge");

  const size_t reset = plugin.find("void EmbeddedDebugOverlayListener::Reset()");
  const size_t detach_imgui =
      plugin.find("SetPresenterAndImmediateDrawer(nullptr, nullptr)", reset);
  const size_t detach_immediate =
      plugin.find("immediate_drawer_->SetPresenter(nullptr)", detach_imgui);
  passed &= Check(reset < detach_imgui && detach_imgui < detach_immediate,
                  "overlay resources must detach from the presenter in dependency order");

  const size_t swap = command_processor.find("ExecutePacketType3_XE_SWAP");
  const size_t timestamp = command_processor.find("QueryHostTickCount", swap);
  const size_t publish = command_processor.find("guest_frame_time_us_.store", timestamp);
  const size_t issue_swap = command_processor.find("IssueSwap(", publish);
  passed &= Check(swap < timestamp && timestamp < publish && publish < issue_swap,
                  "real XE_SWAP packets must publish the displayed frame cadence");
  passed &= Check(cmake.find(
                      "$<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>") !=
                      std::string::npos,
                  "high-frequency general counters must remain disabled in Release");

  if (!passed) return 1;
  std::cout << "Embedded F3 performance overlay policy passed\n";
  return 0;
}
