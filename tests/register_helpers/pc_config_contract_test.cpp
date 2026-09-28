#include <toml++/toml.hpp>

#include "pc_settings_schema.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

const toml::node* FindDottedNode(const toml::table& root,
                                 std::string_view dotted_path) {
  const toml::table* table = &root;
  size_t start = 0;
  for (;;) {
    const size_t separator = dotted_path.find('.', start);
    const std::string_view component = dotted_path.substr(
        start, separator == std::string_view::npos
                   ? dotted_path.size() - start
                   : separator - start);
    const toml::node* node = table->get(component);
    if (!node || separator == std::string_view::npos) return node;
    table = node->as_table();
    if (!table) return nullptr;
    start = separator + 1;
  }
}

bool CheckCommon(const std::filesystem::path& path, bool expect_borderless = true) {
  const toml::table config = toml::parse_file(path.string());
  bool passed = true;
  passed &= Check(config["pc_config_version"].value_or(0) == 1,
                  path.string() + ": schema version is not 1");
  if (expect_borderless) {
    passed &= Check(config["fullscreen"].value_or(false),
                    path.string() + ": borderless-fullscreen startup is not enabled");
    passed &= Check(config["window_mode"].value_or(std::string{}) == "borderless",
                    path.string() + ": explicit borderless startup is not selected");
  } else {
    passed &= Check(!config["fullscreen"].value_or(true),
                    path.string() + ": oversized validation window unexpectedly requests fullscreen");
    passed &= Check(config["window_mode"].value_or(std::string{}) == "windowed",
                    path.string() + ": explicit windowed startup is not selected");
  }
  passed &= Check(config["present"]["letterbox"].value_or(false),
                  path.string() + ": original aspect is not preserved");
  passed &= Check(!config["present"]["allow_overscan_cutoff"].value_or(true),
                  path.string() + ": overscan cutoff is enabled");
  passed &= Check(config["present"]["safe_area_x"].value_or(0) == 100 &&
                      config["present"]["safe_area_y"].value_or(0) == 100,
                  path.string() + ": safe area is not the full Xbox image");
  passed &= Check(!config["input"]["mouse_invert_y"].value_or(true),
                   path.string() + ": optional mouse Y inversion is not off by default");
  passed &= Check(config["input"]["mouse_acceleration"].value_or(-1.0) == 0.0,
                  path.string() + ": optional mouse acceleration is not exact-off");
  passed &= Check(config["input"]["mouse_smoothing"].value_or(-1.0) == 0.0,
                  path.string() + ": optional mouse smoothing is not exact-off");
  passed &= Check(config["input"]["controller_sensitivity"].value_or(
                      std::string{}) == "medium",
                  path.string() + ": title-native controller sensitivity is not medium");
  passed &= Check(!config["input"]["controller_invert_y"].value_or(true),
                  path.string() + ": title-native controller Y inversion is not off");
  // Empty = the built-in patch of the title's composite shaders (no pack file).
  passed &= Check(config["graphics"]["motion_blur_off_shader_pack"].value_or(
                      std::string{"missing"}).empty(),
                   path.string() + ": motion-blur Off does not use the built-in patch");
  const std::string language = config["general"]["language"].value_or(std::string{});
  passed &= Check(language == "english" || language == "auto",
                  path.string() + ": language is neither English nor automatic");
  return passed;
}

bool CheckCompatibilityDefaults(const std::filesystem::path& path) {
  const toml::table config = toml::parse_file(path.string());
  bool passed = true;
  passed &= Check(config["camera"]["field_of_view"].value_or(0.0) == 86.0,
                  path.string() + ": original field of view changed");
  passed &= Check(!config["input"]["keyboard_mouse"].value_or(true),
                  path.string() + ": optional keyboard/mouse is not opt-in");
  passed &= Check(config["graphics"]["motion_blur"].value_or(false),
                  path.string() + ": title-native motion blur is not the default");
  passed &= Check(config["general"]["language"].value_or(std::string{}) == "english",
                  path.string() + ": compatibility language is not English");
  return passed;
}

