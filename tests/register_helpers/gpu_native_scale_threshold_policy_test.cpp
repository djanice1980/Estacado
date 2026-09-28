#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <rex/graphics/pipeline/render_target/scale_class_policy.h>

namespace {

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

bool Require(const std::string& source, const std::string& needle,
             const char* failure) {
  if (source.find(needle) != std::string::npos) return true;
  std::cerr << failure << '\n';
  return false;
}

bool Reject(const std::string& source, const std::string& needle,
            const char* failure) {
  if (source.find(needle) == std::string::npos) return true;
  std::cerr << failure << '\n';
  return false;
}

}  // namespace

int main() {
  using rex::graphics::render_target::
      ShouldKeepScaledDepthForStencilOnlyAlias;
  const std::string rex = REXGLUE_SOURCE_ROOT;
  const std::string project = DARKNESS_SOURCE_ROOT;
  const std::string common_header =
      Read(rex + "/include/rex/graphics/pipeline/render_target/cache.h");
  const std::string common_source =
      Read(rex + "/src/graphics/pipeline/render_target/cache.cpp");
  const std::string d3d12_header =
      Read(rex + "/include/rex/graphics/d3d12/render_target_cache.h");
  const std::string d3d12_source =
      Read(rex + "/src/graphics/d3d12/render_target_cache.cpp");
  const std::string command_source =
      Read(rex + "/src/graphics/d3d12/command_processor.cpp");
  const std::string pipeline_header =
      Read(rex + "/include/rex/graphics/d3d12/pipeline_cache.h");
  const std::string pipeline_source =
      Read(rex + "/src/graphics/d3d12/pipeline_cache.cpp");
  const std::string translator_header = Read(
      rex + "/include/rex/graphics/pipeline/shader/dxbc_translator.h");
  const std::string translator_source = Read(
      rex + "/src/graphics/pipeline/shader/dxbc_translator.cpp");
  const std::string texture_source =
      Read(rex + "/src/graphics/pipeline/texture/cache.cpp");
  const std::string profile = Read(
      project + "/config/pc_phase1_validation/internal_only_2x_720p.toml");

  bool passed = true;
  passed &= Require(common_source, "draw_resolution_scale_threshold, 0",
                    "native-scale threshold cvar is missing");
  passed &= Require(common_source, "GetPath() != Path::kHostRenderTargets",
                    "threshold is not restricted to host render targets");
  passed &= Require(common_header, "uint32_t scale_native : 1",
                    "render-target identity does not carry scale class");
  passed &= Require(common_header, "IsResolveSourceNativeOnly",
                    "resolve ownership does not determine native source class");
  passed &= Require(d3d12_header, "uint32_t dest_scale_native : 1",
                    "transfer identity lacks destination scale class");
  passed &= Require(d3d12_header, "uint32_t source_scale_native : 1",
                    "transfer identity lacks source scale class");
  passed &= Require(d3d12_source, "resolve_copy_native_root_signature_",
                    "native resolve path is absent");
  passed &= Require(d3d12_source, "MarkRangeAsResolved(",
                    "resolve does not publish its actual texture layout");
  passed &= Require(d3d12_header,
                    "current_draw_depth_float24_convert_in_pixel_shader",
                    "float24 depth conversion policy is absent");
  passed &= Require(d3d12_source,
                    "return depth_float24_convert_in_pixel_shader_;",
                    "explicit float24 depth conversion policy is not retained");
  passed &= Reject(d3d12_header, "depth_float24_precision_lost_",
                   "coarse target-wide float24 precision marker remains");
  passed &= Reject(d3d12_source, "MarkDepthFloat24PrecisionLost",
                   "ownership transfer still enables float24 conversion automatically");
  passed &= Require(pipeline_header,
                    "uint32_t depth_float24_convert_in_pixel_shader : 1",
                    "depth-only PSO identity omits explicit float24 conversion");
  passed &= Require(pipeline_source,
                    "description.depth_float24_convert_in_pixel_shader",
                    "depth-only PSO creation ignores explicit float24 conversion");
  passed &= Require(texture_source, "scaled_resolve_pages_[i] &= ~add_bits",
                    "native resolve does not clear scaled-page metadata");
  passed &= Require(command_source, "render_target_cache_->GetDrawScaleX()",
                    "per-draw D3D12 state still assumes global scale");
  passed &= Require(command_source, "UpdateGuestOcclusionQueryScale",
                    "mixed-scale ZPD reports are not segmented");
  const size_t render_target_update =
      command_source.find("render_target_cache_->Update(");
  const size_t vertex_modification = command_source.find(
      "pipeline_cache_->GetCurrentVertexShaderModification(");
  const size_t pixel_modification = command_source.find(
      "pipeline_cache_->GetCurrentPixelShaderModification(");
  if (render_target_update == std::string::npos ||
      vertex_modification == std::string::npos ||
      vertex_modification < render_target_update) {
    std::cerr << "vertex shader scale policy is selected before render-target ownership\n";
    passed = false;
  }
  if (render_target_update == std::string::npos ||
      pixel_modification == std::string::npos ||
      pixel_modification < render_target_update) {
    std::cerr << "pixel shader depth policy is selected before ownership transfers\n";
    passed = false;
  }
  passed &= Require(command_source, "draw_scale=%ux%u scaled_textures=0x%08X",
                    "bounded frame capture lacks per-draw scale identity");
  passed &= Require(command_source, "IsActiveTextureResolutionScaled",
                    "bounded frame capture lacks resolved-texture scale identity");
  passed &= Require(command_source, "written_scaled=%u",
                    "bounded frame capture lacks resolve destination scale identity");
  passed &= Require(pipeline_source, "resolution_scale_native",
                    "pipeline/shader identity lacks native draw state");
  passed &= Require(translator_header, "GetCurrentDrawResolutionScaleX",
                    "DXBC translation lacks per-draw scale access");
  passed &= Require(translator_source, "GetCurrentDrawResolutionScaleY() >> 1",
                    "memexport de-duplication still assumes global scale");
  passed &= Require(profile, "resolution_scale = 2",
                    "internal-scale validation profile is not 2x");
  passed &= Require(profile, "draw_resolution_scale_threshold = 640",
                    "verified 640-pixel post-process threshold is not configured");
  passed &= Require(common_source,
                    "ShouldKeepScaledDepthForStencilOnlyAlias(",
                    "stencil-only scaled-depth alias preservation is absent");
  passed &= Require(common_source,
                    "owner.pitch_tiles_at_32bpp == pitch_tiles_at_32bpp",
                    "alias preservation does not require matching EDRAM sample pitch");
  passed &= Require(common_header, "bool current_draw_scale_native_ = false",
                    "effective per-draw scale class is not persisted");
  passed &= Require(common_header,
                    "bool IsDrawScaleNative() const { return current_draw_scale_native_; }",
                    "per-draw scale consumers still recompute the threshold class");
  passed &= Require(common_source,
                    "current_draw_scale_native_ = scale_native;",
                    "render-target update does not publish its final scale class");

  // Probe237's exact boundary: a native-threshold 640-wide 4x-MSAA stencil
  // pass aliases an already scaled 1280-wide 2x-MSAA D24S8 sample grid.
  passed &= ShouldKeepScaledDepthForStencilOnlyAlias(
      true, true, false, false, true, 1280, 16384);
  // Any possible depth write must retain the ordinary native-scale policy.
  passed &= !ShouldKeepScaledDepthForStencilOnlyAlias(
      true, true, true, false, true, 1280, 16384);
  // Color-writing, unrelated, already-scaled, and unsupported-size cases must
  // not be reclassified by this depth-aspect rule.
  passed &= !ShouldKeepScaledDepthForStencilOnlyAlias(
      true, true, false, true, true, 1280, 16384);
  passed &= !ShouldKeepScaledDepthForStencilOnlyAlias(
      true, true, false, false, false, 1280, 16384);
  passed &= !ShouldKeepScaledDepthForStencilOnlyAlias(
      false, true, false, false, true, 1280, 16384);
  passed &= !ShouldKeepScaledDepthForStencilOnlyAlias(
      true, true, false, false, true, 32768, 16384);

  if (passed) {
    std::cout << "GPU native-scale threshold policy tests passed\n";
  }
  return passed ? 0 : 1;
}
