// In-game text from a local language pack: see runtime_language_pack.h.
#include "runtime_language_pack.h"
#include "runtime_button_prompts.h"

#include "runtime_language_text.h"
#include "runtime_memory_access.h"
#include "ppc_recomp_shared.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

extern "C" PPC_FUNC(__imp__sub_82787CC0);
extern "C" PPC_FUNC(__imp__sub_8238BD10);
extern "C" PPC_FUNC(__imp__sub_8237B890);
extern "C" PPC_FUNC(__imp__sub_8234C900);
extern "C" PPC_FUNC(__imp__sub_8239B398);
extern "C" PPC_FUNC(__imp__sub_8239B708);
extern "C" PPC_FUNC(__imp__sub_8234D0F0);

namespace {

// Read-only once the title runs.
bool g_active = false;
// The pack carries the menu cube atlas with its glyphs (see the cube cells
// below).
bool g_cube_atlas = false;
std::unordered_map<std::string, std::u16string> g_strings;
// Fonts the pack replaces (upper-case font names, e.g. TEXT for
// content/Content/Fonts/Text_AR.xfc).
std::unordered_set<std::string> g_fonts;

// CStr: every instance starts with this vtable; +108 assigns from an 8-bit
// string (the call the font loader uses for its name).
constexpr uint32_t kCStrAssignSlot = 108;

// Guest stack layout of the font Write loop sub_8238BD10: its stack
// arguments occupy entry r1 + 84 .. 128 and the UTF-16 line is at r1 + 100.
constexpr uint32_t kWriteParameterBytes = 160;
constexpr uint32_t kWriteTextArgument = 100;
constexpr size_t kMaximumLine = 1024;  // the title's localize buffer
constexpr size_t kMaximumKey = 128;

std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool IsKey(std::string_view key) {
    if (key.empty() || key.size() >= kMaximumKey) return false;
    for (unsigned char c : key) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

// \t, \n and \\ escapes of the pack's tab-separated files.
std::string Unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 't' || next == 'n' || next == '\\') {
                out.push_back(next == 't' ? '\t' : next == 'n' ? '\n' : '\\');
                ++i;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

size_t LoadStrings(const std::filesystem::path& path, size_t* rejected) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return 0;
    std::string line;
    bool first = true;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        first = false;
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        const std::string key = tab == std::string::npos ? std::string() : line.substr(0, tab);
        if (!IsKey(key)) {
            ++*rejected;
            continue;
        }
        std::u16string text = darkness::language_text::PrepareText(Unescape(line.substr(tab + 1)));
        if (text.empty() || text.size() >= kMaximumLine) {
            ++*rejected;
            continue;
        }
        g_strings[Upper(key)] = std::move(text);
    }
    return g_strings.size();
}

// A short 8-bit guest string (keys, font names); empty when it is longer
// than kMaximumKey or not an identifier.
std::string ReadKey(uint8_t* base, uint32_t address) {
    if (!address) return {};
    char key[kMaximumKey];
    size_t length = 0;
    for (; length < kMaximumKey; ++length) {
        const char c = char(PPC_LOAD_U8(address + length));
        if (!c) break;
        key[length] = c;
    }
    if (length == 0 || length == kMaximumKey || !IsKey(std::string_view(key, length))) {
        return {};
    }
    return Upper(std::string_view(key, length));
}

const std::u16string* FindString(uint8_t* base, uint32_t keyAddress) {
    const std::string key = ReadKey(base, keyAddress);
    if (key.empty()) return nullptr;
    const auto found = g_strings.find(key);
    return found == g_strings.end() ? nullptr : &found->second;
}

void LoadFonts(const std::filesystem::path& folder) {
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string stem = Upper(entry.path().stem().string());
        if (!entry.is_regular_file(error) || Upper(entry.path().extension().string()) != ".XFC" ||
            stem.size() < 4 || stem.compare(stem.size() - 3, 3, "_AR") != 0) {
            continue;
        }
        const std::string name = stem.substr(0, stem.size() - 3);
        if (IsKey(name)) g_fonts.insert(name);
    }
}

