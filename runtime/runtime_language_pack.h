#pragma once

// In-game text from a local language pack (Arabic first). The game ships
// English, French, German, Italian and Spanish; a pack adds a language on top
// of English (speech stays English):
//
//   <executable>/language_packs/<name>/
//     strings.tsv   KEY<TAB>text, UTF-8, logical order (\t \n \\ escapes);
//                   replaces the title's string-table value for KEY
//                   (menus, prompts, objectives, loading tips)
//     content/      optional loose files served over the game content with
//                   the highest priority; content/Content/Fonts/<NAME>_AR.xfc
//                   replaces the game font <NAME> (TEXT, HEADINGS, ...), which
//                   carries the pack's glyphs in the game code page
//                   (runtime_language_codepage.h)
//     textures/     optional <id>.dds replacements of title textures (the
//                   texture-pack format), used whenever the pack is active;
//                   ae688f97f8947ee6.dds is the menu cube atlas
//                   GUI_Proto_Text with the pack's glyphs added (cube cells
//                   runtime_language_pack.cpp)
//
// Keys missing from the pack keep the English text. Right-to-left text is
// shaped when the pack loads and put into visual order per drawn line
// (runtime_language_text.h). The translated text derives from the game's
// script, so packs are produced locally and never committed or shipped.

#include "runtime_filesystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct RuntimeLanguagePackStatus {
    bool present{};
    bool active{};
    std::filesystem::path root;
    size_t strings{};
    size_t rejectedLines{};
    size_t fonts{};
    std::optional<GuestContentOverlay> contentOverlay;
    // <root>/textures when present (empty otherwise), and whether it holds
    // the menu cube atlas.
    std::filesystem::path textures;
    bool cubeAtlas{};
    std::string error;
};

// Before the title runs. A missing pack leaves the game in English.
RuntimeLanguagePackStatus ConfigureRuntimeLanguagePack(const std::filesystem::path& root);

// After the image is mapped, before the title runs: with the pack's menu
// cube atlas, the title's random blank cube cells move to the atlas's first
// row (runtime_language_pack.cpp, menu cube cells).
void ApplyRuntimeLanguagePackImagePatches(uint8_t* base);

// The graphics settings with the pack's texture folder as the top-level
// gpu_texture_language_root (read by the texture cache: these replacements
// apply whenever the pack is active, independent of HD texture packs).
// Unchanged when textures is empty.
std::string RuntimePcConfigWithLanguageTextures(const std::string& contents,
                                                const std::filesystem::path& textures);
