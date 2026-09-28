#pragma once

#include <string_view>
#include <string>
#include <vector>
#include <optional>

enum class PcSettingEditorKind {
    Boolean,
    Integer,
    IntegerChoice,
    Number,
    Choice,
    Resolution,
    Key,  // a key or mouse-button binding (rex::ui::settings::IsBindingKeyName)
};

struct PcSettingChoice {
    std::string_view value;
    std::wstring_view label;
    bool advanced{};  // offered in the advanced view only
};

struct PcEditableSettingSpec {
    std::string_view key;
    std::wstring_view section;
    std::wstring_view label;
    PcSettingEditorKind editor;
    double minimum{};
    double maximum{};
    double step{};
    std::vector<PcSettingChoice> choices;
    bool restartRequired{true};
    std::optional<double> defaultNumber;
    double displayMultiplier{1.0};
    std::wstring_view units;
};

// Version 1 is the shared startup-settings contract used by the runtime,
// native settings utility, and any future in-game settings surface. The
// runtime remains the authoritative validator and writer.
constexpr unsigned kPcEditableSettingsSchemaVersion = 1;

const std::vector<PcEditableSettingSpec>& PcEditableSettingsSchema();
const PcEditableSettingSpec* FindPcEditableSetting(std::string_view key);
std::vector<std::wstring_view> PcEditableSettingsSections();

// Presentation shared by the launcher and the in-game overlay: a plain
// description, an optional visibility condition (shown only while another
// setting has a given value), whether it belongs to the advanced view, and
// whether the in-game overlay applies it immediately (otherwise it is saved
// for the next start).
struct PcSettingPresentation {
    std::wstring_view description;
    std::string_view visibleWhenKey;
    std::string_view visibleWhenValue;
    bool advanced{};
    bool liveInGame{};
};
const PcSettingPresentation& PcSettingPresentationFor(std::string_view key);
// Numeric UI conversion never mutates runtime values or silently clamps invalid
// typed input. Slider steps and displayed ranges share the runtime schema.
std::optional<double> ParsePcDisplayedNumber(const PcEditableSettingSpec& spec,
                                            std::wstring_view text);
int PcSliderStepCount(const PcEditableSettingSpec& spec);
double PcSliderValue(const PcEditableSettingSpec& spec, int position);
int PcSliderPosition(const PcEditableSettingSpec& spec, double value);
std::wstring FormatPcDisplayedNumber(const PcEditableSettingSpec& spec, double value);
