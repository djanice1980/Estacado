// In-game language-pack text: formatting codes, shaping into the game code
// page and per-line visual order for the title's font renderer
// (runtime/runtime_language_text.h, runtime/runtime_language_codepage.h).
#include "runtime_language_text.h"

#include <initializer_list>
#include <iostream>
#include <string>

namespace {
using darkness::language_codepage::GameCodeIndex;
using darkness::language_codepage::IsArabicGameCode;
using darkness::language_codepage::kCodes;
using darkness::language_codepage::kForms;
using darkness::language_codepage::ToGameCode;
using darkness::language_text::CodeLength;
using darkness::language_text::MirroredLayoutText;
using darkness::language_text::PrepareText;
using darkness::language_text::VisualLine;

bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

// Code units from numbers (the source stays ASCII).
std::u16string U(std::initializer_list<unsigned> units) {
  std::u16string out;
  for (unsigned unit : units) out.push_back(char16_t(unit));
  return out;
}
std::u16string A(const char* ascii) {
  std::u16string out;
  for (const char* c = ascii; *c; ++c) out.push_back(char16_t(*c));
  return out;
}
// Arabic contextual forms -> game codes.
std::u16string G(std::initializer_list<unsigned> forms) {
  std::u16string out;
  for (unsigned cp : forms) out.push_back(ToGameCode(cp));
  return out;
}
std::u16string Reversed(std::u16string text) { return std::u16string(text.rbegin(), text.rend()); }

constexpr unsigned kSection = 0xA7;
}  // namespace

