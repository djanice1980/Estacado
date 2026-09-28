// Measured automatic settings (V338): the Win32 side and the in-game
// measurement. See runtime_gpu_calibration.h.
#include "runtime_gpu_calibration.h"
#include "runtime_graphics.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {

// Only the main menu shows the 3D backdrop (the title screen, logo movies and
// 2D screens cost a fraction of it): its screen class (the vtable of the
// front end's active screen object, fixed in the XEX).
constexpr uint32_t kMainMenuScreenClass = 0x82079088;  // V339: logos 0x82078228,
                                                      // title/2D 0x82077FB8
// Frames on the main menu before sampling (screen transition, steady
// clocks), and frames per sampled window: about 1 + 2 x 1.3 s at the reduced
// menu rate, which menus keep meanwhile (the reference model was measured
// there too).
constexpr uint32_t kSettleFrames = 48;
constexpr uint32_t kWindowFrames = 60;
// Two consecutive plausible windows within this relative spread finish.
constexpr double kAgreement = 0.15;

// Guest menu thread only (RuntimeGpuCalibrationOnGuestFrame).
struct Session {
    bool armed = false;
    std::filesystem::path path;
    RuntimeGpuIdentity gpu;
    uint32_t scale = 0;
    uint32_t outputHeight = 0;
    double targetFps = 0;
    uint32_t screen = 0;
    uint32_t screenFrames = 0;
    bool sampling = false;
    uint64_t windowStart = 0;
    bool havePrevious = false;
    RuntimeBackdropWindow previous;
    uint32_t windows = 0;
};
Session session;

std::string UtcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

bool WriteRecord(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << text;
        if (!file.flush()) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void Finish(const RuntimeBackdropWindow& a, const RuntimeBackdropWindow& b) {
    // The heavier of the two agreeing windows (conservative).
    const RuntimeBackdropWindow& window = a.medianMs >= b.medianMs ? a : b;
    RuntimeGpuCalibrationRecord record;
    record.gpu = session.gpu;
    record.backdrop.scale = session.scale;
    record.backdrop.medianMs = window.medianMs;
    record.backdrop.p90Ms = window.p90Ms;
    record.backdrop.frames = window.frames;
    record.measured = UtcNow();
    const bool saved = WriteRecord(session.path, RuntimeGpuCalibrationToToml(record));
    RuntimeScaleChoiceInputs inputs;
    inputs.backdrop = record.backdrop;
    inputs.outputHeight = session.outputHeight;
    inputs.targetFps = session.targetFps;
    inputs.videoMemoryBytes = session.gpu.videoMemoryBytes;
    const RuntimeScaleChoice choice = RuntimeChooseMeasuredScale(inputs);
    std::printf(
        "RUNTIME_GPU_CALIBRATION saved=%u scale=%u backdrop_median_ms=%.2f backdrop_p90_ms=%.2f "
        "cost_factor=%.2f target_fps=%.0f street_ms_1x=%.1f street_ms_2x=%.1f "
        "street_ms_3x=%.1f next_start_scale=%u holds_target=%u holds_60=%u reason=%s "
        "windows=%u\n",
        saved ? 1u : 0u, session.scale, window.medianMs, window.p90Ms, choice.costFactor,
        choice.targetFps, choice.predictedStreetMs[1], choice.predictedStreetMs[2],
        choice.predictedStreetMs[3], choice.scale, choice.holdsTarget ? 1u : 0u,
        choice.holds60 ? 1u : 0u, choice.reason, session.windows);
    std::fflush(stdout);
    session.armed = false;
}

}  // namespace

void RuntimeGpuCalibrationArm(const std::filesystem::path& recordPath,
                              const RuntimeGpuIdentity& gpu, uint32_t scale,
                              uint32_t outputHeight, double targetFps) {
    if (scale < 1 || scale > RuntimeGpuCostModel::kMaxScale || !gpu.vendorId) return;
    session = Session{};
    session.armed = true;
    session.path = recordPath;
    session.gpu = gpu;
    session.scale = scale;
    session.outputHeight = outputHeight;
    session.targetFps = targetFps;
}

