#pragma once

// The in-game code page for Arabic language packs.
//
// Parts of the title's UI convert wide strings to 8-bit text by keeping the
// low byte of each character (main-menu captions, for example), so Arabic
// cannot reach the renderer as U+FExx. Arabic text is therefore drawn with
// fonts whose codes 0x80..0xFF (except 0xA7, the formatting-code marker)
// hold the Arabic contextual forms instead of the Latin-1 letters: the same
// code survives both the wide and the 8-bit paths.
//
// The table below is the single source for the runtime and the language-pack
// tools (scripts/arabic_game_fonts.py parses it): kForms[i] is drawn with
// code kCodes[i]. The pack fonts carry the Arabic glyphs at these codes.

#include <array>
#include <cstdint>

namespace darkness::language_codepage {

constexpr char16_t kSectionSign = 0x00A7;

// U+FE80..U+FEFC (all Arabic contextual forms and the lam-alef ligatures),
// then the Arabic question mark and comma.
constexpr std::array<char16_t, 127> kForms = [] {
    std::array<char16_t, 127> forms{};
    size_t i = 0;
    for (char16_t cp = 0xFE80; cp <= 0xFEFC; ++cp) forms[i++] = cp;
    forms[i++] = 0x061F;  // Arabic question mark
    forms[i++] = 0x060C;  // Arabic comma
    return forms;
}();

// 0x80..0xFF without the section sign.
constexpr std::array<char16_t, 127> kCodes = [] {
    std::array<char16_t, 127> codes{};
    size_t i = 0;
    for (char16_t code = 0x80; code <= 0xFF; ++code) {
        if (code != kSectionSign) codes[i++] = code;
    }
    return codes;
}();

// Arabic code point (a contextual form or punctuation) -> game code, or the
// input when it has none. Arabic semicolon and the rare alef maksura
// initial/medial forms fall back to their nearest drawable equivalents.
constexpr char16_t ToGameCode(char32_t cp) {
    if (cp >= 0xFE80 && cp <= 0xFEFC) return kCodes[size_t(cp - 0xFE80)];
    if (cp == 0x061F) return kCodes[125];
    if (cp == 0x060C) return kCodes[126];
    if (cp == 0x061B) return u';';
    if (cp == 0xFBE8) return kCodes[size_t(0xFEF3 - 0xFE80)];  // as yeh initial
    if (cp == 0xFBE9) return kCodes[size_t(0xFEF4 - 0xFE80)];  // as yeh medial
    return cp <= 0xFFFF ? char16_t(cp) : u'?';
}

// A game code that draws an Arabic glyph (right to left).
constexpr bool IsArabicGameCode(char16_t code) {
    return code >= 0x80 && code <= 0xFF && code != kSectionSign;
}

// Position of a game code in kCodes (the glyph kForms[i]), or -1.
constexpr int GameCodeIndex(char16_t code) {
    if (!IsArabicGameCode(code)) return -1;
    return int(code) - 0x80 - (code > kSectionSign ? 1 : 0);
}

}  // namespace darkness::language_codepage
