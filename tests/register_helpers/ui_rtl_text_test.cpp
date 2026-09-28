// Arabic shaping and right-to-left ordering for the ImGui settings UI
// (rex/ui/rtl_text.h): contextual forms, lam-alef ligatures, marks, visual
// order with left-to-right runs, wrapping.
#include <rex/ui/rtl_text.h>

#include <iostream>
#include <string>

namespace {
bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}
}  // namespace

int main() {
  using namespace rex::ui::rtl;
  bool ok = true;

  // "salam" = seen lam alef meem: seen initial, lam-alef final ligature,
  // meem isolated (alef does not join forward).
  {
    const std::u32string shaped = ShapeArabic(U"سلام");
    ok &= Check(shaped == U"ﺳﻼﻡ", "salam: seen initial + lam-alef final + meem isolated");
  }
  // "beit" (beh yeh teh): initial, medial, final.
  {
    const std::u32string shaped = ShapeArabic(U"بيت");
    ok &= Check(shaped == U"ﺑﻴﺖ", "beh initial, yeh medial, teh final");
  }
  // Right-joining letters break the word: "dar" (dal alef reh) - all isolated
  // or final forms, no initial.
  {
    const std::u32string shaped = ShapeArabic(U"دار");
    ok &= Check(shaped == U"ﺩﺍﺭ", "dal, alef, reh isolated");
  }
  // Isolated lam-alef at a word start ("la").
  {
    ok &= Check(ShapeArabic(U"لا") == U"ﻻ", "lam-alef isolated ligature");
  }
  // A mark (shadda) is transparent: meem shadda dal -> meem initial, mark,
  // dal final.
  {
    const std::u32string shaped = ShapeArabic(U"مّد");
    ok &= Check(shaped == U"ﻣّﺪ", "shadda does not break joining");
  }

  // Visual order: Arabic reversed, "FSR 3.1" kept, brackets mirrored.
  {
    const std::u32string visual = VisualOrder(U"أ FSR 3.1 ب");
    ok &= Check(visual == U"ب FSR 3.1 أ", "left-to-right run keeps its order");
    ok &= Check(VisualOrder(U"أ (ب)") == U"(ب) أ", "brackets mirrored");
    // Bracket pairs (bidi N0): Latin inside after Latin stays in the Latin run;
    // Arabic inside makes the pair right-to-left (mirrored).
    ok &= Check(VisualOrder(U"أ XenonRecomp (MIT) ب") == U"ب XenonRecomp (MIT) أ",
                "Latin bracket pair stays with its run");
    ok &= Check(VisualOrder(U"لعبة The Darkness (Xbox 360) كبرنامج") ==
                    U"جمانربك The Darkness (Xbox 360) ةبعل",
                "closing bracket before Arabic stays in the Latin run");
    ok &= Check(VisualOrder(U"أ (ب c)") == U"(c ب) أ", "mixed pair is right-to-left");
    ok &= Check(VisualOrder(U"(MIT) أ") == U"أ (MIT)" || VisualOrder(U"(MIT) أ") == U"أ )MIT(",
                "pair at the line start");
    ok &= Check(VisualOrder(U"مّد") == U"دّم",
                "a mark is drawn just before its letter (the font places it over the next glyph)");
  }
  // Text without right-to-left letters is untouched.
  ok &= Check(VisualLine("Anisotropic filtering 16x") == "Anisotropic filtering 16x",
              "left-to-right text unchanged");
  ok &= Check(ContainsRtl("\xD8\xB3") && !ContainsRtl("abc"), "right-to-left detection");
  // UTF-8 round trip.
  {
    const std::string text = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 2x";
    ok &= Check(EncodeUtf8(DecodeUtf8(text)) == text, "UTF-8 round trip");
  }
  // Wrapping: each word measured as one unit per code point; lines come out
  // top to bottom in logical order, each in visual order.
  {
    auto measure = [](const std::string& s) { return float(DecodeUtf8(s).size()); };
    const std::vector<std::string> lines =
        WrapVisual("\xD8\xA3\xD8\xA8 \xD8\xAA\xD8\xAB \xD8\xAC\xD8\xAD", 5.0f, measure);
    ok &= Check(lines.size() == 2, "wrapped into two lines");
    ok &= Check(lines.size() == 2 && DecodeUtf8(lines[1]).size() == 2, "last word alone");
  }

  if (ok) std::cout << "ui_rtl_text: ok\n";
  return ok ? 0 : 1;
}