// Writes text + NUL as big-endian UTF-16 at a guest address.
void StoreText(uint8_t* base, uint32_t address, std::u16string_view text) {
    for (size_t i = 0; i < text.size(); ++i) PPC_STORE_U16(address + uint32_t(i * 2), text[i]);
    PPC_STORE_U16(address + uint32_t(text.size() * 2), 0);
}

uint32_t AlignedTextBytes(size_t units) { return (uint32_t((units + 1) * 2) + 15u) & ~15u; }

// Menu cube text (CMWnd_CubeText, CubeButton: the main menu, notices, the
// pause menu). Every cube face draws one cell of the atlas GUI_Proto_Text
// (512 x 1024), chosen by a cell number: large glyphs (sizes 'n' and 'h')
// in 16 x 32 cells of 32 x 32 pixels, small glyphs (size 's') in 32 x 32
// half cells of 16 x 32 pixels. The title's cells: large glyphs from 272,
// small ones from 864, in the order of its character table (ASCII and part
// of Latin-1). Empty cubes (spaces, unknown characters, the cubes around the
// text) get a random cell below 190, all empty: 21 title functions scale a
// random number by the constant 190 / 2^32 at kCubeRandomScale. The pack's
// atlas keeps all of that and adds, in the empty area:
//   row 0            empty (blank cells 0..15 in both grids)
//   cells 16..142    large glyphs of the game codes (kCodes order)
//   cells 288..414   small glyphs of the game codes
// and with it the constant becomes 15 / 2^32 (ApplyRuntimeLanguagePackImage-
// Patches), so random blank cells stay in row 0, and the cube face
// comparison (sub_8234D0F0) takes cells below 16 as blank.
constexpr uint32_t kCubeRandomScale = 0x8209E500;
constexpr uint32_t kCubeRandomScaleBits = 0x333E0000;  // 190 / 2^32
constexpr uint32_t kCubeBlankScaleBits = 0x31700000;   // 15 / 2^32
constexpr uint32_t kCubeBlankCells = 16;
constexpr uint32_t kCubeLargeCells = 16;
constexpr uint32_t kCubeSmallCells = 288;
constexpr char kCubeAtlasFile[] = "ae688f97f8947ee6.dds";

// The cube grid (the layout's r3): 20 x 20 cells of 6 bytes from +288, row
// by row: mode (0 one large glyph, 1 a quarter of a 2 x 2 glyph, 2 two small
// glyphs), a flag byte, cell a, cell b (the right half in mode 2).
constexpr uint32_t kCubeGrid = 288;
constexpr uint32_t kCubeColumns = 20;
constexpr uint32_t kCubeRows = 20;
constexpr uint32_t kCubeRecord = 6;
using CubeRecord = std::array<uint8_t, kCubeRecord>;

// The layout's conversion of a box given in pixels (flag 4 clear) to cells.
constexpr uint32_t kCubeBoxScaleX = 0x8205BE44;
constexpr uint32_t kCubeBoxScaleY = 0x8209DE58;