namespace {

// DARKNESS_GPU_FRAME_METER_LOG=1 (test launches): the meter's median/p90 of
// the frames since the previous line, about every 60 guest frames (stderr,
// in order with the test input's phase marks).
void LogFrameMeter(bool frontEnd, uint32_t screen) {
    static const bool enabled = [] {
        char value[8]{};
        return GetEnvironmentVariableA("DARKNESS_GPU_FRAME_METER_LOG", value, sizeof(value)) &&
               value[0] == '1';
    }();
    if (!enabled) return;
    static uint32_t frames = 0;
    static uint64_t lastTotal = 0;
    if (++frames < 60) return;
    frames = 0;
    std::array<uint32_t, 512> busy{};
    uint64_t total = 0;
    RuntimeGraphicsGpuFrameBusyUs(nullptr, 0, &total);
    const uint32_t fresh = uint32_t((std::min<uint64_t>)(total - lastTotal, busy.size()));
    lastTotal = total;
    if (!fresh) return;
    const uint32_t count = RuntimeGraphicsGpuFrameBusyUs(busy.data(), fresh, &total);
    const RuntimeBackdropWindow window = RuntimeSummarizeBackdropWindow(busy.data(), count);
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < count; ++i) maximum = (std::max)(maximum, busy[i]);
    std::fprintf(stderr,
                 "RUNTIME_GPU_FRAME_METER frames=%u median_ms=%.3f p90_ms=%.3f max_ms=%.3f "
                 "front_end=%u total=%llu tick_ms=%llu screen=0x%08X\n",
                 count, window.medianMs, window.p90Ms, double(maximum) / 1000.0,
                 frontEnd ? 1u : 0u, static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(GetTickCount64()), screen);
    std::fflush(stderr);
}

}  // namespace

void RuntimeGpuCalibrationOnGuestFrame(bool frontEnd, uint32_t screen) {
    LogFrameMeter(frontEnd, screen);
    if (!session.armed) return;
    const bool mainMenu = frontEnd && (!kMainMenuScreenClass || screen == kMainMenuScreenClass);
    if (!mainMenu || screen != session.screen) {
        session.screen = mainMenu ? screen : 0;
        session.screenFrames = 0;
        session.sampling = false;
        session.havePrevious = false;
        if (!mainMenu) return;
    }
    if (++session.screenFrames < kSettleFrames) return;
    uint64_t total = 0;
    RuntimeGraphicsGpuFrameBusyUs(nullptr, 0, &total);
    if (!session.sampling) {
        session.sampling = true;
        session.windowStart = total;
        return;
    }
    if (total < session.windowStart + kWindowFrames) return;
    std::array<uint32_t, kWindowFrames> busy{};
    const uint32_t count = RuntimeGraphicsGpuFrameBusyUs(busy.data(), kWindowFrames, &total);
    session.windowStart = total;
    const RuntimeBackdropWindow window = RuntimeSummarizeBackdropWindow(busy.data(), count);
    const bool plausible = RuntimeIsPlausibleBackdrop(session.scale, window.medianMs);
    ++session.windows;
    std::printf("RUNTIME_GPU_CALIBRATION_WINDOW scale=%u median_ms=%.2f p90_ms=%.2f frames=%u "
                "stable=%u plausible=%u screen=0x%08X\n",
                session.scale, window.medianMs, window.p90Ms, window.frames,
                window.stable ? 1u : 0u, plausible ? 1u : 0u, screen);
    std::fflush(stdout);
    if (!window.stable || !plausible) {
        session.havePrevious = false;
        return;
    }
    if (session.havePrevious &&
        std::fabs(window.medianMs - session.previous.medianMs) <=
            kAgreement * (std::max)(window.medianMs, session.previous.medianMs)) {
        Finish(session.previous, window);
        return;
    }
    session.previous = window;
    session.havePrevious = true;
}
