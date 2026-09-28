#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  std::ifstream source_file(
      root + "/scripts/run-pc-phase1-autonomous-matrix.ps1",
      std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  if (source.empty()) {
    std::cerr << "autonomous matrix runner was not readable\n";
    return 1;
  }

  const char* required_profiles[] = {
      "'original'", "'original_scale_trace'", "'original_resolve_trace'",
      "'output_only'", "'internal_only'", "'internal_scale_trace'",
      "'internal_first_full_resolve_trace'",
      "'internal_scene_seed_trace'",
      "'internal_mixed_scale_transition_trace'",
      "'original_scene_chain_trace'",
      "'internal_scene_chain_trace'",
      "'original_scene_chain_checkpoints'",
      "'internal_scene_chain_checkpoints'",
      "'original_scene_chain_full_late_trace'",
      "'internal_scene_chain_full_late_trace'",
      "'original_simple_title_composition_trace'",
      "'internal_simple_title_composition_trace'",
      "'internal_resolve_trace'",
      "'internal_scale_trace_motion_blur_off'",
      "'motion_blur_only'", "'aa_only'", "'anisotropic_only'",
      "'fov_only'", "'keyboard_mouse_only'", "'vsync'", "'vrr'",
      "'immediate'", "'combined_enhanced'"};
  for (const char* profile : required_profiles) {
    if (source.find(profile) == std::string::npos) {
      std::cerr << "autonomous matrix is missing profile " << profile << '\n';
      return 1;
    }
  }

  const bool one_process_at_a_time =
      source.find("Get-ActiveTitleProcesses") != std::string::npos &&
      source.find("Refusing to start a matrix while a title process is active") !=
          std::string::npos &&
      source.find("A competing title process appeared before matrix case") !=
          std::string::npos &&
      source.find("Stop-ControlledTitle $process") != std::string::npos;
  const bool preserves_evidence =
      source.find("executable_sha256") != std::string::npos &&
      source.find("gpu_sha256") != std::string::npos &&
      source.find("runtime_sha256") != std::string::npos &&
      source.find("configuration_sha256") != std::string::npos &&
      source.find("GRAPHICS_CACHE_IDENTITY") != std::string::npos &&
      source.find("REX_PC_SETTINGS_EFFECTIVE") != std::string::npos &&
      source.find("REX_HOST_PRESENT_EFFECTIVE") != std::string::npos &&
      source.find("capture_window.ps1") != std::string::npos &&
      source.find("evaluate-visual-regression.ps1") != std::string::npos &&
      source.find("pc_visual_references_v1.json") != std::string::npos &&
      source.find("visual_regression.json") != std::string::npos &&
      source.find("visual_regression_passed") != std::string::npos &&
      source.find("gpu_state_captures") != std::string::npos &&
      source.find("gpu_resource_captures") != std::string::npos &&
      source.find("Get-ProbeInputObservation $combinedText") !=
          std::string::npos &&
      source.find("guest_input_observations = $guestInputObservations") !=
          std::string::npos &&
      source.find("input_observation_scope") != std::string::npos &&
      source.find("rex_gameplay_gpu_state_*.log") != std::string::npos &&
      source.find("rex_active_texture_*.bin") != std::string::npos &&
      source.find("rex_first_full_resolve_source_fp16.bin") !=
          std::string::npos &&
      source.find("rex_level_gamma_pwl.bin") != std::string::npos &&
      source.find("rex_level_swap_*.bin") != std::string::npos &&
      source.find("rex_mixed_scale_*.bin") != std::string::npos &&
      source.find("rex_scene_chain_*.bin") != std::string::npos &&
      source.find("rex_scene_feedback_*.bin") != std::string::npos &&
      source.find("rex_scene_seed_*.bin") != std::string::npos &&
      source.find("rex_simple_title_*.bin") != std::string::npos &&
      source.find("rex_targeted_resolve_source_fp16.bin") !=
          std::string::npos &&
      source.find("rex_texture_resource_*.bin") != std::string::npos &&
      source.find(
          "REX_EMBEDDED_RESOLVE_SOURCE_CAPTURE_SCENE_COLOR=true") !=
          std::string::npos &&
      source.find(
          "REX_EMBEDDED_RESOLVE_SOURCE_CAPTURE_FIRST_FULL_FRAME=true") !=
          std::string::npos &&
      source.find(
          "REX_EMBEDDED_SCENE_SEED_DRAW_CAPTURE=true") !=
          std::string::npos &&
      source.find(
          "REX_EMBEDDED_MIXED_SCALE_TRANSITION_CAPTURE=true") !=
          std::string::npos &&
      source.find(
          "REX_EMBEDDED_SCENE_CHAIN_CAPTURE=true") !=
          std::string::npos &&
      source.find(
          "REX_EMBEDDED_SCENE_CHAIN_CHECKPOINT_CAPTURE=true") !=
          std::string::npos &&
      source.find("visual_required_matching_frames") != std::string::npos &&
      source.find("visual_invariant_failures") != std::string::npos &&
      source.find("matrix_summary.json") != std::string::npos;
  const bool controlled_shutdown =
      source.find("CloseMainWindow") != std::string::npos &&
      source.find("WaitForExit(10000)") != std::string::npos &&
      source.find("Stop-Process -Id $Process.Id -Force") != std::string::npos;
  const bool sealed_candidate_bisection =
      source.find("[switch]$SealedPackageOnly") != std::string::npos &&
      source.find("SealedPackageOnly = $SealedPackageOnly") !=
          std::string::npos &&
      source.find("sealed_package_only = [bool]$SealedPackageOnly") !=
          std::string::npos;
  const bool no_synthetic_input =
      source.find("SendKeys") == std::string::npos &&
      source.find("PostMessage") == std::string::npos &&
      source.find("keybd_event") == std::string::npos &&
      source.find("physical_input_injected = $false") != std::string::npos;

  if (!one_process_at_a_time || !preserves_evidence ||
      !sealed_candidate_bisection ||
      !controlled_shutdown || !no_synthetic_input) {
    std::cerr << "autonomous matrix safety/evidence policy regressed\n";
    return 1;
  }
  std::cout << "Autonomous matrix safety/evidence policy passed\n";
  return 0;
}