// V320 enhanced-first: the packaged example configuration (the defaults a
// player without TheDarkness.pc.toml runs, and the settings reset target) and
// the Enhanced preset carry the recommended PC values.
bool CheckEnhancedDefaults(const std::filesystem::path& path) {
  const toml::table config = toml::parse_file(path.string());
  bool passed = true;
  // V330: Automatic internal scale (0: resolved per machine at startup).
  passed &= Check(config["output_resolution"].value_or(std::string{}) == "native" &&
                      config["resolution_scale"].value_or(-1) == 0 &&
                      config["draw_resolution_scale_threshold"].value_or(0) == 640 &&
                      !config["draw_resolution_scale_native_grid_rules"]
                           .value_or(std::string{})
                           .empty(),
                  path.string() + ": enhanced output/automatic internal scale policy changed");
  passed &= Check(config["display"]["menu_frame_rate"].value_or(std::string{}) == "reduced",
                  path.string() + ": enhanced menus are not at the reduced frame rate");
  // V361: SMAA 1x (same GPU cost as FXAA Extreme, without its blur).
  passed &= Check(config["swap_post_effect"].value_or(std::string{}) == "smaa" &&
                      config["anisotropic_override"].value_or(0) == 5,
                  path.string() + ": enhanced AA or 16x anisotropic policy changed");
  // FSR 1 only when upscaling: a 1:1 image stays the exact bilinear copy.
  passed &= Check(config["present"]["effect"].value_or(std::string{}) == "auto",
                  path.string() + ": enhanced upscaling is not automatic FSR 1");
  passed &= Check(!config["graphics"]["motion_blur"].value_or(true),
                  path.string() + ": enhanced motion blur is enabled");
  // V376: the field of view works now; every preset keeps today's view.
  passed &= Check(config["camera"]["field_of_view"].value_or(0.0) == 86.0,
                  path.string() + ": enhanced field of view is not the original 86");
  // VSync never tears (VRR displays still follow the frame rate); the "vrr"
  // mode presents without waiting and tears on fixed-refresh displays.
  passed &= Check(config["display"]["present_mode"].value_or(std::string{}) == "vsync" &&
                      config["display"]["max_frame_latency"].value_or(0) == 1 &&
                      config["display"]["frame_limit"].value_or(-1) == 0 &&
                      config["display"]["frame_rate"].value_or(std::string{}) == "refresh",
                  path.string() + ": enhanced presentation/frame-rate policy changed");
  passed &= Check(config["input"]["keyboard_mouse"].value_or(false) &&
                      config["input"]["mouse_look"].value_or(std::string{}) == "native" &&
                      config["input"]["mouse_sensitivity"].value_or(0.0) == 1.0,
                  path.string() + ": enhanced keyboard/native mouse look is not on");
  // Like the console's dashboard language (English when not shipped).
  passed &= Check(config["general"]["language"].value_or(std::string{}) == "auto",
                  path.string() + ": enhanced language does not follow Windows");
  return passed;
}

// V330 player quality levels besides Enhanced: native output (the output
// follows the monitor), the console's internal scale, the PC input.
bool CheckPlayerPreset(const std::filesystem::path& path) {
  const toml::table config = toml::parse_file(path.string());
  bool passed = true;
  passed &= Check(config["output_resolution"].value_or(std::string{}) == "native" &&
                      config["resolution_scale"].value_or(0) == 1,
                  path.string() + ": player preset must follow the monitor at internal 1x");
  passed &= Check(config["input"]["keyboard_mouse"].value_or(false) &&
                      config["input"]["mouse_look"].value_or(std::string{}) == "native" &&
                      config["general"]["language"].value_or(std::string{}) == "auto" &&
                      config["display"]["present_mode"].value_or(std::string{}) == "vsync" &&
                      config["display"]["menu_frame_rate"].value_or(std::string{}) == "reduced",
                  path.string() + ": player preset lost keyboard/native look, language, VSync or "
                                  "reduced menus");
  return passed;
}

