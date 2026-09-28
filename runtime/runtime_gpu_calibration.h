#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Automatic settings, measured (V338): the title's 3D menu backdrop is a
// ready-made GPU benchmark. With Automatic internal scale and no measurement
// for this graphics card yet, the runtime times the backdrop's GPU work per
// frame (the GPU frame meter) on the title/main menu, stores the result per
// card in the user data folder, and every later start picks the scale from
// it. Until a measurement exists the video-memory guess applies
// (runtime_auto_scale.h). A manual scale is never changed.

// Reference card: the one the model was measured on. A measured backdrop time
// divided by the reference time at the same scale gives the card's GPU cost
// factor; the busiest street view's GPU busy time is predicted as that factor
// times the reference street cost, and its GPU-limited frame time as that
// divided by the share of a GPU-limited frame the GPU is actually busy (work
// is serialized with the command processor within a frame: measured at 3x on
// the reference card, where the GPU sets the frame rate).
struct RuntimeGpuCostModel {
    static constexpr uint32_t kVersion = 3;
    static constexpr uint32_t kMaxScale = 3;
    // GPU busy time per frame (ms) on the reference card, indexed by scale
    // (1..3): the main-menu backdrop and the busiest street view.
    double backdropMs[kMaxScale + 1];
    double streetMs[kMaxScale + 1];
    // GPU busy time / frame time when the GPU limits the frame rate.
    double gpuBusyShare;
};
const RuntimeGpuCostModel& RuntimeReferenceGpuCostModel();

// Fraction of the target frame time the predicted GPU-limited frame time of
// the busiest street view may use (the busy share already makes it realistic).
inline constexpr double kAutoScaleHeadroom = 1.0;
// Measurements implying a card this much faster than the reference are not
// the backdrop (logo movies and 2D title screens cost ~0.1x of it).
inline constexpr double kCalibrationMinCostFactor = 0.4;

struct RuntimeBackdropWindow {
    double medianMs = 0;
    double p90Ms = 0;
    uint32_t frames = 0;
    bool stable = false;
};
// Median and 90th percentile of per-frame GPU busy times (microseconds); a
// window is stable with at least 60 frames and p90 within 1.35x the median.
RuntimeBackdropWindow RuntimeSummarizeBackdropWindow(const uint32_t* busyUs, size_t count);

struct RuntimeMeasuredBackdrop {
    uint32_t scale = 0;  // internal scale during the measurement
    double medianMs = 0;
    double p90Ms = 0;
    uint32_t frames = 0;
};

struct RuntimeScaleChoiceInputs {
    RuntimeMeasuredBackdrop backdrop;
    uint32_t outputHeight = 0;      // 0: unknown (treated as 1440 lines)
    double targetFps = 0;           // the frame rate to hold; clamped to 60..120
    uint64_t videoMemoryBytes = 0;  // 0: unknown (no memory limit)
};

struct RuntimeScaleChoice {
    uint32_t scale = 1;
    double costFactor = 0;      // measured / reference backdrop time
    double targetFps = 60;      // after clamping
    double predictedStreetMs[RuntimeGpuCostModel::kMaxScale + 1] = {};  // GPU busy
    double predictedFrameMs[RuntimeGpuCostModel::kMaxScale + 1] = {};   // GPU-limited
    bool holdsTarget = false;   // the busiest street stays within the target
    bool holds60 = false;       // at the chosen scale
    const char* reason = "";
};
// The highest useful scale (1 below 1000 output lines, 2 up to 1800, 3 above)
// whose predicted GPU-limited frame time of the busiest street view fits
// kAutoScaleHeadroom of the target frame time; otherwise 1x, the fastest
// (frames first).
RuntimeScaleChoice RuntimeChooseMeasuredScale(const RuntimeScaleChoiceInputs& inputs,
                                              const RuntimeGpuCostModel& model =
                                                  RuntimeReferenceGpuCostModel());

// The frame rate the automatic scale should hold for a display.frame_rate
// value (frameLimit: display.frame_limit for "custom"): a fixed cap is
// itself; the display refresh and uncapped use the display's recommended
// rate (144 on 144 Hz); original uses 60. refreshHz <= 0: unknown (60 Hz).
double RuntimeAutomaticScaleTargetFps(std::string_view frameRate, uint32_t frameLimit,
                                      double refreshHz);

// True when a window is plausibly the backdrop at that scale.
bool RuntimeIsPlausibleBackdrop(uint32_t scale, double medianMs,
                                const RuntimeGpuCostModel& model = RuntimeReferenceGpuCostModel());

struct RuntimeGpuIdentity {
    std::string description;  // UTF-8
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t videoMemoryBytes = 0;
};

struct RuntimeGpuCalibrationRecord {
    uint32_t model = RuntimeGpuCostModel::kVersion;
    RuntimeGpuIdentity gpu;
    RuntimeMeasuredBackdrop backdrop;
    std::string measured;  // UTC ISO 8601
};
std::string RuntimeGpuCalibrationToToml(const RuntimeGpuCalibrationRecord& record);
std::optional<RuntimeGpuCalibrationRecord> RuntimeGpuCalibrationFromToml(std::string_view text);
// The record applies to this card with the current model.
bool RuntimeGpuCalibrationMatches(const RuntimeGpuCalibrationRecord& record,
                                  const RuntimeGpuIdentity& gpu);

// --- Runtime (Win32) -------------------------------------------------------

// The high-performance adapter (the one the renderer uses).
RuntimeGpuIdentity RuntimeDetectGpuIdentity();
std::filesystem::path RuntimeGpuCalibrationPath(const std::filesystem::path& userDataRoot);
std::optional<RuntimeGpuCalibrationRecord> RuntimeLoadGpuCalibration(
    const std::filesystem::path& path);

// Starts measuring on the next title/main-menu backdrop (Automatic scale
// without a record for this card). The result is stored at recordPath and
// applies from the next start.
void RuntimeGpuCalibrationArm(const std::filesystem::path& recordPath,
                              const RuntimeGpuIdentity& gpu, uint32_t scale,
                              uint32_t outputHeight, double targetFps);
// Called once per guest frame by the menu detection: frontEnd is true on the
// title and main-menu screens (no game client, a menu window), screen
// identifies the menu window.
void RuntimeGpuCalibrationOnGuestFrame(bool frontEnd, uint32_t screen);