float LoadFloat(uint8_t* base, uint32_t address) {
    const uint32_t bits = PPC_LOAD_U32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

RuntimeLanguagePackStatus ConfigureRuntimeLanguagePack(const std::filesystem::path& root) {
    RuntimeLanguagePackStatus status;
    status.root = root;
    std::error_code error;
    status.present = std::filesystem::is_directory(root, error);
    if (!status.present) return status;
    status.strings = LoadStrings(root / L"strings.tsv", &status.rejectedLines);
    const std::filesystem::path content = root / L"content";
    if (std::filesystem::is_directory(content, error)) {
        const std::filesystem::path canonical = std::filesystem::canonical(content, error);
        if (!error) status.contentOverlay = GuestContentOverlay{"language-pack", canonical, 1000000};
        LoadFonts(content / L"Content" / L"Fonts");
    }
    status.fonts = g_fonts.size();
    status.active = status.strings > 0;
    g_active = status.active;
    const std::filesystem::path textures = root / L"textures";
    if (status.active && std::filesystem::is_directory(textures, error)) {
        status.textures = textures;
        status.cubeAtlas = std::filesystem::is_regular_file(textures / kCubeAtlasFile, error);
    }
    g_cube_atlas = status.cubeAtlas;
    std::string fonts;
    for (const auto& name : g_fonts) fonts += (fonts.empty() ? "" : ",") + name;
    std::fprintf(stderr,
                 "RUNTIME_LANGUAGE_PACK root=%s active=%u strings=%zu rejected=%zu content=%u "
                 "fonts=%s textures=%u cube_atlas=%u\n",
                 root.string().c_str(), status.active ? 1u : 0u, status.strings,
                 status.rejectedLines, status.contentOverlay ? 1u : 0u,
                 fonts.empty() ? "none" : fonts.c_str(), status.textures.empty() ? 0u : 1u,
                 status.cubeAtlas ? 1u : 0u);
    std::fflush(stderr);
    return status;
}

std::string RuntimePcConfigWithLanguageTextures(const std::string& contents,
                                                const std::filesystem::path& textures) {
    if (textures.empty()) return contents;
    // A TOML basic string; top-level keys precede every table.
    const auto path = textures.u8string();  // UTF-8
    std::string line = "gpu_texture_language_root = \"";
    for (const auto c : path) {
        if (c == '\\' || c == '"') line.push_back('\\');
        line.push_back(char(c));
    }
    line += "\"\n";
    const bool bom = contents.rfind("\xEF\xBB\xBF", 0) == 0;
    return bom ? contents.substr(0, 3) + line + contents.substr(3) : line + contents;
}

void ApplyRuntimeLanguagePackImagePatches(uint8_t* base) {
    if (!g_cube_atlas) return;
    const uint32_t found = PPC_LOAD_U32(kCubeRandomScale);
    if (found != kCubeRandomScaleBits) {
        // Another title build: keep the title's blank cells (menu text then
        // shares the atlas rows with them, so the pack's cells stay unused).
        g_cube_atlas = false;
        std::fprintf(stderr, "RUNTIME_LANGUAGE_PACK cube_random_scale mismatch found=0x%08X\n",
                     found);
        std::fflush(stderr);
        return;
    }
    PPC_STORE_U32(kCubeRandomScale, kCubeBlankScaleBits);
    std::fprintf(stderr, "RUNTIME_LANGUAGE_PACK cube_random_scale 190 -> 15\n");
    std::fflush(stderr);
}

// String-table lookup (r3 = CStr result, r4 = 8-bit key): SYSTEM ->
// STRINGTABLES, later tables first, else the key itself. A key the pack
// translates becomes a wide CStr built by the title's own constructor
// (sub_821F8728) from a copy of the text on the guest stack below this
// frame.
PPC_FUNC(sub_82787CC0) {
    // Keyboard button prompts (runtime_button_prompts.h): the title's
    // controller prompt templates and button names, while keyboard prompts
    // are wanted.
    std::optional<std::u16string> prompt;
    if (PPC_LOAD_U8(ctx.r4.u32) == 'C') {
        const std::string key = ReadKey(base, ctx.r4.u32);
        if (!key.empty()) prompt = RuntimeKeyboardPromptText(key);
    }
    const std::u16string* text =
        prompt ? &*prompt : (g_active ? FindString(base, ctx.r4.u32) : nullptr);
    if (!text) {
        __imp__sub_82787CC0(ctx, base);
        return;
    }
    const uint32_t result = ctx.r3.u32;
    const uint32_t stack = ctx.r1.u32;
    const uint32_t frame = (stack - 256 - AlignedTextBytes(text->size())) & ~15u;
    const uint32_t buffer = frame + 128;
    StoreText(base, buffer, *text);
    PPC_STORE_U32(frame, stack);
    ctx.r1.u32 = frame;
    ctx.r3.u32 = result;
    ctx.r4.u32 = buffer;
    sub_821F8728(ctx, base);
    ctx.r1.u32 = stack;
    ctx.r3.u32 = result;
}

// Font resources (XFC:<NAME>) load FONTS\<NAME>.XFC (r3 = font, r5 = name),
// which the title reads from its XDF precache packs, so loose files with
// those names are never read. A pack's fonts are named <NAME>_AR.XFC (in no
// XDF): the loader gets that name for the file while the font keeps its own
// name, restored afterwards with the CStr assign the loader itself uses.
PPC_FUNC(sub_8237B890) {
    if (!g_active || !ctx.r5.u32) {
        __imp__sub_8237B890(ctx, base);
        return;
    }
    // The resource name, "XFC:<NAME>" (the loader drops the first four
    // characters), sometimes with the extension.
    std::string raw;
    for (uint32_t i = 0; i < kMaximumKey; ++i) {
        const char c = char(PPC_LOAD_U8(ctx.r5.u32 + i));
        if (!c) break;
        raw.push_back(c);
    }
    const std::string upper = Upper(raw);
    const size_t prefix = upper.rfind("XFC:", 0) == 0 ? 4 : 0;
    std::string name = upper.substr(prefix);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".XFC") == 0) {
        name.resize(name.size() - 4);
    }
    const bool replaced = IsKey(name) && g_fonts.find(name) != g_fonts.end();
    std::fprintf(stderr, "RUNTIME_LANGUAGE_PACK font_request name=%s pack_font=%u\n", raw.c_str(),
                 replaced ? 1u : 0u);
    std::fflush(stderr);
    if (!replaced) {
        __imp__sub_8237B890(ctx, base);
        return;
    }
    const uint32_t font = ctx.r3.u32;
    const uint32_t originalName = ctx.r5.u32;
    const std::string redirected = raw.substr(0, prefix) + name + "_AR";
    const uint32_t stack = ctx.r1.u32;
    const uint32_t frame = (stack - 256 - 64) & ~15u;
    const uint32_t buffer = frame + 128;
    for (size_t i = 0; i < redirected.size(); ++i) {
        PPC_STORE_U8(buffer + uint32_t(i), uint8_t(redirected[i]));
    }
    PPC_STORE_U8(buffer + uint32_t(redirected.size()), 0);
    PPC_STORE_U32(frame, stack);
    ctx.r1.u32 = frame;
    ctx.r5.u32 = buffer;
    __imp__sub_8237B890(ctx, base);
    const uint64_t loaded = ctx.r3.u64;
    ctx.r3.u32 = font + 8;
    ctx.r4.u32 = originalName;
    const uint32_t assign = PPC_LOAD_U32(PPC_LOAD_U32(font + 8) + kCStrAssignSlot);
    PPC_CALL_INDIRECT_FUNC(assign);
    ctx.r1.u32 = stack;
    ctx.r3.u64 = loaded;
    std::fprintf(stderr, "RUNTIME_LANGUAGE_PACK font=%s file=%s loaded=%u\n", name.c_str(),
                 redirected.c_str(), unsigned(loaded & 0xFF));
    std::fflush(stderr);
}

