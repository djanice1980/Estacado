#pragma once

// In-game Arabic text (language packs): text preparation and line order for
// the title's font renderer. Header-only so the policy test can check it.
//
// The title draws UTF-16 text strictly left to right, one glyph per code unit
// (font Write loop sub_8238BD10), after its own word wrap. Arabic therefore
// reaches the renderer already shaped (contextual forms, done once when the
// pack loads) and in the game code page (runtime_language_codepage.h: codes
// 0x80..0xFF, which survive the title's 8-bit conversions), and each drawn
// line is put into visual order just before it is drawn.
//
// Formatting codes: U+00A7 followed by a letter, with the lengths of the
// title's code parser sub_8238A380 (A/a 3, C/c 5, D/d 2, N/n 5, Q/q 10,
// T/t 5, X/x 5, Y/y 5, Z/z 4 code units; anything else is not a code).

#include "runtime_language_codepage.h"

#include <rex/ui/rtl_text.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace darkness::language_text {

constexpr char16_t kCodeMarker = language_codepage::kSectionSign;

// Length of the formatting code starting at text[index] (0 = not a code).
inline size_t CodeLength(std::u16string_view text, size_t index) {
    if (index + 1 >= text.size() || text[index] != kCodeMarker) return 0;
    // The title sign-extends the low byte of the letter.
    const int letter = int(int8_t(uint8_t(text[index + 1] & 0xFF))) - 'A';
    size_t length = 0;
    switch (letter) {
        case 'A' - 'A': case 'a' - 'A': length = 3; break;
        case 'C' - 'A': case 'c' - 'A': length = 5; break;
        case 'D' - 'A': case 'd' - 'A': length = 2; break;
        case 'N' - 'A': case 'n' - 'A': length = 5; break;
        case 'Q' - 'A': case 'q' - 'A': length = 10; break;
        case 'T' - 'A': case 't' - 'A': length = 5; break;
        case 'X' - 'A': case 'x' - 'A': length = 5; break;
        case 'Y' - 'A': case 'y' - 'A': length = 5; break;
        case 'Z' - 'A': case 'z' - 'A': length = 4; break;
        default: return 0;
    }
    return index + length <= text.size() ? length : text.size() - index;
}

// Right-to-left in a drawn line: Arabic game codes (and Arabic that did not
// go through the code page).
inline bool IsRtl(char16_t unit) {
    return language_codepage::IsArabicGameCode(unit) || rex::ui::rtl::IsRtlCodePoint(unit);
}

// Strong left to right: ASCII letters and digits, Latin beyond Latin-1.
inline bool IsLtr(char16_t unit) {
    if (language_codepage::IsArabicGameCode(unit)) return false;
    return rex::ui::rtl::IsLtrCodePoint(unit);
}

inline bool ContainsRtl(std::u16string_view text) {
    for (char16_t unit : text) {
        if (IsRtl(unit)) return true;
    }
    return false;
}

// Harakat and other combining marks: the game fonts carry no mark glyphs
// (and the renderer has no mark positioning), so pack text drops them.
inline bool IsDroppedMark(char32_t cp) {
    return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06ED);
}

// Pack text (UTF-8, logical order) -> game text in logical order: Arabic
// letters in their contextual forms, mapped to the game code page. Code
// points outside the Basic Multilingual Plane become '?'.
inline std::u16string PrepareText(std::string_view utf8) {
    std::u32string logical;
    for (char32_t cp : rex::ui::rtl::DecodeUtf8(utf8)) {
        if (!IsDroppedMark(cp)) logical.push_back(cp);
    }
    const std::u32string shaped = rex::ui::rtl::ShapeArabic(logical);
    std::u16string out;
    out.reserve(shaped.size());
    for (char32_t cp : shaped) out.push_back(language_codepage::ToGameCode(cp));
    return out;
}

