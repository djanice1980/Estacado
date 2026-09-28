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

bool Contains(const std::string& text, const char* value) {
  return text.find(value) != std::string::npos;
}

}  // namespace

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  const std::string compare =
      ReadFile(root + "/scripts/compare-visual-capture.ps1");
  const std::string evaluate =
      ReadFile(root + "/scripts/evaluate-visual-regression.ps1");
  const std::string detect =
      ReadFile(root + "/scripts/detect-visual-artifact.ps1");
  const std::string checkpoint =
      ReadFile(root + "/scripts/detect-visual-checkpoint.ps1");
  const std::string manifest =
      ReadFile(root + "/config/pc_visual_references_v1.json");
  if (compare.empty() || evaluate.empty() || detect.empty() ||
      checkpoint.empty() || manifest.empty()) {
    std::cerr << "visual regression policy inputs were not readable\n";
    return 1;
  }

  const bool animation_tolerant =
      Contains(compare, "[string[]]$ReferencePath") &&
      Contains(compare, "luma_correlation") &&
      Contains(compare, "edge_correlation") &&
      Contains(compare, "BrightMask") &&
      Contains(compare, "bright_mask_dice") &&
      Contains(compare, "histogram_l1") &&
      Contains(evaluate, "minimum_matching_frames") &&
      Contains(evaluate, "minimum_matching_fraction") &&
      Contains(evaluate, "required_matching_frames") &&
      Contains(evaluate, "$checkpointFrames") &&
      Contains(evaluate, "checkpoint_reached") &&
      Contains(evaluate, "out_of_phase_frames") &&
      Contains(manifest, "\"animated_sequence\": true");
  const bool catches_layer_failures =
      Contains(evaluate, "black_fraction_delta") &&
      Contains(evaluate, "saturated_red_fraction_delta") &&
      Contains(evaluate, "minimum_std_luma") &&
      Contains(evaluate, "maximum_mean_luma_delta") &&
      Contains(evaluate, "maximum_invariant_failures") &&
      Contains(evaluate, "invariant_failures") &&
      Contains(compare, "CropX") &&
      Contains(compare, "CropWidth") &&
      Contains(compare, "crop_normalized") &&
      Contains(evaluate, "region_failures") &&
      Contains(evaluate, "actual_crop_pixels") &&
      Contains(evaluate,
               "$checkpointFrames.Count * $minimumMatchingFraction") &&
      Contains(evaluate,
               "$matchingFrames -ge $requiredMatchingFrames") &&
      Contains(manifest, "enhanced_title_prompt_scale1");
  const bool preserves_authority =
      Contains(manifest,
               "LIVE_VALIDATED_ORIGINAL_TITLE_ATTRACTION_COMPOSITION") &&
      Contains(manifest, "LIVE_VALIDATED_ENHANCED_SCALE1_TITLE_PROMPT") &&
      Contains(manifest, "LIVE_VALIDATED_SCALE1_FRONTEND_MAIN_MENU") &&
      Contains(manifest, "LIVE_VALIDATED_FIRST_LEVEL_GAMEPLAY_ORIGINAL") &&
      Contains(manifest, "probe82_v80_original_prompt_sequence_*.png") &&
      Contains(manifest,
               "probe210_v134_main_menu_after_genuine_back.bmp") &&
      Contains(manifest, "probe148_v106_enhanced1440_preinput.bmp");
  const bool separates_frontend_screen_families =
      Contains(manifest, "original_frontend_menu") &&
      Contains(manifest, "original_frontend_options") &&
      Contains(manifest,
               "LIVE_CAPTURED_ORIGINAL_FRONTEND_OPTIONS_SUBMENU") &&
      Contains(manifest, "probe82_v80_original_post_start_menu.png");
  const bool separates_input_gate_from_fidelity =
      Contains(checkpoint, "checkpoint_reference_sets") &&
      Contains(checkpoint, "GATE_DETECTION_ONLY") &&
      Contains(checkpoint, "visual_fidelity_asserted = $false") &&
      Contains(checkpoint, "checkpoint_detected") &&
      Contains(checkpoint,
               "Refusing to overwrite visual-checkpoint evidence") &&
      Contains(manifest,
               "GATE_DETECTION_ONLY_VISUAL_FIDELITY_NOT_ASSERTED") &&
      Contains(manifest, "probe81_v79_press_start_gate_user.png");
  const bool detects_known_artifacts =
      Contains(detect, "artifact_reference_sets") &&
      Contains(detect, "artifact_detected") &&
      Contains(detect, "absence_proven = $false") &&
      Contains(detect, "Refusing to overwrite visual-artifact evidence") &&
      Contains(manifest, "KNOWN_BAD_SUMMONED_CREATURE_STRETCHED_SHEET") &&
      Contains(manifest, "stretched_translucent_sheet") &&
      Contains(manifest,
               "LIVE_VALIDATED_TITLE_PROMPT_COMPOSITION_SAME_STATE_ONLY") &&
      Contains(manifest,
               "KNOWN_BAD_FRONTEND_TITLE_MALFORMED_DARK_OVERLAY_LAYERS") &&
      Contains(manifest, "user_frontend_title_correct_20260904.png") &&
      Contains(manifest, "user_frontend_title_broken_20260831.png");
  const bool preserves_evidence =
      Contains(evaluate, "Refusing to overwrite visual-regression evidence") &&
      Contains(evaluate, "actual_sha256") &&
      Contains(evaluate, "best_reference_sha256") &&
      Contains(evaluate, "ConvertTo-Json -Depth 8");
  const bool no_synthetic_input =
      !Contains(compare, "SendKeys") && !Contains(evaluate, "SendKeys") &&
      !Contains(compare, "PostMessage") && !Contains(evaluate, "PostMessage") &&
      !Contains(detect, "SendKeys") && !Contains(detect, "PostMessage") &&
      !Contains(checkpoint, "SendKeys") &&
      !Contains(checkpoint, "PostMessage") &&
      !Contains(compare, "keybd_event") && !Contains(evaluate, "keybd_event") &&
      !Contains(detect, "keybd_event") &&
      !Contains(checkpoint, "keybd_event");

  if (!animation_tolerant || !catches_layer_failures ||
      !preserves_authority || !separates_frontend_screen_families ||
      !separates_input_gate_from_fidelity ||
      !detects_known_artifacts ||
      !preserves_evidence || !no_synthetic_input) {
    std::cerr << "visual regression evidence policy regressed\n";
    return 1;
  }
  std::cout << "Visual regression evidence policy passed\n";
  return 0;
}