bool CheckEnhancedValidationDefaults(const std::filesystem::path& path) {
  const toml::table config = toml::parse_file(path.string());
  bool passed = true;
  passed &= Check(config["camera"]["field_of_view"].value_or(0.0) == 86.0,
                  path.string() + ": enhanced validation field of view is not 86");
  passed &= Check(config["input"]["keyboard_mouse"].value_or(false),
                  path.string() + ": enhanced validation input bridge is disabled");
  passed &= Check(!config["graphics"]["motion_blur"].value_or(true),
                  path.string() + ": enhanced validation motion blur is enabled");
  passed &= Check(config["display"]["present_mode"].value_or(std::string{}) ==
                      "vrr" &&
                      config["display"]["max_frame_latency"].value_or(0) == 1 &&
                      config["display"]["frame_limit"].value_or(-1) == 0,
                  path.string() + ": enhanced VRR/fallback latency policy changed");
  passed &= Check(config["swap_post_effect"].value_or(std::string{}) ==
                      "fxaa_extreme" &&
                      config["anisotropic_override"].value_or(0) == 5,
                  path.string() + ": enhanced AA or 16x anisotropic policy changed");
  return passed;
}

}  // namespace

int main() {
  const std::filesystem::path root = DARKNESS_SOURCE_ROOT;
  const auto example = root / "config" / "pc_settings_v1.toml";
  const auto enhanced = root / "config" / "pc_presets" / "enhanced.toml";
  const auto original = root / "config" / "pc_presets" /
                        "original_720p.toml";
  const auto native_desktop = root / "config" / "pc_presets" /
                              "native_desktop_original.toml";
  const auto balanced_1080p = root / "config" / "pc_presets" /
                              "balanced_1080p.toml";
  const auto performance_1080p = root / "config" / "pc_presets" /
                                 "performance_1080p_fxaa.toml";
  const auto quality_1440p = root / "config" / "pc_presets" /
                             "quality_1440p.toml";
  const auto quality_1440p_internal_2x =
      root / "config" / "pc_phase1_validation" /
      "quality_1440p_internal_2x.toml";
  // The validation profiles belong to the development tree (not published).
  const bool has_validation = std::filesystem::exists(quality_1440p_internal_2x);
  const auto quality = root / "config" / "pc_presets" /
                       "quality_4k.toml";
  const auto steam_deck = root / "config" / "pc_presets" /
                          "steam_deck_800p_compatibility.toml";
  const auto performance = root / "config" / "pc_presets" / "performance.toml";
  const auto original_look = root / "config" / "pc_presets" / "original.toml";

  bool passed = CheckCommon(performance) && CheckCommon(original_look) &&
                CheckPlayerPreset(performance) && CheckPlayerPreset(original_look);
  {
    const toml::table performance_config = toml::parse_file(performance.string());
    passed &= Check(performance_config["swap_post_effect"].value_or(std::string{}) == "fxaa" &&
                        performance_config["display"]["frame_rate"].value_or(std::string{}) ==
                            "60" &&
                        !performance_config["graphics"]["motion_blur"].value_or(true) &&
                        performance_config["camera"]["field_of_view"].value_or(0.0) == 86.0 &&
                        performance_config["present"]["effect"].value_or(std::string{}) == "auto",
                    "Performance preset lost FXAA, the 60 FPS cap, blur off, the original view or "
                    "FSR 1 upscaling");
    const toml::table original_look_config = toml::parse_file(original_look.string());
    passed &= Check(original_look_config["swap_post_effect"].value_or(std::string{}) == "none" &&
                        original_look_config["anisotropic_override"].value_or(0) == 3 &&
                        original_look_config["graphics"]["motion_blur"].value_or(false) &&
                        original_look_config["display"]["frame_rate"].value_or(std::string{}) ==
                            "original" &&
                        original_look_config["camera"]["field_of_view"].value_or(0.0) == 86.0 &&
                        original_look_config["present"]["effect"].value_or(std::string{}) ==
                            "bilinear",
                    "Original preset lost the console look (no AA, 4x AF, blur, 30 FPS, 95, "
                    "bilinear)");
  }
  passed &= CheckCommon(example) && CheckCommon(enhanced) && CheckCommon(original) &&
                CheckCommon(native_desktop) &&
                CheckCommon(performance_1080p) &&
                 CheckCommon(balanced_1080p) && CheckCommon(quality_1440p) &&
                 (!has_validation || CheckCommon(quality_1440p_internal_2x)) &&
                 CheckCommon(quality, false) && CheckCommon(steam_deck) &&
                CheckEnhancedDefaults(example) &&
                CheckEnhancedDefaults(enhanced) &&
                CheckCompatibilityDefaults(original) &&
                CheckCompatibilityDefaults(native_desktop) &&
                CheckCompatibilityDefaults(performance_1080p) &&
                CheckCompatibilityDefaults(balanced_1080p) &&
                CheckCompatibilityDefaults(steam_deck) &&
                CheckEnhancedValidationDefaults(quality_1440p) &&
                (!has_validation || CheckEnhancedValidationDefaults(quality_1440p_internal_2x)) &&
                CheckEnhancedValidationDefaults(quality);
  const std::array<std::filesystem::path, 10> packaged_presets = {
      enhanced, performance, original_look, original, native_desktop, performance_1080p,
      balanced_1080p, quality_1440p, quality, steam_deck};
  for (const auto& preset : packaged_presets) {
    const toml::table config = toml::parse_file(preset.string());
    for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
      // Presets leave key bindings to the input driver's defaults, which the
      // example config (the reset target) lists below.
      if (setting.editor == PcSettingEditorKind::Key) continue;
      passed &= Check(
          FindDottedNode(config, setting.key) != nullptr,
          preset.string() + ": shared editable setting is absent: " +
              std::string(setting.key));
    }
  }
  const toml::table example_config = toml::parse_file(example.string());
  for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
    if (setting.editor != PcSettingEditorKind::Key) continue;
    const toml::node* node = FindDottedNode(example_config, setting.key);
    passed &= Check(node && node->value<std::string>(),
                    "Example config lacks key binding " + std::string(setting.key));
  }
  // The example (defaults, reset target) and the Enhanced preset agree on
  // every editable setting and on the title's internal-scale annotations,
  // which also match the validated internal-2x profile.
  const toml::table enhanced_config = toml::parse_file(enhanced.string());
  for (const PcEditableSettingSpec& setting : PcEditableSettingsSchema()) {
    if (setting.editor == PcSettingEditorKind::Key) continue;  // driver defaults
    const toml::node* left = FindDottedNode(example_config, setting.key);
    const toml::node* right = FindDottedNode(enhanced_config, setting.key);
    const auto text = [](const toml::node* node) {
      std::ostringstream out;
      if (node) node->visit([&out](const auto& value) { out << value; });
      return out.str();
    };
    passed &= Check(left && right && text(left) == text(right),
                    "Example config and Enhanced preset differ at " +
                        std::string(setting.key));
  }
  const toml::table validation_2x_config = toml::parse_file(
      (root / "config" / "pc_presets" / "validation_1440p_internal_2x.toml").string());
  for (const toml::table* config : {&example_config, &enhanced_config}) {
    passed &= Check(
        (*config)["draw_resolution_scale_native_grid_rules"].value_or(std::string{}) ==
            validation_2x_config["draw_resolution_scale_native_grid_rules"].value_or(
                std::string{"missing"}),
        "Enhanced native-grid annotations differ from the validated internal-2x profile");
  }
  constexpr std::array<std::string_view, 15> controller_buttons = {
      "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start",
      "back", "left_stick", "right_stick", "left_shoulder",
      "right_shoulder", "guide", "a", "b", "x", "y"};
  for (std::string_view button : controller_buttons) {
    const std::string name(button);
    passed &= Check(
        example_config["input"]["controller_bind"][name].value_or(
            std::string{}) == name,
        "Example config controller map is not identity for " + name);
  }
  const toml::table original_config = toml::parse_file(original.string());
  passed &= Check(original_config["output_resolution"].value_or(
                      std::string{}) == "720p" &&
                      original_config["resolution_scale"].value_or(0) == 1,
                  "Original preset no longer selects 720p at scale 1");
  const toml::table native_desktop_config =
      toml::parse_file(native_desktop.string());
  passed &= Check(native_desktop_config["output_resolution"].value_or(
                      std::string{}) == "native" &&
                      native_desktop_config["resolution_scale"].value_or(0) == 1,
                  "Native-desktop preset changed the original internal scale");
  const toml::table balanced_1080p_config =
      toml::parse_file(balanced_1080p.string());
  const toml::table performance_1080p_config =
      toml::parse_file(performance_1080p.string());
  passed &= Check(performance_1080p_config["output_resolution"].value_or(
                      std::string{}) == "1080p" &&
                      performance_1080p_config["resolution_scale"].value_or(0) == 1 &&
                      performance_1080p_config["swap_post_effect"].value_or(
                          std::string{}) == "fxaa",
                  "1080p performance preset no longer selects scale-1 FXAA");
  passed &= Check(balanced_1080p_config["output_resolution"].value_or(
                      std::string{}) == "1080p" &&
                      balanced_1080p_config["resolution_scale"].value_or(0) == 1,
                  "1080p preset must preserve the validated internal scale");
  const toml::table quality_1440p_config =
      toml::parse_file(quality_1440p.string());
  passed &= Check(quality_1440p_config["output_resolution"].value_or(
                      std::string{}) == "1440p" &&
                      quality_1440p_config["resolution_scale"].value_or(0) == 1,
                  "1440p output preset must preserve the validated internal scale");
  if (has_validation) {
    const toml::table quality_1440p_internal_2x_config =
        toml::parse_file(quality_1440p_internal_2x.string());
    passed &= Check(
        quality_1440p_internal_2x_config["output_resolution"].value_or(
            std::string{}) == "1440p" &&
            quality_1440p_internal_2x_config["resolution_scale"].value_or(0) == 2 &&
            quality_1440p_internal_2x_config["draw_resolution_scale_threshold"]
                    .value_or(0) == 640,
        "V154 1440p validation profile must select internal 2x with the evidenced 640-pixel threshold");
  }
  const toml::table quality_config = toml::parse_file(quality.string());
  passed &= Check(quality_config["output_resolution"].value_or(
                       std::string{}) == "4k" &&
                       quality_config["resolution_scale"].value_or(0) == 1 &&
                       quality_config["window_mode"].value_or(
                           std::string{}) == "windowed" &&
                       !quality_config["fullscreen"].value_or(true),
                   "4K output-path preset must use a real windowed 3840x2160 surface at scale 1");
  const toml::table steam_deck_config =
      toml::parse_file(steam_deck.string());
  passed &= Check(steam_deck_config["output_resolution"].value_or(
                      std::string{}) == "1280x800" &&
                      steam_deck_config["resolution_scale"].value_or(0) == 1 &&
                      steam_deck_config["display"]["present_mode"].value_or(
                          std::string{}) == "vsync" &&
                      steam_deck_config["display"]["frame_limit"].value_or(
                          -1) == 0,
                  "Steam Deck preset must preserve 720p-compatible timing on a contained 800p surface");
  if (passed) std::cout << "PC configuration contract passed\n";
  return passed ? 0 : 1;
}
