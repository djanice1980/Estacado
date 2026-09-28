#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string root = DARKNESS_SOURCE_ROOT;
  std::ifstream source_file(
      root + "/scripts/start-pc-phase1-validation-probe.ps1",
      std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  if (source.empty()) {
    std::cerr << "validation launcher was not readable\n";
    return 1;
  }

  const size_t enumerate =
      source.find("GetEnvironmentVariables('Process')");
  const size_t filter = source.find("StartsWith('REX_'", enumerate);
  const size_t clear = source.find(
      "Remove-Item -LiteralPath (\"Env:\\{0}\" -f $name)", filter);
  const size_t set_controlled = source.find(
      "$env:REX_EMBEDDED_GAMEPLAY_CAPTURE_START_SWAP", clear);
  const size_t start = source.find("Start-Process -FilePath $exe", set_controlled);
  const size_t validate_only = source.find("if ($ValidateOnly)");
  const size_t finally_block = source.find("} finally {", start);
  const size_t restore_clear = source.find(
      "Remove-Item -LiteralPath (\"Env:\\{0}\" -f $name)",
      finally_block);
  const size_t restore = source.find(
      "SetEnvironmentVariable(\n                $name, [string]$savedEnvironment[$name], 'Process')",
      finally_block);
  const bool reports_sanitized_names =
      source.find("SanitizedInheritedRexEnvironment") != std::string::npos;
  const bool manifest_owns_candidate =
      source.find("ConvertFrom-Json") != std::string::npos &&
      source.find("$manifest.candidate") != std::string::npos &&
      source.find("-ManifestPath $ManifestPath") != std::string::npos &&
      source.find("build/runtime_pc_phase1_candidate_v") == std::string::npos;
  const bool preset_selection_is_bounded =
      source.find("[ValidateSet(\n        'original_720p'") !=
          std::string::npos &&
      source.find("'steam_deck_800p_compatibility')]") !=
          std::string::npos &&
      source.find("Join-Path $candidate (\"presets/{0}.toml\" -f $PresetName)") !=
          std::string::npos &&
      source.find("'output_only_1440p'") != std::string::npos &&
      source.find("'quality_1440p_internal_2x'") != std::string::npos &&
      source.find("'internal_only_2x_720p'") != std::string::npos &&
      source.find("'aa_only_720p'") != std::string::npos &&
      source.find("'anisotropic_only_720p'") != std::string::npos &&
      source.find("'fov_only_720p'") != std::string::npos &&
      source.find("'keyboard_mouse_only_720p'") != std::string::npos &&
      source.find("'pc_input_fov_1080p')]") != std::string::npos &&
      source.find("config/pc_phase1_validation/{0}.toml") !=
          std::string::npos &&
      source.find("$manifest.validation_profiles.PSObject.Properties") !=
          std::string::npos &&
      source.find("Validation configuration hash mismatch") !=
          std::string::npos &&
      source.find("& $exe --validate-config --pc-config $pcConfig") !=
          std::string::npos &&
      source.find("$configValidationOutput -match 'HOST_START'") !=
          std::string::npos &&
      source.find("$argumentParts += '--input-diagnostics'") !=
          std::string::npos;
  const bool diagnostics_profiles_are_bounded =
      source.find("[ValidateSet('full', 'light', 'validation', 'visual', 'input', 'scale')]") !=
          std::string::npos &&
      source.find("$enableGameplayCapture = $DiagnosticsProfile -in @('full', 'scale')") !=
          std::string::npos &&
      source.find("$enableInputDiagnostics = $DiagnosticsProfile -in @('full', 'input', 'validation')") !=
          std::string::npos &&
      source.find("$enableCadenceDiagnostics = $DiagnosticsProfile -in @('full', 'light', 'validation')") !=
          std::string::npos &&
      source.find("$enableAudioDiagnostics = $DiagnosticsProfile -in @('full', 'light', 'validation')") !=
          std::string::npos &&
      source.find("$env:REX_EMBEDDED_MANUAL_GAMEPLAY_CAPTURE = 'true'") !=
          std::string::npos &&
      source.find("$evidenceCase") != std::string::npos;
  const bool startup_snapshot_is_explicit =
      source.find("& $exe --print-capabilities") != std::string::npos &&
      source.find("ProfileClassification") != std::string::npos &&
      source.find("HostDisplayMode") != std::string::npos &&
      source.find("HOST_144HZ_DOES_NOT_CLAIM_144FPS_SIMULATION") !=
          std::string::npos &&
      source.find("PC_ENHANCED_3840X2160_HIGH_DPI_BACKING_SURFACE_DOWNSAMPLED_TO_2560X1440_PHYSICAL_ORIGINAL_720P_INTERNAL") !=
          std::string::npos &&
      source.find("PC_ENHANCED_2560X1440_INTERNAL_2X_AUTONOMOUS_PASS_PHYSICAL_VALIDATION_PENDING") !=
          std::string::npos;
  const bool sealed_package_mode_is_explicit =
      source.find("[switch]$SealedPackageOnly") != std::string::npos &&
      source.find("-SealedPackageOnly:$SealedPackageOnly") !=
          std::string::npos &&
      source.find("'SEALED_PACKAGE_ONLY'") != std::string::npos &&
      source.find("VerificationMode") != std::string::npos;
  const bool passed = enumerate != std::string::npos &&
                      filter != std::string::npos &&
                      clear != std::string::npos &&
                      set_controlled != std::string::npos &&
                      start != std::string::npos &&
                      finally_block != std::string::npos &&
                      restore != std::string::npos &&
                      enumerate < filter && filter < clear &&
                      clear < set_controlled && set_controlled < start &&
                      validate_only != std::string::npos &&
                      validate_only < start &&
                      start < finally_block && finally_block < restore &&
                      restore_clear != std::string::npos &&
                      finally_block < restore_clear && restore_clear < restore &&
                      reports_sanitized_names && manifest_owns_candidate &&
                      preset_selection_is_bounded &&
                      diagnostics_profiles_are_bounded &&
                      startup_snapshot_is_explicit &&
                      sealed_package_mode_is_explicit;
  if (!passed) {
    std::cerr << "validation launcher no longer sanitizes and restores inherited REX_* overrides\n";
    return 1;
  }

  std::cout << "Validation launcher environment policy passed\n";
  return 0;
}
