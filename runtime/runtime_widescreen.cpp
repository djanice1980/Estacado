// Widescreen (V406, experimental): see runtime_widescreen.h.
#include "runtime_widescreen.h"
#include "runtime_pc_settings.h"

#include "runtime_memory_access.h"
#include "runtime_video_mode.h"
#include "ppc_recomp_shared.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

extern "C" PPC_FUNC(__imp__sub_8223E268);

namespace {

// The title's 16:9 constant (scaler height, movie pixel aspect).
constexpr uint32_t kSixteenByNineConstant = 0x8209F124;
constexpr uint32_t kSixteenByNineBits = 0x3FE38E39;  // 1.7777778f
}  // namespace

RuntimeGuestDisplaySize RuntimeGuestDisplaySizeFromEnvironment() {
    RuntimeGuestDisplaySize size;
    // REX_GUEST_DISPLAY: the same, through the isolated test launcher.
    const char* text = std::getenv("DARKNESS_GUEST_DISPLAY");
    if (!text) text = std::getenv("REX_GUEST_DISPLAY");
    if (!text) return size;
    unsigned width = 0, height = 0;
    if (std::sscanf(text, "%ux%u", &width, &height) == 2 && width >= 640 && height >= 480 &&
        width <= 4096 && height <= 2048 && width % 16 == 0 && height % 16 == 0) {
        size.width = width;
        size.height = height;
    }
    return size;
}

void ConfigureRuntimeWidescreen(RuntimeGuestDisplaySize size) {
    darkness::guest_video_mode::ConfigureDisplaySize(size.width, size.height);
    std::fprintf(stderr, "RUNTIME_WIDESCREEN guest_display=%ux%u active=%u\n", size.width,
                 size.height, size.Widescreen() ? 1u : 0u);
    std::fflush(stderr);
}

std::string RuntimePcConfigWithGuestDisplaySize(const std::string& contents,
                                                RuntimeGuestDisplaySize size) {
    if (!size.Widescreen()) return contents;
    std::string adjusted = contents;
    // The internal-scale class boundary follows the wider mode (the title's
    // half-resolution post buffers stay native): the top-level
    // draw_resolution_scale_threshold line, when it holds the title value.
    static const std::regex kThreshold(R"((^|\n)draw_resolution_scale_threshold = (\d+)[ \t]*(\r?\n))");
    std::smatch match;
    if (std::regex_search(adjusted, match, kThreshold)) {
        const uint32_t configured = uint32_t(std::stoul(match[2].str()));
        const uint32_t threshold = RuntimeWidescreenScaleThreshold(size.width, configured);
        if (threshold != configured) {
            adjusted.replace(size_t(match.position(0)), size_t(match.length(0)),
                             match[1].str() + "draw_resolution_scale_threshold = " +
                                 std::to_string(threshold) + match[3].str());
            std::fprintf(stderr, "RUNTIME_WIDESCREEN scale_threshold %u -> %u\n", configured,
                         threshold);
            std::fflush(stderr);
        }
    }
    // The built-in bloom rules name the 16:9 buffers; a taller mode's buffers
    // follow the video mode (RuntimeWidescreenNativeGridRules; wider modes
    // are left alone there). Only the title's own rule string is rewritten;
    // custom rules are the player's.
    static const std::regex kRules(
        R"((^|\n)draw_resolution_scale_native_grid_rules = (["'])([^"'\n]*)\2[ \t]*(\r?\n))");
    if (std::regex_search(adjusted, match, kRules) && match[3].str() == kTitleNativeGridRules &&
        RuntimeWidescreenNativeGridRules(match[3].str(), size.width, size.height) != match[3].str()) {
        const std::string rules =
            RuntimeWidescreenNativeGridRules(match[3].str(), size.width, size.height);
        adjusted.replace(size_t(match.position(0)), size_t(match.length(0)),
                         match[1].str() + "draw_resolution_scale_native_grid_rules = " +
                             match[2].str() + rules + match[2].str() + match[4].str());
        std::fprintf(stderr, "RUNTIME_WIDESCREEN native_grid_rules 1280x720 -> %ux%u, 160x90 -> %ux%u\n",
                     size.width, size.height, size.width / 8, size.height / 8);
        std::fflush(stderr);
    }
    // Top-level keys must precede the first table.
    const std::string keys = "video_mode_width = " + std::to_string(size.width) +
                             "\nvideo_mode_height = " + std::to_string(size.height) + "\n";
    return keys + adjusted;
}

void ApplyRuntimeWidescreenImagePatches(uint8_t* base) {
    const uint32_t width = darkness::guest_video_mode::DisplayWidth();
    const uint32_t height = darkness::guest_video_mode::DisplayHeight();
    if (width == darkness::guest_video_mode::kDisplayWidth &&
        height == darkness::guest_video_mode::kDisplayHeight) {
        return;
    }
    const uint32_t found = PPC_LOAD_U32(kSixteenByNineConstant);
    if (found != kSixteenByNineBits) {
        std::fprintf(stderr, "RUNTIME_WIDESCREEN constant_mismatch found=0x%08X\n", found);
        std::fflush(stderr);
        return;
    }
    const float aspect = float(width) / float(height);
    uint32_t bits;
    std::memcpy(&bits, &aspect, sizeof(bits));
    PPC_STORE_U32(kSixteenByNineConstant, bits);
    std::fprintf(stderr, "RUNTIME_WIDESCREEN aspect_constant=%.6f\n", aspect);
    std::fflush(stderr);
}

// The title applies its chosen mode here (display object in r3): backbuffer
// and front buffers take the mode entry's width and height (+64/+68). The
// entry is [[[display+24]+24] + index*4], index at display+304.
PPC_FUNC(sub_8223E268) {
    const uint32_t width = darkness::guest_video_mode::DisplayWidth();
    const uint32_t height = darkness::guest_video_mode::DisplayHeight();
    const uint32_t display = ctx.r3.u32;
    if ((width != 1280 || height != 720) && display) {
        const uint32_t owner = PPC_LOAD_U32(display + 24);
        const uint32_t list = owner ? PPC_LOAD_U32(owner + 24) : 0;
        const uint32_t index = PPC_LOAD_U32(display + 304);
        const uint32_t mode = list && index < 64 ? PPC_LOAD_U32(list + index * 4) : 0;
        if (mode) {
            const uint32_t modeWidth = PPC_LOAD_U32(mode + 64);
            const uint32_t modeHeight = PPC_LOAD_U32(mode + 68);
            if (modeWidth == 1280 && (modeHeight == 720 || modeHeight == 704)) {
                PPC_STORE_U32(mode + 64, width);
                PPC_STORE_U32(mode + 68, height);
                std::fprintf(stderr, "RUNTIME_WIDESCREEN mode index=%u %ux%u -> %ux%u\n", index,
                             modeWidth, modeHeight, width, height);
                std::fflush(stderr);
            }
        }
    }
    __imp__sub_8223E268(ctx, base);
}