// One drawn line -> visual (left-to-right drawing) order for a right-to-left
// line. Left-to-right runs (numbers, Latin words and the neutrals between
// them) keep their order; the rest is reversed with ASCII brackets mirrored.
// Formatting codes before the first drawn character stay in front; a code in
// the middle of the line stays with the character that follows it.
// Lines without right-to-left letters are returned unchanged.
inline std::u16string VisualLine(std::u16string_view line) {
    if (!ContainsRtl(line)) return std::u16string(line);
    struct Unit {
        std::u16string codes;  // formatting codes attached before this character
        char16_t character;
        uint8_t kind;  // 1 left-to-right, 2 right-to-left, 0 neutral
    };
    std::u16string prefix;
    std::u16string pending;
    std::vector<Unit> units;
    for (size_t i = 0; i < line.size();) {
        const size_t code = CodeLength(line, i);
        if (code) {
            (units.empty() ? prefix : pending).append(line.substr(i, code));
            i += code;
            continue;
        }
        const char16_t c = line[i++];
        const uint8_t kind = IsLtr(c) ? 1 : (IsRtl(c) ? 2 : 0);
        units.push_back({std::move(pending), c, kind});
        pending.clear();
    }
    // Neutrals between two left-to-right characters join that run.
    const size_t n = units.size();
    for (size_t i = 0; i < n;) {
        if (units[i].kind != 0) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < n && units[end].kind == 0) ++end;
        const bool ltr_before = i > 0 && units[i - 1].kind == 1;
        const bool ltr_after = end < n && units[end].kind == 1;
        for (size_t k = i; k < end; ++k) units[k].kind = (ltr_before && ltr_after) ? 1 : 2;
        i = end;
    }
    std::u16string out = prefix;
    out.reserve(line.size());
    for (size_t end = n; end > 0;) {
        size_t start = end - 1;
        if (units[start].kind == 1) {
            while (start > 0 && units[start - 1].kind == 1) --start;
            for (size_t k = start; k < end; ++k) {
                out += units[k].codes;
                out.push_back(units[k].character);
            }
        } else {
            const char16_t c = units[start].character;
            out += units[start].codes;
            out.push_back(c < 0x80 ? char16_t(rex::ui::rtl::MirrorInRtl(c)) : c);
        }
        end = start;
    }
    out += pending;  // codes after the last character
    return out;
}

// Menu cube text (the main menu, notices): the title lays a text out one
// character per cube, left to right, wrapping by words, and the runtime then
// mirrors every row the text occupies inside its box. That reverses each row,
// so this copy, in logical order, has its left-to-right runs (numbers, Latin
// words and the neutrals between them) reversed so that they read correctly
// after the mirror, and ASCII brackets in right-to-left context mirrored.
// Word wrap still sees the words in logical order. Texts without
// right-to-left letters are returned unchanged.
inline std::u16string MirroredLayoutText(std::u16string_view text) {
    if (!ContainsRtl(text)) return std::u16string(text);
    const size_t n = text.size();
    std::vector<uint8_t> kind(n);
    for (size_t i = 0; i < n; ++i) kind[i] = IsLtr(text[i]) ? 1 : (IsRtl(text[i]) ? 2 : 0);
    // Neutrals between two left-to-right characters join that run.
    for (size_t i = 0; i < n;) {
        if (kind[i] != 0) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < n && kind[end] == 0) ++end;
        const bool ltr_before = i > 0 && kind[i - 1] == 1;
        const bool ltr_after = end < n && kind[end] == 1;
        for (size_t k = i; k < end; ++k) kind[k] = (ltr_before && ltr_after) ? 1 : 2;
        i = end;
    }
    std::u16string out(text);
    for (size_t i = 0; i < n;) {
        if (kind[i] == 1) {
            size_t end = i;
            while (end < n && kind[end] == 1) ++end;
            for (size_t k = 0; k < end - i; ++k) out[i + k] = text[end - 1 - k];
            i = end;
        } else {
            const char16_t c = text[i];
            out[i] = c < 0x80 ? char16_t(rex::ui::rtl::MirrorInRtl(c)) : c;
            ++i;
        }
    }
    return out;
}

}  // namespace darkness::language_text