// Word-wrapped text box (sub_8234C900: r7 = UTF-16 text, r10 = flags). The
// title aligns boxes left or centred only (TEXT_CENTER 0x800); a wrapped
// right-to-left paragraph aligned left leaves its lines ragged on the wrong
// side, so boxes with right-to-left text are centred (subtitles, messages).
PPC_FUNC(sub_8234C900) {
    constexpr uint64_t kTextCenter = 0x800;
    if (g_active && ctx.r7.u32 && !(ctx.r10.u64 & kTextCenter)) {
        for (uint32_t i = 0; i < kMaximumLine; ++i) {
            const char16_t unit = char16_t(PPC_LOAD_U16(ctx.r7.u32 + i * 2));
            if (!unit) break;
            if (darkness::language_text::IsRtl(unit)) {
                ctx.r10.u64 |= kTextCenter;
                break;
            }
        }
    }
    __imp__sub_8234C900(ctx, base);
}

namespace {
// Developer trace (DARKNESS_TEXT_TRACE=<file>): every distinct drawn line
// with formatting codes, the codes escaped (button prompt research, 0.9.1).
std::atomic<int> g_text_trace_state{-1};  // -1 unknown, 0 off, 1 on
std::mutex g_text_trace_mutex;
std::ofstream g_text_trace;
std::unordered_set<std::u16string> g_text_trace_seen;

void TraceDrawnLine(uint8_t* base, uint32_t textAddress) {
    int state = g_text_trace_state.load(std::memory_order_relaxed);
    if (state < 0) {
        std::lock_guard lock(g_text_trace_mutex);
        state = g_text_trace_state.load(std::memory_order_relaxed);
        if (state < 0) {
            const char* path = std::getenv("DARKNESS_TEXT_TRACE");
            if (path && *path) g_text_trace.open(path, std::ios::binary | std::ios::trunc);
            state = g_text_trace.is_open() ? 1 : 0;
            g_text_trace_state.store(state, std::memory_order_relaxed);
        }
    }
    if (!state || !textAddress) return;
    std::u16string line;
    bool codes = false;
    for (size_t i = 0; i < kMaximumLine; ++i) {
        const char16_t unit = char16_t(PPC_LOAD_U16(textAddress + uint32_t(i * 2)));
        if (!unit) break;
        codes = codes || unit == darkness::language_text::kCodeMarker || unit < 0x20 || unit > 0xFF;
        line.push_back(unit);
    }
    if (!codes) return;
    std::lock_guard lock(g_text_trace_mutex);
    if (g_text_trace_seen.size() >= 4096 || !g_text_trace_seen.insert(line).second) return;
    std::ostringstream out;
    for (const char16_t unit : line) {
        if (unit >= 0x20 && unit < 0x7F) {
            out << char(unit);
        } else {
            out << "<" << std::hex << unsigned(unit) << std::dec << ">";
        }
    }
    g_text_trace << out.str() << '\n';
    g_text_trace.flush();
}
}  // namespace

