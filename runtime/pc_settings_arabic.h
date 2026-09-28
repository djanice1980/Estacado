#pragma once

// Arabic interface (launcher, F1 overlay, settings panel): our own texts in
// Arabic, drawn right-to-left by ReXGlue's settings panel (rex/ui/rtl_text.h).
// Source: scripts/arabic_ui_translations.py (generated table).

#include <map>
#include <string>
#include <string_view>

// The Arabic text for an English interface text; empty when there is none
// (technical names such as FXAA or 1280 x 720 stay as they are).
std::string_view PcSettingsArabicText(std::string_view english);

// English text -> Arabic for every entry (the panel's own words travel in the
// settings schema).
const std::map<std::string, std::string, std::less<>>& PcSettingsArabicTable();

// Whether the interface is Arabic for a general.language value: "arabic", or
// "auto" on a Windows display language that is Arabic.
bool PcSettingsInterfaceArabic(std::string_view languageValue);
