#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReadText(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  const std::string source = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/texture_cache.cpp");
  const std::string command_processor = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");

  if (source.find(
          "embedded_texture_readback_address_min, 0") ==
          std::string::npos ||
      source.find(
          "embedded_texture_readback_address_max, 0") ==
          std::string::npos) {
    return Fail("texture readback diagnostic must remain disabled by default");
  }
  if (source.find(
          "texture_readback_address_max > texture_readback_address_min") ==
          std::string::npos ||
      source.find(
          "texture_guest_base >= texture_readback_address_min") ==
          std::string::npos ||
      source.find(
          "texture_guest_base < texture_readback_address_max") ==
          std::string::npos) {
    return Fail("texture readback must use an explicit half-open guest range");
  }
  if (source.find("!texture_readback_diagnostic_started_") ==
          std::string::npos ||
      source.find("texture_readback_diagnostic_started_ = true") ==
          std::string::npos) {
    return Fail("texture readback must be strictly one-shot");
  }
  if (source.find("host_slice_layout_base.Offset, payload_size") ==
          std::string::npos ||
      source.find("texture_readback_dest.PlacedFootprint.Offset = resource_offset") ==
          std::string::npos) {
    return Fail("scratch and texture copies must preserve their real footprints");
  }
  if (source.find("HashTextureDiagnosticRows(") == std::string::npos ||
      source.find("texture_readback_row_bytes_") == std::string::npos) {
    return Fail("readback hashes must exclude undefined row padding");
  }
  if (source.find("CaptureActiveTextureReadbackDiagnostics(") ==
          std::string::npos ||
      source.find("embedded_texture_readback_policy::TryReserve(") ==
          std::string::npos ||
      source.find("active_texture_readback_diagnostic_policy_.key_count") ==
          std::string::npos ||
      source.find("REX_EMBEDDED_ACTIVE_TEXTURE_READBACK_READY") ==
          std::string::npos ||
      source.find("diagnostic.row_count - 1") == std::string::npos ||
      source.find("diagnostic.resource_identity") == std::string::npos ||
      command_processor.find("0xA59B41D0BD79484B") ==
          std::string::npos ||
      command_processor.find(
          "IsCurrentEmbeddedGameplayCaptureFrame() && pixel_shader") ==
          std::string::npos) {
    return Fail(
        "final composition resource readback must remain bounded and shader-keyed");
  }
  if (command_processor.find(
          "embedded_scaled_texture_readback_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "state.texture_fetch_indices, state.texture_fetch_count, true") ==
          std::string::npos ||
      command_processor.find(
          "embedded_manual_capture_policy::IsCompleteFrameArmed(") ==
          std::string::npos) {
    return Fail(
        "generic scaled-resource capture must be default-off and selected-frame-only");
  }
  if (source.find("REX_EMBEDDED_TEXTURE_LOAD_READBACK_READY") ==
          std::string::npos ||
      source.find("scaled_resolve=%u scale=%ux%u") ==
          std::string::npos) {
    return Fail("readback evidence must include resource identity and scale");
  }
  if (source.find("ArmTextureReadbackDiagnostic(") == std::string::npos ||
      source.find("texture_readback_armed_address_max_") ==
          std::string::npos ||
      source.find("texture_guest_end > texture_readback_armed_address_min_") ==
          std::string::npos) {
    return Fail("semantic readback arming must use an overflow-safe overlap interval");
  }
  if (command_processor.find(
          "!embedded_swap_readback.attempted &&\n      "
          "IsCurrentEmbeddedGameplayCaptureFrame() &&\n      "
          "embedded_frame_frontier.successful_resolves >= 8") ==
          std::string::npos ||
      command_processor.find("embedded_frame_frontier.draw_packets >= 500") !=
          std::string::npos ||
      command_processor.find(
          "embedded_frame_frontier.pixel_shader_draws >= 250") !=
          std::string::npos) {
    return Fail(
        "swap readback must follow the deterministic capture frame rather than title-specific draw thresholds");
  }
  const size_t resolve_call = command_processor.find(
      "const bool resolved = render_target_cache_->Resolve(");
  const size_t semantic_arm = command_processor.find(
      "texture_cache_->ArmTextureReadbackDiagnostic(");
  if (resolve_call == std::string::npos || semantic_arm == std::string::npos ||
      semantic_arm < resolve_call ||
      command_processor.find(
          "resolved && written_length && arm_scene_color_texture_readback",
          resolve_call) == std::string::npos) {
    return Fail("semantic scene-color readback must arm from the successful resolve output");
  }

  std::cout << "GPU texture readback policy regression passed\n";
  return 0;
}
