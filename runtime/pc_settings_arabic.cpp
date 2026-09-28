#include "pc_settings_arabic.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include <windows.h>

namespace {

struct Entry {
    std::string_view english;
    std::string_view arabic;
};

// Sorted by the English text (UTF-8 byte order = code point order).
constexpr Entry kEntries[] = {
#include "pc_settings_arabic_table.inc"
};

}  // namespace

std::string_view PcSettingsArabicText(std::string_view english) {
    const auto it = std::lower_bound(
        std::begin(kEntries), std::end(kEntries), english,
        [](const Entry& entry, std::string_view key) { return entry.english < key; });
    if (it == std::end(kEntries) || it->english != english) return {};
    return it->arabic;
}

const std::map<std::string, std::string, std::less<>>& PcSettingsArabicTable() {
    static const std::map<std::string, std::string, std::less<>> table = [] {
        std::map<std::string, std::string, std::less<>> out;
        for (const Entry& entry : kEntries) {
            out.emplace(std::string(entry.english), std::string(entry.arabic));
        }
        return out;
    }();
    return table;
}

bool PcSettingsInterfaceArabic(std::string_view languageValue) {
    if (languageValue == "arabic") return true;
    if (languageValue == "auto") return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ARABIC;
    return false;
}
