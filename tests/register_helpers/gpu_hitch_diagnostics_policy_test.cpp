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
  const std::string command_processor = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  const std::string launcher = ReadText(
      root + "/scripts/start-pc-phase1-validation-probe.ps1");
  const std::string presenter = ReadText(
      root + "/external/ReXGlue/src/ui/d3d12/d3d12_presenter.cpp");
  const std::string render_target_cache = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/render_target_cache.cpp");
  const std::string texture_cache = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/texture_cache.cpp");
  const std::string pipeline_cache = ReadText(
      root + "/external/ReXGlue/src/graphics/d3d12/pipeline_cache.cpp");
  const std::string dxbc_translator = ReadText(
      root + "/external/ReXGlue/src/graphics/pipeline/shader/dxbc_translator.cpp");
  const std::string dxbc_translator_fetch = ReadText(
      root + "/external/ReXGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp");
  const std::string dxbc_translator_alu = ReadText(
      root + "/external/ReXGlue/src/graphics/pipeline/shader/dxbc_translator_alu.cpp");
  const std::string imports = ReadText(
      root + "/runtime/verified_imports.cpp");

  if (command_processor.find(
          "REXCVAR_DEFINE_BOOL(embedded_hitch_diagnostics, false") ==
      std::string::npos) {
    return Fail("embedded hitch timing must be disabled by default");
  }

  const std::string gated_timing =
      "!kernel_state_ && REXCVAR_GET(embedded_hitch_diagnostics)";
  const size_t first_gate = command_processor.find(gated_timing);
  if (first_gate == std::string::npos ||
      command_processor.find("!kernel_state_ && (REXCVAR_GET(embedded_hitch_diagnostics) || cp_cadence_.active)") ==
          std::string::npos) {
    return Fail("swap timing must be opt-in; draw timing may also use bounded opt-in CP windows");
  }

  if (command_processor.find("host_submission_backlog=%llu") ==
          std::string::npos ||
      command_processor.find("submission_fence_->GetCompletedValue()") ==
          std::string::npos) {
    return Fail("bounded hitch records must include real host submission backlog");
  }

  if (command_processor.find(
          "bool IsEmbeddedFrameDiagnosticsEnabled()") == std::string::npos ||
      command_processor.find(
          "!IsEmbeddedGameplayCaptureEnabled() ||") != std::string::npos) {
    return Fail("ordinary runs must not collect or read back gameplay frontier state");
  }

  if (command_processor.find(
          "embedded_resolve_source_capture_min_draw_packets, 500") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_source_capture_min_pixel_draws, 250") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_source_capture_min_prior_resolves, 8") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_source_capture_destination, 0") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_source_capture_scene_color, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_boundary_capture_first_scene_feedback, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_resolve_source_capture_first_full_frame, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_seed_draw_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_mixed_scale_transition_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_chain_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_chain_late_summary, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_chain_checkpoint_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_simple_title_composition_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_color_map_chain_capture, false") ==
          std::string::npos ||
      command_processor.find("static uint32_t color_map_capture_mask = 0") ==
          std::string::npos ||
      command_processor.find("slots, slot_count, false, draw_ordinal, true") ==
          std::string::npos ||
      command_processor.find("rex_color_map_stage_%u_after_draw_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_depth_snapshot_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_depth_pair_trace, false") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_shaded_texture_capture, false") ==
          std::string::npos ||
      command_processor.find(
          "scene_shaded_effect_fetch.format == "
          "xenos::TextureFormat::k_8_8_8_8") == std::string::npos ||
      command_processor.find(
          "scene_shaded_effect_fetch.endianness == xenos::Endian::k8in32") ==
          std::string::npos ||
      command_processor.find(
          "scene_shaded_effect_fetch.size_2d.width + 1 == 256") ==
          std::string::npos ||
      command_processor.find(
          "diagnostic.resource_depth_or_array_size") == std::string::npos ||
      command_processor.find("capture_current_destination == "
                             "capture_target_destination") ==
          std::string::npos ||
      command_processor.find(
          "capture_source_color_info == 0x000C0300") ==
          std::string::npos ||
      command_processor.find(
          "capture_control.value == 0x00100140") ==
          std::string::npos ||
      command_processor.find(
          "capture_source_color_info == 0x00030300") ==
          std::string::npos ||
      command_processor.find(
          "rex_first_full_resolve_source_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_feedback_resolve_source_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_feedback_edram_after_dump.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_feedback_scaled_after_copy.bin") ==
          std::string::npos ||
      command_processor.find(
          "capture_scene_color_resolve || capture_first_full_frame_resolve") ==
          std::string::npos ||
      command_processor.find(
          "pixel_shader->ucode_data_hash() == UINT64_C(0xAC56A81E9E6339F7)") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_seed_after_draw_1_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_seed_before_draw_1_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_seed_pre_draw_captured") ==
          std::string::npos ||
      command_processor.find(
          "rex_mixed_scale_native_after_draw_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_mixed_scale_scaled_before_draw_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_mixed_scale_scaled_after_draw_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_chain_after_draw_%u_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_chain_capture_count < 6") ==
          std::string::npos ||
      command_processor.find(
          "summary_ordinal > 6 && summary_ordinal <= 32") ==
          std::string::npos ||
      command_processor.find(
          "scaled_hdr_scene_chain_late\", nullptr") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_CHAIN_LATE_SUMMARY ordinal=%u result=%u") ==
          std::string::npos ||
      command_processor.find(
          "checkpoint_ordinal == 8 || checkpoint_ordinal == 16") ==
          std::string::npos ||
      command_processor.find(
          "checkpoint_ordinal == 24 || checkpoint_ordinal == 32") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_chain_checkpoint_after_draw_%u_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_CHAIN_CHECKPOINT ordinal=%u result=%u") ==
          std::string::npos ||
      command_processor.find(
          "pixel_shader->ucode_data_hash() == UINT64_C(0x5AD41773AF82E9ED)") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SIMPLE_TITLE_INPUTS result=queued") ==
          std::string::npos ||
      command_processor.find(
          "rex_simple_title_composition_after_draw_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "pixel_shader->ucode_data_hash() == UINT64_C(0xBE763931E2AB7D56)") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_depth_snapshot_attempted") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_depth_before_first_corrupting_draw.bin") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_depth_pair_trace_count < 16") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_DEPTH_PAIR ordinal=%u kind=%s") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_PIPELINE ordinal=%u kind=%s") ==
          std::string::npos ||
      command_processor.find("msaa_2x_supported=%u") ==
          std::string::npos ||
      command_processor.find("programmable_sample_tier=%u") ==
          std::string::npos ||
      command_processor.find("host_sample_mask=0x%08X") ==
          std::string::npos ||
      command_processor.find("poly_preferred=%a,%a") ==
          std::string::npos ||
      command_processor.find("host_depth_bias=%d") ==
          std::string::npos ||
      command_processor.find("host_slope_bias=%a") ==
          std::string::npos ||
      command_processor.find("host_viewport=%u,%u,%u,%u,%a,%a") ==
          std::string::npos ||
      command_processor.find("host_ndc=%a,%a,%a,%a,%a,%a") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_OCCLUSION result=%s ordinal=%u kind=%s") ==
          std::string::npos ||
      command_processor.find("normalized_samples=%llu") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_occlusion_record_count ==") ==
          std::string::npos ||
      command_processor.find(
          "embedded_scene_depth_phase_trace, false") ==
          std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_DEPTH_PHASE result=%u label=%s") ==
          std::string::npos ||
      command_processor.find(
          "before_prepass_pair_1") == std::string::npos ||
      command_processor.find(
          "before_shaded_group_2") == std::string::npos ||
      command_processor.find(
          "before_shadow_volume_group_1") == std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_SHADOW_OCCLUSION result=skipped") ==
          std::string::npos ||
      command_processor.find(
          "after_shadow_volume_8") == std::string::npos ||
      command_processor.find(
          "vertex_constants_hash") == std::string::npos ||
      command_processor.find(
          "position_bones_hash") == std::string::npos ||
      command_processor.find(
          "referenced_bones_hash") == std::string::npos ||
      command_processor.find(
          "position_bone_offset_bitmap") == std::string::npos ||
      command_processor.find(
          "REX_EMBEDDED_SCENE_SHADED_TEXTURES result=queued") ==
          std::string::npos ||
      command_processor.find(
          "rex_scene_shaded_sample_output_fp16.bin") ==
          std::string::npos ||
      command_processor.find(
          "if (capture_embedded_scene_shaded_textures)") ==
          std::string::npos ||
      command_processor.find(
          "kPositionVertexStrideWords = 7") == std::string::npos ||
      command_processor.find(
          "embedded_scene_seed_output_capture_count < 2") ==
          std::string::npos ||
      command_processor.find(
          "IsCurrentEmbeddedGameplayCaptureFrame()") == std::string::npos) {
    return Fail(
        "pre-EDRAM source capture must stay bounded, opt-in, and keep production thresholds");
  }

  if (texture_cache.find("base_array_size > 16") == std::string::npos ||
      texture_cache.find("array_slice * uint32_t(texture_desc.MipLevels)") ==
          std::string::npos ||
      texture_cache.find("diagnostic.array_slice = array_slice") ==
          std::string::npos ||
      texture_cache.find("a%uof%u_r%016llX_d%llu_%08X.bin") ==
          std::string::npos ||
      texture_cache.find(
          "embedded_texture_readback_policy::TryReserve(") ==
          std::string::npos) {
    return Fail(
        "active texture diagnostics must capture bounded base array slices");
  }

  if (pipeline_cache.find(
          "d3d12_embedded_scene_sample_diagnostic, 0") ==
          std::string::npos ||
      pipeline_cache.find(".range(0, 7)") == std::string::npos ||
      pipeline_cache.find("modification.pixel.texture_sample_diagnostic") ==
          std::string::npos ||
      dxbc_translator.find(
          "texture_sample_diagnostic != 0") == std::string::npos ||
      dxbc_translator_fetch.find(
          "kDiagnosticFetchConstants[] = {4, 1, 0, 2}") ==
          std::string::npos ||
      dxbc_translator_fetch.find(
          "system_temps_color_[0], 0b0111") == std::string::npos) {
    return Fail(
        "scene shader sample exposure must remain exact, opt-in, and cache-keyed");
  }

  if (pipeline_cache.find(
          "d3d12_embedded_scene_shaded_depth_compare_diagnostic, 0") ==
          std::string::npos ||
      pipeline_cache.find(
          "Diagnose the scaled embedded scene shaded depth mismatch") ==
          std::string::npos ||
      pipeline_cache.find(".range(0, 2)") == std::string::npos ||
      pipeline_cache.find("UINT64_C(0x818A2B33A7AB4ECE)") ==
          std::string::npos ||
      pipeline_cache.find("UINT64_C(0xBE763931E2AB7D56)") ==
          std::string::npos ||
      pipeline_cache.find(
          "description.depth_func == xenos::CompareFunction::kEqual") ==
          std::string::npos ||
      pipeline_cache.find("xenos::CompareFunction::kLessEqual") ==
          std::string::npos ||
      pipeline_cache.find("xenos::CompareFunction::kGreaterEqual") ==
          std::string::npos ||
      pipeline_cache.find(
          "REX_EMBEDDED_SCENE_SHADED_DEPTH_DIRECTION") ==
          std::string::npos ||
      pipeline_cache.find("diagnostic_ordinal < 4") == std::string::npos ||
      pipeline_cache.find(
          "uint32_t(effective_depth_func)") == std::string::npos) {
    return Fail(
        "scaled shaded depth direction diagnostic must stay exact, bounded, and opt-in");
  }

  if (dxbc_translator_alu.find(
          "is_embedded_scene_final_rgb_multiply") == std::string::npos ||
      dxbc_translator_alu.find("scene_diagnostic >= 5") ==
          std::string::npos ||
      dxbc_translator_alu.find(
          "instr.vector_and_constant_result.storage_index == 4") ==
          std::string::npos ||
      dxbc_translator_alu.find(
          "instr.vector_operands[0].storage_index == 0") ==
          std::string::npos ||
      dxbc_translator_alu.find(
          "instr.vector_operands[1].storage_index == 4") ==
          std::string::npos ||
      dxbc_translator.find(
          "scene_diagnostic == 5 || scene_diagnostic == 6") ==
          std::string::npos ||
      dxbc_translator_alu.find(
          "system_temps_embedded_scene_final_multiply_operands_[0]") ==
          std::string::npos ||
      dxbc_translator.find(
          "B=max(abs(the actually exported color.xyz))") ==
          std::string::npos) {
    return Fail(
        "scene shader final RGB diagnostic must remain tied to the exact multiply");
  }

  if (launcher.find("REX_EMBEDDED_HITCH_DIAGNOSTICS = 'true'") ==
          std::string::npos ||
      launcher.find("REX_DISPLAY_PRESENT_DIAGNOSTICS = 'true'") ==
          std::string::npos ||
      launcher.find("--hitch-diagnostics") == std::string::npos) {
    return Fail("controlled validation launcher must explicitly opt into hitch timing");
  }

  if (render_target_cache.find(
          "embedded_scene_depth_transfer_trace, false") == std::string::npos ||
      render_target_cache.find(
          "embedded_scene_depth_transfer_trace_count < 32") ==
          std::string::npos ||
      render_target_cache.find(
          "REX_EMBEDDED_SCENE_DEPTH_TRANSFER_SOURCE") == std::string::npos ||
      render_target_cache.find(
          "dest_rt_key.GetDepthFormat() ==") == std::string::npos ||
      render_target_cache.find(
          "REX_EMBEDDED_SCENE_DEPTH_READBACK result=ok") ==
          std::string::npos ||
      render_target_cache.find("kDumpTileCount = kDumpPitchTiles * kDumpRows") ==
          std::string::npos ||
      render_target_cache.find("mismatch_group_count") ==
          std::string::npos ||
      render_target_cache.find("parity_stencil_histogram") ==
          std::string::npos ||
      render_target_cache.find("parity_depth_hash") == std::string::npos ||
      render_target_cache.find("depth_hash=%08X,%08X,%08X,%08X") ==
          std::string::npos ||
      render_target_cache.find("stencil_7F=%llu,%llu,%llu,%llu") ==
          std::string::npos ||
      render_target_cache.find(
          "depth_nonzero_stencil_80=%llu,%llu,%llu,%llu") ==
          std::string::npos ||
      render_target_cache.find(
          "stage=edram_after_dump") == std::string::npos ||
      texture_cache.find(
          "CaptureCurrentScaledResolveRange(") == std::string::npos ||
      texture_cache.find(
          "stage=scaled_after_copy") == std::string::npos) {
    return Fail(
        "scene depth and resolve-boundary tracing must remain exact, bounded, and opt-in");
  }

  if (presenter.find(
          "REXCVAR_DEFINE_BOOL(display_present_diagnostics, false") ==
          std::string::npos ||
      presenter.find(
          "record_present_timing &&") == std::string::npos) {
    return Fail("ordinary presentation must not take per-frame diagnostic timings");
  }

  if (imports.find("const bool recordHitch = RuntimeHitchDiagnosticsEnabled();") ==
          std::string::npos ||
      imports.find("const bool slowRead = recordHitch &&") ==
          std::string::npos ||
      imports.find("if (recordHitch && waitDurationUs >= 75000)") ==
          std::string::npos) {
    return Fail("ordinary file reads and waits must not take hitch timestamps");
  }

  std::cout << "GPU hitch diagnostics policy regression passed\n";
  return 0;
}