// Font Write loop: draws one UTF-16 line left to right. A line with
// right-to-left letters is drawn from a visual-order copy: the stack
// arguments are copied below this frame with the text argument pointing at
// the copy, so the title's buffers are never modified.
PPC_FUNC(sub_8238BD10) {
    if (g_text_trace_state.load(std::memory_order_relaxed) != 0) {
        TraceDrawnLine(base, PPC_LOAD_U32(ctx.r1.u32 + kWriteTextArgument));
    }
    if (!g_active) {
        __imp__sub_8238BD10(ctx, base);
        return;
    }
    const uint32_t stack = ctx.r1.u32;
    const uint32_t textAddress = PPC_LOAD_U32(stack + kWriteTextArgument);
    std::u16string line;
    bool rtl = false;
    if (textAddress) {
        for (size_t i = 0; i < kMaximumLine; ++i) {
            const char16_t unit = char16_t(PPC_LOAD_U16(textAddress + uint32_t(i * 2)));
            if (!unit) break;
            rtl = rtl || darkness::language_text::IsRtl(unit);
            line.push_back(unit);
        }
    }
    if (!rtl) {
        __imp__sub_8238BD10(ctx, base);
        return;
    }
    const std::u16string visual = darkness::language_text::VisualLine(line);
    const uint32_t frame =
        (stack - kWriteParameterBytes - 128 - AlignedTextBytes(visual.size())) & ~15u;
    for (uint32_t offset = 0; offset < kWriteParameterBytes; offset += 4) {
        PPC_STORE_U32(frame + offset, PPC_LOAD_U32(stack + offset));
    }
    const uint32_t buffer = frame + kWriteParameterBytes;
    StoreText(base, buffer, visual);
    PPC_STORE_U32(frame + kWriteTextArgument, buffer);
    ctx.r1.u32 = frame;
    __imp__sub_8238BD10(ctx, base);
    ctx.r1.u32 = stack;
}

