#include "runtime_graphics_cache.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
}  // namespace

int main() {
  bool passed = true;
  static constexpr std::array<uint8_t, 3> kAbc = {'a', 'b', 'c'};
  passed &= Check(
      RuntimeSha256(kAbc.data(), kAbc.size()) ==
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
      "SHA-256 implementation must match the standard abc vector");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "darkness_runtime_graphics_cache_test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  const std::filesystem::path settings = root / "settings.toml";
  {
    std::ofstream stream(settings, std::ios::binary);
    stream << "pc_config_version = 1\nresolution_scale = 1\n";
  }
  RuntimeGraphicsCacheCompatibilityInputs compatibility{};
  compatibility.backend = "d3d12";

  const auto first = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, {}, compatibility);
  const auto repeated = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, {}, compatibility);
  passed &= Check(first.root == repeated.root,
                  "identical title inputs must reuse one cache root");
  passed &= Check(first.root.parent_path().filename() ==
                      first.executableSha256,
                  "cache root must contain the exact XEX identity");
  passed &= Check(first.root.filename() == first.compatibilitySha256 &&
                      first.compatibilitySha256.size() == 64,
                  "cache root must use one compact compatibility identity");
  passed &= Check(first.backend == "d3d12" && !first.settingsSha256.empty(),
                  "cache identity must record the backend and settings provenance");
  passed &= Check(first.root.parent_path().parent_path().filename() ==
                      "545407E0",
                  "cache root must contain the title identifier");

  const std::vector<RuntimeEnvironmentOverride> firstOverrides{
      {"rex_resolution_scale", "2"},
      {"REX_D3D12_BINDLESS", "false"},
  };
  const std::vector<RuntimeEnvironmentOverride> reorderedOverrides{
      {"REX_D3D12_BINDLESS", "false"},
      {"REX_RESOLUTION_SCALE", "2"},
  };
  const auto overridden = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, firstOverrides, compatibility);
  const auto reordered = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, reorderedOverrides, compatibility);
  passed &= Check(overridden.root == reordered.root,
                  "environment identity must ignore assignment order and name case");
  passed &= Check(overridden.root != first.root &&
                      overridden.environmentOverrideCount == 2 &&
                      !overridden.environmentSha256.empty(),
                  "cache-relevant REX overrides (test launches) must select a separate cache root");
  passed &= Check(overridden.root.parent_path().filename() ==
                          overridden.executableSha256 &&
                      overridden.root.filename().string().size() == 64,
                  "environment identity must use one compact combined SHA leaf");

  const auto changedOverride = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, {{"REX_RESOLUTION_SCALE", "3"}}, compatibility);
  passed &= Check(changedOverride.root != overridden.root,
                  "changed REX override value must select its own cache root");

  const auto diagnosticsOnly = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(), settings, {},
      {{"REX_DIAGNOSTICS_CAMERA_STATE", "true"},
       {"REX_DISPLAY_PRESENT_DIAGNOSTICS", "true"},
       {"REX_DISPLAY_PRESENT_STALL_THRESHOLD_MS", "75"},
       {"REX_EMBEDDED_GAMEPLAY_CAPTURE_COUNT", "6"},
       {"REX_EMBEDDED_GAMEPLAY_CAPTURE_INTERVAL_SWAPS", "600"},
       {"REX_EMBEDDED_GAMEPLAY_CAPTURE_START_SWAP", "900"},
       {"REX_EMBEDDED_HITCH_DIAGNOSTICS", "true"},
       {"REX_EMBEDDED_CP_CADENCE_DIAGNOSTICS", "true"},
       {"REX_EMBEDDED_HITCH_TRACE_THRESHOLD_MS", "75"},
       {"REX_LOG_LEVEL", "debug"}}, compatibility);
  passed &= Check(diagnosticsOnly.root == first.root &&
                      diagnosticsOnly.environmentSha256.empty() &&
                      diagnosticsOnly.environmentOverrideCount == 0,
                  "observation-only diagnostics must not force a cold graphics cache");

  // V290 identity v3: the stored records (guest microcode and pipeline
  // descriptions) are settings- and build-independent and are revalidated,
  // retranslated and recreated at every load, so a settings change or a new
  // runtime/GPU build keeps the warm store.
  {
    std::ofstream stream(settings, std::ios::binary | std::ios::trunc);
    stream << "pc_config_version = 1\nresolution_scale = 2\n";
  }
  const auto changedSettings = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbc.data(), kAbc.size(),
      settings, {}, {}, compatibility);
  passed &= Check(changedSettings.root == first.root &&
                      changedSettings.settingsSha256 != first.settingsSha256,
                  "changed PC settings must keep the warm store (provenance still changes)");

  static constexpr std::array<uint8_t, 4> kAbcd = {'a', 'b', 'c', 'd'};
  const auto changedExecutable = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbcd.data(), kAbcd.size(),
      settings, {}, {}, compatibility);
  passed &= Check(changedExecutable.root != changedSettings.root,
                  "changed XEX contents must not reuse another title build's store");

  auto changedBackendCompatibility = compatibility;
  changedBackendCompatibility.backend = "vulkan";
  const auto changedBackend = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbcd.data(), kAbcd.size(),
      settings, {}, {}, changedBackendCompatibility);
  passed &= Check(changedBackend.root != changedExecutable.root,
                  "changed graphics backend must not reuse incompatible cache files");

  const std::filesystem::path mods_manifest = root / "TheDarkness.mods.toml";
  const std::filesystem::path mod_root = root / "mods" / "example";
  std::filesystem::create_directories(mod_root, error);
  {
    std::ofstream stream(mods_manifest, std::ios::binary);
    stream << "mods_config_version = 1\nenabled = true\n";
  }
  const std::filesystem::path mod_file = mod_root / "Content.bin";
  {
    std::ofstream stream(mod_file, std::ios::binary);
    stream << "mod-v1";
  }
  auto moddedCompatibility = compatibility;
  moddedCompatibility.modsManifest = mods_manifest;
  moddedCompatibility.modTrees.push_back({"example", mod_root});
  const auto modded = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbcd.data(), kAbcd.size(),
      settings, {}, {}, moddedCompatibility);
  passed &= Check(modded.root != changedExecutable.root &&
                      !modded.modsSha256.empty() &&
                      modded.modFileCount == 1,
                  "enabled mod contents must select a separate cache root");
  {
    std::ofstream stream(mod_file, std::ios::binary | std::ios::trunc);
    stream << "mod-v2";
  }
  const auto changedMod = BuildRuntimeGraphicsCacheIdentity(
      root / "cache", 0x545407E0u, kAbcd.data(), kAbcd.size(),
      settings, {}, {}, moddedCompatibility);
  passed &= Check(changedMod.root != modded.root,
                  "changed loose mod contents must not reuse incompatible cache files");

  std::filesystem::remove_all(root, error);
  if (passed) std::cout << "Runtime graphics cache identity tests passed\n";
  return passed ? 0 : 1;
}
