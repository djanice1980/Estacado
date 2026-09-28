#include "runtime_mods.h"
#include "runtime_xbox_path_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  if (!output) throw std::runtime_error("unable to write test fixture");
}

std::string ReadGuestFile(GuestFileSystem& filesystem,
                          const std::string& path) {
  const uint32_t handle = filesystem.OpenGameFile(path, true);
  if (!handle) return {};
  std::vector<uint8_t> bytes(256);
  uint32_t bytesRead{};
  if (!filesystem.Read(handle, 0, bytes.data(),
                       static_cast<uint32_t>(bytes.size()), &bytesRead) ||
      !filesystem.Close(handle)) {
    return {};
  }
  return std::string(bytes.begin(), bytes.begin() + bytesRead);
}

template <typename Callback>
bool Throws(Callback&& callback) {
  try {
    callback();
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}
}  // namespace

int main() {
  bool passed = true;
  if (const char* fixture = std::getenv("DARKNESS_MOD_LINK_FIXTURE")) {
    const std::filesystem::path fixtureRoot(fixture);
    passed &= Check(Throws([&] {
      LoadRuntimeModConfiguration(fixtureRoot / "TheDarkness.mods.toml", fixtureRoot);
    }), "explicit linked-root fixture must fail closed");
  }
  {
    const std::vector<std::wstring> components = {L"Content", L"Audio"};
    const auto match = darkness::xbox_path::SelectUniqueComponent(
        components, L"content");
    passed &= Check(match.found && !match.ambiguous && match.index == 0,
                    "Xbox path component matching must ignore ASCII case");
    const auto missing = darkness::xbox_path::SelectUniqueComponent(
        components, L"Textures");
    passed &= Check(!missing.found && !missing.ambiguous,
                    "a missing Xbox path component was fabricated");
    const std::vector<std::wstring> colliding = {L"Content", L"CONTENT"};
    const auto ambiguous = darkness::xbox_path::SelectUniqueComponent(
        colliding, L"content");
    passed &= Check(ambiguous.found && ambiguous.ambiguous,
                    "case-colliding host entries must fail closed");
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("darkness_mods_" + std::to_string(nonce));
  const auto manifest = root / "TheDarkness.mods.toml";
  const auto mods = root / "mods";
  const auto game = root / "game";

  std::error_code error;
  std::filesystem::remove_all(root, error);
  WriteText(game / "Content/shared.bin", "base");
  WriteText(game / "Content/base_only.bin", "base-only");
  WriteText(mods / "low/Content/shared.bin", "low");
  WriteText(mods / "low/Content/low_only.bin", "low-only");
  WriteText(mods / "high/Content/shared.bin", "high");
  WriteText(mods / "high/Content/high_only.bin", "high-only");

  const auto missing = LoadRuntimeModConfiguration(root / "missing.toml", root);
  passed &= Check(!missing.manifestPresent && !missing.enabled &&
                      missing.layers.empty(),
                  "a missing manifest must leave overrides disabled");

  WriteText(manifest,
            "mods_config_version = 1\n"
            "enabled = false\n");
  const auto disabled = LoadRuntimeModConfiguration(manifest, root);
  passed &= Check(disabled.manifestPresent && !disabled.enabled &&
                      disabled.layers.empty(),
                  "an explicitly disabled manifest must not activate layers");

  WriteText(manifest,
            "mods_config_version = 1\n"
            "enabled = true\n"
            "[[mod]]\n"
            "id = \"low\"\n"
            "path = \"low\"\n"
            "priority = 10\n"
            "[[mod]]\n"
            "id = \"high\"\n"
            "path = \"high\"\n"
            "priority = 100\n");
  const auto configuration = LoadRuntimeModConfiguration(manifest, root);
  passed &= Check(configuration.enabled && configuration.layers.size() == 2 &&
                      configuration.layers[0].id == "high" &&
                      configuration.layers[1].id == "low",
                  "layers must be ordered by deterministic priority");
  passed &= Check(configuration.conflicts.size() == 1 &&
                      configuration.conflicts[0].winnerId == "high" &&
                      configuration.conflicts[0].shadowedId == "low" &&
                      configuration.conflicts[0].relativePath.generic_string() ==
                          "Content/shared.bin",
                  "the exact winning and shadowed content conflict must be reported");
  const auto validation_lines = RuntimeModConfigurationLines(configuration);
  passed &= Check(std::find(validation_lines.begin(), validation_lines.end(),
                            "MOD_CONFIG_VALID=1") != validation_lines.end() &&
                      std::find(validation_lines.begin(), validation_lines.end(),
                                "MOD_CONFIG_ENABLED=1") != validation_lines.end() &&
                      std::find(validation_lines.begin(), validation_lines.end(),
                                "MOD_LAYER_COUNT=2") != validation_lines.end() &&
                      std::find(validation_lines.begin(), validation_lines.end(),
                                "MOD_CONFLICT=Content/shared.bin|high|low") !=
                          validation_lines.end(),
                  "mod validation output must expose stable layer and conflict state");

  const auto invalid_lines = RuntimeModValidationErrorLines(
      manifest, "bad manifest\r\nsecond line");
  passed &= Check(std::find(invalid_lines.begin(), invalid_lines.end(),
                            "MOD_CONFIG_VALID=0") != invalid_lines.end() &&
                      std::find(invalid_lines.begin(), invalid_lines.end(),
                                "MOD_CONFIG_ERROR=bad manifest  second line") !=
                          invalid_lines.end(),
                  "mod validation errors must be machine-readable single lines");

  auto& filesystem = GetGuestFileSystem();
  filesystem.ResetForTests();
  filesystem.SetGameRoot(game);
  filesystem.SetContentOverlays(configuration.layers);
  passed &= Check(ReadGuestFile(filesystem, "D:\\Content\\shared.bin") == "high",
                  "highest-priority content must win");
  passed &= Check(ReadGuestFile(filesystem, "d:\\content\\SHARED.BIN") == "high" &&
                      ReadGuestFile(filesystem, "D:\\CONTENT\\BASE_ONLY.BIN") ==
                          "base-only",
                  "title and overlay lookup must preserve Xbox case-insensitivity");
  passed &= Check(ReadGuestFile(filesystem, "D:\\Content\\low_only.bin") ==
                      "low-only" &&
                      ReadGuestFile(filesystem, "D:\\Content\\base_only.bin") ==
                          "base-only",
                  "non-conflicting mod and base content must remain visible");

  const uint32_t directory = filesystem.OpenGameDirectory("d:\\content");
  std::set<std::string> names;
  GuestDirectoryEntry entry{};
  bool restart = true;
  for (;;) {
    const auto result = filesystem.QueryDirectory(
        directory, restart ? "*" : std::string{}, restart, &entry);
    restart = false;
    if (result == GuestDirectoryQueryResult::NoMoreFiles) break;
    if (result != GuestDirectoryQueryResult::Success) {
      passed = false;
      break;
    }
    names.insert(entry.name);
  }
  passed &= Check(filesystem.Close(directory) && names.size() == 4 &&
                      names.count("shared.bin") == 1 &&
                      names.count("high_only.bin") == 1 &&
                      names.count("low_only.bin") == 1 &&
                      names.count("base_only.bin") == 1,
                  "directory enumeration must merge layers without duplicates");

  const auto save = root / "save";
  WriteText(save / "slot.bin", "save-data");
  passed &= Check(filesystem.RegisterHostMount("savedrive:", save) &&
                      ReadGuestFile(filesystem, "savedrive:\\slot.bin") ==
                          "save-data",
                  "portable writable mounts must bypass title-content overlays");
  WriteText(save / "Nested/MixedCase.bin", "mixed-save");
  passed &= Check(ReadGuestFile(filesystem,
                                "SAVEDRIVE:\\nested\\mixedcase.BIN") ==
                      "mixed-save",
                  "portable mounts must retain Xbox case-insensitive reads");
  const auto created = filesystem.CreateOrOpenGameFile(
      "savedrive:\\nested\\new.bin", true, 2, true);
  passed &= Check(created.status == GuestFileOpenStatus::Success &&
                      created.handle != 0 && filesystem.Close(created.handle) &&
                      std::filesystem::is_regular_file(save / "Nested/new.bin"),
                  "portable creation must resolve an existing parent without case sensitivity");

  WriteText(manifest,
            "mods_config_version = 1\n"
            "enabled = true\n"
            "[[mod]]\n"
            "id = \"escape\"\n"
            "path = \"../outside\"\n");
  passed &= Check(Throws([&] {
                    LoadRuntimeModConfiguration(manifest, root);
                  }),
                  "a layer path escaping the mods root must fail closed");

  WriteText(manifest,
            "mods_config_version = 1\n"
            "enabled = true\n"
            "[[mod]]\n"
            "id = \"same\"\n"
            "path = \"low\"\n"
            "[[mod]]\n"
            "id = \"SAME\"\n"
            "path = \"high\"\n");
  passed &= Check(Throws([&] {
                    LoadRuntimeModConfiguration(manifest, root);
                  }),
                  "case-insensitive duplicate mod IDs must fail closed");

  WriteText(manifest,
            "mods_config_version = 1\n"
            "enabled = \"yes\"\n");
  passed &= Check(Throws([&] {
                    LoadRuntimeModConfiguration(manifest, root);
                  }),
                  "invalid activation types must fail closed");

  const auto dependencyManifest = [](const std::string& requirement,
                                      const std::string& provider = "enabled = true\n",
                                      int version = 2) {
    return "mods_config_version = " + std::to_string(version) +
        "\nenabled = true\n[[mod]]\nid = \"high\"\npath = \"high\"\npriority = 100\n" +
        requirement + "\n[[mod]]\nid = \"low\"\npath = \"low\"\npriority = 10\n" + provider;
  };
  WriteText(manifest, dependencyManifest("requires = [\"LOW\"]"));
  const auto dependent = LoadRuntimeModConfiguration(manifest, root);
  passed &= Check(dependent.layers[0].id == "high" && dependent.requirements.size() == 1 &&
      dependent.requirements[0].requiredId == "low",
      "forward case-insensitive requirement must preserve explicit priority");
  const auto dependentLines = RuntimeModConfigurationLines(dependent);
  passed &= Check(std::find(dependentLines.begin(), dependentLines.end(),
      "MOD_REQUIREMENT=high|low") != dependentLines.end(),
      "startup validator must report actual requirements");
  for (const auto* invalid : {"requires = [\"missing\"]", "requires = [\"HIGH\"]",
      "requires = [\"low\", \"LOW\"]", "requires = 1", "requires = [1]",
      "requires = [\"../low\"]"}) {
    WriteText(manifest, dependencyManifest(invalid));
    passed &= Check(Throws([&] { LoadRuntimeModConfiguration(manifest, root); }),
                    std::string("invalid requirement must fail: ") + invalid);
  }
  WriteText(manifest, dependencyManifest("requires = [\"low\"]", "enabled = false\n"));
  passed &= Check(Throws([&] { LoadRuntimeModConfiguration(manifest, root); }),
                  "disabled provider cannot satisfy a requirement");
  WriteText(manifest, dependencyManifest("requires = [\"low\"]", "enabled = true\n", 1));
  passed &= Check(Throws([&] { LoadRuntimeModConfiguration(manifest, root); }),
                  "new semantics cannot silently activate under schema 1");
  WriteText(manifest, dependencyManifest("requires = [\"low\"]",
                                       "requires = [\"high\"]\n"));
  passed &= Check(LoadRuntimeModConfiguration(manifest, root).requirements.size() == 2,
                  "mutual data requirements do not invent an initialization order");

  // Root/intermediate links must not disappear during canonicalization.
  // Targets are synthetic fixtures; never use original title content.
  std::filesystem::create_directory_symlink(mods / "high", mods / "alias", error);
  if (error) {
    std::cout << "SKIP: directory symlink fixture unavailable: " << error.message() << '\n';
    error.clear();
  } else {
    for (const auto* linkedPath : {"alias", "alias/Content"}) {
      WriteText(manifest, std::string("mods_config_version = 1\nenabled = true\n") +
          "[[mod]]\nid = \"linked\"\npath = \"" + linkedPath + "\"\n");
      passed &= Check(Throws([&] { LoadRuntimeModConfiguration(manifest, root); }),
                      std::string("linked layer component must be rejected: ") + linkedPath);
    }
    std::filesystem::remove(mods / "alias", error);
    if (error) throw std::runtime_error("unable to remove test link");
  }
  filesystem.ResetForTests();
  std::filesystem::remove_all(root, error);
  if (passed) std::cout << "Runtime loose-content mod tests passed\n";
  return passed ? 0 : 1;
}