// Menu cube character -> atlas cell (sub_8239B398: r3 = the character's low
// byte, r4 = 0 large, 1 small). With the pack's atlas the game codes get the
// pack's cells.
PPC_FUNC(sub_8239B398) {
    if (!g_cube_atlas) {
        __imp__sub_8239B398(ctx, base);
        return;
    }
    const int index = darkness::language_codepage::GameCodeIndex(char16_t(ctx.r3.u32 & 0xFF));
    if (index >= 0) {
        ctx.r3.u64 = ((ctx.r4.u32 & 0xFF) ? kCubeSmallCells : kCubeLargeCells) + uint32_t(index);
        return;
    }
    __imp__sub_8239B398(ctx, base);
}

// Cube face comparison (sub_8234D0F0: r3, r4 = two cell records from their
// mode byte: mode, flag, cell a, cell b; returns 1 for the same face, so the
// cube does not turn). Faces match when the modes match and every cell
// matches, any two blank cells counting as equal; the title's blank cells
// are those below 190. With the pack's atlas the blank cells are those below
// 16, so the pack's large glyphs (16..142) count as glyphs and their cubes
// turn to show them. Same logic as the title's otherwise.
PPC_FUNC(sub_8234D0F0) {
    if (!g_cube_atlas) {
        __imp__sub_8234D0F0(ctx, base);
        return;
    }
    const uint32_t a = ctx.r3.u32;
    const uint32_t b = ctx.r4.u32;
    const uint8_t mode = PPC_LOAD_U8(a);
    const auto same = [](uint32_t x, uint32_t y) {
        return x == y || (x < kCubeBlankCells && y < kCubeBlankCells);
    };
    bool equal = false;
    if (mode == PPC_LOAD_U8(b)) {
        const uint32_t a0 = PPC_LOAD_U16(a + 2);
        const uint32_t b0 = PPC_LOAD_U16(b + 2);
        const uint32_t a1 = PPC_LOAD_U16(a + 4);
        const uint32_t b1 = PPC_LOAD_U16(b + 4);
        switch (mode) {
            case 0: equal = same(a0, b0); break;                       // one large glyph
            case 1: equal = a0 == b0 && a1 == b1; break;               // quarter of a 2 x 2 glyph
            case 2: equal = same(a0, b0) && same(a1, b1); break;       // two small glyphs
            case 3: equal = true; break;
            default: break;
        }
    }
    ctx.r3.u64 = equal ? 1 : 0;
}

