#include <rex/ui/d3d12/d3d12_present_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

std::string ReadText(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}
}  // namespace

int main() {
  using namespace rex::ui::d3d12;
  bool passed = true;

  passed &= Check(ParseHostPresentMode("vsync") == HostPresentMode::kVsync,
                  "vsync must parse");
  passed &= Check(ParseHostPresentMode("immediate") ==
                      HostPresentMode::kImmediate,
                  "immediate must parse");
  passed &= Check(ParseHostPresentMode("vrr") ==
                      HostPresentMode::kVariableRefreshRate,
                  "vrr must parse");
  passed &= Check(!ParseHostPresentMode("adaptive"),
                  "unknown present modes must be rejected");

  const UINT synchronized_swap_chain_flags =
      ResolveHostSwapChainFlags(false);
  passed &= Check(
      synchronized_swap_chain_flags ==
          UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
      "every swap chain must enable the per-chain frame-latency contract");
  const UINT tearing_swap_chain_flags = ResolveHostSwapChainFlags(true);
  passed &= Check(
      (tearing_swap_chain_flags &
       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0 &&
          (tearing_swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0,
      "tearing support must retain the frame-latency contract");

  const std::string presenter = ReadText(
      std::string(DARKNESS_SOURCE_ROOT) +
      "/external/ReXGlue/src/ui/d3d12/d3d12_presenter.cpp");
  passed &= Check(
      presenter.find(
          "ResolveHostSwapChainFlags(paint_context_.swap_chain_allows_tearing)") !=
          std::string::npos,
      "ResizeBuffers must preserve both immutable swap-chain flags");
  passed &= Check(
      presenter.find(
          "ResolveHostSwapChainFlags(dxgi_supports_tearing_)") !=
          std::string::npos,
      "swap-chain creation must enable the frame-latency contract");
  passed &= Check(
      presenter.find("SetMaximumFrameLatency(") != std::string::npos,
      "the validated per-swap-chain latency setting must remain applied");
  passed &= Check(
      presenter.find("REX_HOST_PRESENT_EFFECTIVE") != std::string::npos &&
          presenter.find("vrr_active=%u") != std::string::npos,
      "the effective VRR-or-VSync policy must be captured once for validation");

  const auto vsync = ResolveHostPresentParameters(
      HostPresentMode::kVsync, true);
  passed &= Check(vsync.sync_interval == 1 && vsync.flags == 0 &&
                      !vsync.variable_refresh_rate_active,
                  "vsync must never request tearing");

  const auto immediate_tearing = ResolveHostPresentParameters(
      HostPresentMode::kImmediate, true);
  passed &= Check(immediate_tearing.sync_interval == 0 &&
                      immediate_tearing.flags == DXGI_PRESENT_ALLOW_TEARING &&
                      !immediate_tearing.variable_refresh_rate_active,
                  "immediate mode must use supported tearing");

  const auto immediate_no_tearing = ResolveHostPresentParameters(
      HostPresentMode::kImmediate, false);
  passed &= Check(immediate_no_tearing.sync_interval == 0 &&
                      immediate_no_tearing.flags == 0,
                  "immediate mode must remain valid without tearing support");

  const auto vrr = ResolveHostPresentParameters(
      HostPresentMode::kVariableRefreshRate, true);
  passed &= Check(vrr.sync_interval == 0 &&
                      vrr.flags == DXGI_PRESENT_ALLOW_TEARING &&
                      vrr.variable_refresh_rate_active,
                  "VRR must use the DXGI tearing contract");

  const auto vrr_fallback = ResolveHostPresentParameters(
      HostPresentMode::kVariableRefreshRate, false);
  passed &= Check(vrr_fallback.sync_interval == 1 &&
                      vrr_fallback.flags == 0 &&
                      !vrr_fallback.variable_refresh_rate_active,
                  "unsupported VRR must fall back to tear-free VSync");

  if (passed) {
    std::cout << "D3D12 host present-policy tests passed\n";
  }
  return passed ? 0 : 1;
}