int main() {
  bool ok = true;
  // Code page: 127 forms on 0x80..0xFF without the section sign, one to one.
  ok &= Check(kCodes.front() == 0x80 && kCodes.back() == 0xFF && kCodes[0x27] == 0xA8,
              "codes skip 0xA7");
  ok &= Check(ToGameCode(0xFE80) == 0x80 && ToGameCode(0xFEFC) == 0xFD &&
                  ToGameCode(0x061F) == 0xFE && ToGameCode(0x060C) == 0xFF,
              "forms map in order");
  bool unique = true;
  for (size_t i = 0; i < kForms.size(); ++i) {
    unique = unique && ToGameCode(kForms[i]) == kCodes[i] && IsArabicGameCode(kCodes[i]);
  }
  ok &= Check(unique, "every form has its own Arabic game code");
  ok &= Check(!IsArabicGameCode(kSection) && !IsArabicGameCode('A') && ToGameCode('A') == 'A' &&
                  ToGameCode(0x061B) == ';',
              "section sign, ASCII and fallbacks");

  // The title's code lengths (sub_8238A380), sign-extended low byte.
  ok &= Check(CodeLength(U({kSection}) + A("Z14Hi"), 0) == 4, "size code");
  ok &= Check(CodeLength(U({kSection}) + A("c0f0"), 0) == 5 &&
                  CodeLength(U({kSection}) + A("C0f0"), 0) == 5,
              "colour code");
  ok &= Check(CodeLength(U({kSection}) + A("d"), 0) == 2 &&
                  CodeLength(U({kSection}) + A("q123456789"), 0) == 10,
              "reset and long codes");
  ok &= Check(CodeLength(U({kSection}) + A("x01"), 0) == 4, "a code cut by the end of the line");
  ok &= Check(CodeLength(U({kSection}) + A("B"), 0) == 0 && CodeLength(U({kSection}), 0) == 0 &&
                  CodeLength(A("a") + U({kSection}) + A("Z14"), 0) == 0 &&
                  CodeLength(U({kSection, 0x0627}), 0) == 0,
              "not codes");
  ok &= Check(CodeLength(U({kSection, 0x0643}) + A("123"), 0) == 5,
              "low byte 'C' counts like the title");

  // Shaping into the code page: lam-alef ligature, joined forms, harakat
  // dropped, logical order, formatting codes and ASCII unchanged.
  ok &= Check(PrepareText("\xD9\x84\xD8\xA7") == G({0xFEFB}), "lam-alef isolated");
  const std::u16string salam = PrepareText("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85");
  ok &= Check(salam == G({0xFEB3, 0xFEFC, 0xFEE1}),
              "salam: initial seen, final lam-alef, isolated meem");
  ok &= Check(PrepareText("\xD8\xA8\xD9\x8E") == G({0xFE8F}), "fatha dropped");
  ok &= Check(PrepareText("\xC2\xA7Z14OK") == U({kSection}) + A("Z14OK"), "codes and Latin unchanged");
  ok &= Check(PrepareText("\xD9\x85\xD8\x9F") == G({0xFEE1, 0x061F}), "Arabic question mark");

  // Visual order.
  const std::u16string z14 = U({kSection}) + A("Z14");
  ok &= Check(VisualLine(z14 + A("Hello 12")) == z14 + A("Hello 12"), "left-to-right unchanged");
  ok &= Check(VisualLine(U({0xE9, 0xC0})) == U({0xC0, 0xE9}),
              "Latin-1 codes are Arabic in the game code page");
  ok &= Check(VisualLine(salam) == Reversed(salam), "one word reversed");
  ok &= Check(VisualLine(z14 + salam) == z14 + Reversed(salam), "leading codes stay in front");
  ok &= Check(VisualLine(salam + A(" 3")) == A("3 ") + Reversed(salam), "number at the end");
  ok &= Check(VisualLine(salam + A(" FSR 3.1")) == A("FSR 3.1 ") + Reversed(salam),
              "Latin run keeps its order");
  ok &= Check(VisualLine(A("(") + salam + A(")")) == A("(") + Reversed(salam) + A(")"),
              "ASCII brackets mirrored");
  const std::u16string ab = G({0xFE91, 0xFE8E});
  ok &= Check(VisualLine(G({0xFEBB}) + U({0xAB}) + ab) == Reversed(ab) + U({0xAB}) + G({0xFEBB}),
              "game codes are never mirrored");
  const std::u16string colour = U({kSection}) + A("c0f0");
  ok &= Check(VisualLine(ab + colour + G({0xFEE1})) == colour + G({0xFEE1}) + Reversed(ab),
              "a mid-line code stays with the following letter");
  const std::u16string reset = U({kSection}) + A("d");
  ok &= Check(VisualLine(ab + reset) == Reversed(ab) + reset, "trailing code last");
  const std::u16string mixed = z14 + salam + A(" x2 (") + ab + A(") ") + colour + salam;
  ok &= Check(VisualLine(mixed).size() == mixed.size(), "same length (a permutation)");

  // Menu cube cells: a game code's position in the code page.
  bool indices = GameCodeIndex(0x80) == 0 && GameCodeIndex(0xA8) == 0x27 &&
                 GameCodeIndex(0xFF) == 126 && GameCodeIndex(kSection) == -1 &&
                 GameCodeIndex('A') == -1 && GameCodeIndex(0x100) == -1;
  for (size_t i = 0; i < kCodes.size(); ++i) indices = indices && GameCodeIndex(kCodes[i]) == int(i);
  ok &= Check(indices, "game code index follows kCodes");

  // Menu cube text: laid out left to right, then each row mirrored. For one
  // line without codes that equals the visual order.
  ok &= Check(MirroredLayoutText(A("Hello 12")) == A("Hello 12"), "left-to-right unchanged");
  ok &= Check(MirroredLayoutText(salam + A(" ") + ab) == salam + A(" ") + ab,
              "right-to-left words keep logical order");
  ok &= Check(MirroredLayoutText(salam + A(" FSR 3.1")) == salam + A(" 1.3 RSF"),
              "left-to-right run reversed for the mirror");
  for (const std::u16string& line :
       {salam, salam + A(" 3"), salam + A(" FSR 3.1"), A("(") + salam + A(")"),
        A("Xbox 360 ") + salam + A(" ") + ab, ab + A(" x2 (") + salam + A(")!")}) {
    ok &= Check(Reversed(MirroredLayoutText(line)) == VisualLine(line),
                "mirrored layout reads like the visual line");
  }
  if (ok) std::cout << "runtime_language_text: ok\n";
  return ok ? 0 : 1;
}