// Menu cube text layout (sub_8239B708: r3 = cube grid, r4 = box {x0, y, x1},
// r5 = size 'n', 's' or 'h', r6 = UTF-16 text, r7 = flags; flag 4: the box
// is in cells, else in pixels). Characters are placed left to right from the
// box's left edge or centred (flag 8), wrapping by words. A right-to-left
// text is laid out from MirroredLayoutText and every row it wrote is then
// mirrored inside the box (the halves of a small-glyph cell swap too); cells
// it vacates get back what they held before the call. Size 'h' (one line,
// 2 x 2 cells per character) is laid out in visual order instead. The copy
// of the text lives below this frame: the title's strings are not modified.
PPC_FUNC(sub_8239B708) {
    if (!g_active || !ctx.r6.u32) {
        __imp__sub_8239B708(ctx, base);
        return;
    }
    std::u16string text;
    for (size_t i = 0; i < kMaximumLine; ++i) {
        const char16_t unit = char16_t(PPC_LOAD_U16(ctx.r6.u32 + uint32_t(i * 2)));
        if (!unit) break;
        text.push_back(unit);
    }
    if (!darkness::language_text::ContainsRtl(text)) {
        __imp__sub_8239B708(ctx, base);
        return;
    }
    const bool large2x2 = ctx.r5.u32 == 'h';
    const std::u16string layout = large2x2 ? darkness::language_text::VisualLine(text)
                                           : darkness::language_text::MirroredLayoutText(text);
    const uint32_t stack = ctx.r1.u32;
    const uint32_t frame = (stack - 256 - AlignedTextBytes(layout.size())) & ~15u;
    const uint32_t buffer = frame + 128;
    StoreText(base, buffer, layout);
    PPC_STORE_U32(frame, stack);
    if (large2x2) {
        ctx.r1.u32 = frame;
        ctx.r6.u32 = buffer;
        __imp__sub_8239B708(ctx, base);
        ctx.r1.u32 = stack;
        return;
    }
    // The box's columns, converted as the layout converts them.
    const uint32_t box = ctx.r4.u32;
    int x0 = int32_t(PPC_LOAD_U32(box));
    int x1 = int32_t(PPC_LOAD_U32(box + 8));
    if (!(ctx.r7.u32 & 4)) {
        const float scale = LoadFloat(base, kCubeBoxScaleX);
        x0 = int(float(x0) * scale);
        x1 = int(float(x1) * scale);
    }
    // Cells the call writes: their flag byte (0 or 1) replaces a marker.
    constexpr uint8_t kUnwritten = 0xFF;
    const uint32_t grid = ctx.r3.u32 + kCubeGrid;
    std::array<CubeRecord, kCubeColumns * kCubeRows> before;
    for (uint32_t cell = 0; cell < kCubeColumns * kCubeRows; ++cell) {
        const uint32_t record = grid + cell * kCubeRecord;
        for (uint32_t k = 0; k < kCubeRecord; ++k) before[cell][k] = PPC_LOAD_U8(record + k);
        PPC_STORE_U8(record + 1, kUnwritten);
    }
    ctx.r1.u32 = frame;
    ctx.r6.u32 = buffer;
    __imp__sub_8239B708(ctx, base);
    ctx.r1.u32 = stack;
    for (uint32_t row = 0; row < kCubeRows; ++row) {
        std::array<CubeRecord, kCubeColumns> after;
        std::array<bool, kCubeColumns> written{};
        int first = -1;
        int last = -1;
        for (uint32_t col = 0; col < kCubeColumns; ++col) {
            const uint32_t cell = row * kCubeColumns + col;
            const uint32_t record = grid + cell * kCubeRecord;
            for (uint32_t k = 0; k < kCubeRecord; ++k) after[col][k] = PPC_LOAD_U8(record + k);
            if (after[col][1] == kUnwritten) {
                after[col][1] = before[cell][1];
                continue;
            }
            written[col] = true;
            if (first < 0) first = int(col);
            last = int(col);
        }
        std::array<CubeRecord, kCubeColumns> result = after;
        if (first >= 0) {
            const int lo = std::max(0, std::min(x0, first));
            const int hi = std::min(int(kCubeColumns), std::max(x1, last + 1));
            for (int col = lo; col < hi; ++col) {
                if (written[col]) result[col] = before[row * kCubeColumns + uint32_t(col)];
            }
            for (int col = lo; col < hi; ++col) {
                if (!written[col]) continue;
                CubeRecord moved = after[col];
                if (moved[0] == 2) {
                    std::swap(moved[2], moved[4]);
                    std::swap(moved[3], moved[5]);
                }
                result[size_t(lo + hi - 1 - col)] = moved;
            }
        }
        for (uint32_t col = 0; col < kCubeColumns; ++col) {
            const uint32_t record = grid + (row * kCubeColumns + col) * kCubeRecord;
            for (uint32_t k = 0; k < kCubeRecord; ++k) PPC_STORE_U8(record + k, result[col][k]);
        }
    }
}
