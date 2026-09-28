// Pure parts of the measured automatic settings (no Win32): statistics,
// the scale choice and the per-card record format. Unit-tested with
// synthetic timings (tests/register_helpers/runtime_gpu_calibration_test.cpp).
#include "runtime_gpu_calibration.h"
#include "../external/ReXGlue/include/rex/ui/frame_rate_policy.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

const RuntimeGpuCostModel& RuntimeReferenceGpuCostModel() {
    // Reference PC (a current 16 GB graphics card, an 8-core desktop CPU),
    // 2560x1440, GPU frame meter:
    // backdrop = main menu at the reduced menu rate (V339: 1.973 / 7.433 /
    // 17.342 ms; V350 re-measured 1.963 / 7.575 / 17.479, unchanged - the menu
    // is render-bound). street = the heaviest westward view, uncapped, with
    // the persistent upload path (V349+, no frame-end page reset): V350 1x
    // 1.56 ms @151 FPS and 2x 4.25 ms @143 (the command processor limits
    // both), 3x 9.05 ms @103 (V348 same view, GPU-bound: busy 93% of the
    // frame; 0.92 is the busy share). Before V349 the street was 2.50 / 5.05 /
    // 9.74 ms. scripts/measure-backdrop-scales.ps1,
    // scripts/measure-street-poses.ps1 -FrameMeter -NoGpuTiming.
    static const RuntimeGpuCostModel model{
        {0.0, 1.973, 7.433, 17.342},
        {0.0, 1.56, 4.25, 9.05},
        0.92,
    };
    return model;
}

RuntimeBackdropWindow RuntimeSummarizeBackdropWindow(const uint32_t* busyUs, size_t count) {
    RuntimeBackdropWindow window;
    if (!busyUs || !count) return window;
    std::vector<uint32_t> sorted(busyUs, busyUs + count);
    std::sort(sorted.begin(), sorted.end());
    window.frames = uint32_t(count);
    window.medianMs = double(sorted[count / 2]) / 1000.0;
    window.p90Ms = double(sorted[std::min(count - 1, count * 9 / 10)]) / 1000.0;
    window.stable = count >= 60 && window.medianMs > 0 && window.p90Ms <= window.medianMs * 1.35;
    return window;
}

double RuntimeAutomaticScaleTargetFps(std::string_view frameRate, uint32_t frameLimit,
                                      double refreshHz) {
    rex::ui::FrameRateMode mode = rex::ui::FrameRateMode::kRefresh;
    uint32_t limit = frameLimit;
    if (!rex::ui::ParseFrameRate(frameRate, mode, limit)) mode = rex::ui::FrameRateMode::kRefresh;
    const double refresh = refreshHz > 0 ? refreshHz : 60.0;
    switch (mode) {
        case rex::ui::FrameRateMode::kOriginal:
            return 60.0;
        case rex::ui::FrameRateMode::kRefresh:
        case rex::ui::FrameRateMode::kUncapped:
            for (const rex::ui::FrameRateOption& option :
                 rex::ui::FrameRateOptionsForDisplay(refresh)) {
                if (option.recommended) return option.fps;
            }
            return refresh;
        default: {
            const double target = rex::ui::ResolveFrameRatePolicy(mode, limit, true, refresh).target_fps;
            return target > 0 ? target : refresh;
        }
    }
}

bool RuntimeIsPlausibleBackdrop(uint32_t scale, double medianMs, const RuntimeGpuCostModel& model) {
    if (scale < 1 || scale > RuntimeGpuCostModel::kMaxScale || !(medianMs > 0)) return false;
    return medianMs / model.backdropMs[scale] >= kCalibrationMinCostFactor;
}

namespace {

// Rough video memory the title needs at each scale (render targets, scaled
// resolve memory, textures); a card below it stays at a lower scale.
uint64_t ScaleVideoMemoryFloor(uint32_t scale) {
    switch (scale) {
        case 1: return 0;
        case 2: return 3ull << 30;
        default: return 6ull << 30;
    }
}

}  // namespace

RuntimeScaleChoice RuntimeChooseMeasuredScale(const RuntimeScaleChoiceInputs& inputs,
                                              const RuntimeGpuCostModel& model) {
    RuntimeScaleChoice choice;
    const uint32_t measuredScale = inputs.backdrop.scale;
    if (measuredScale < 1 || measuredScale > RuntimeGpuCostModel::kMaxScale ||
        !(inputs.backdrop.medianMs > 0)) {
        choice.reason = "no_measurement";
        return choice;
    }
    choice.costFactor = inputs.backdrop.medianMs / model.backdropMs[measuredScale];
    choice.targetFps = std::clamp(inputs.targetFps > 0 ? inputs.targetFps : 60.0, 60.0, 120.0);
    for (uint32_t scale = 1; scale <= RuntimeGpuCostModel::kMaxScale; ++scale) {
        choice.predictedStreetMs[scale] = choice.costFactor * model.streetMs[scale];
        choice.predictedFrameMs[scale] =
            choice.predictedStreetMs[scale] / (model.gpuBusyShare > 0 ? model.gpuBusyShare : 1.0);
    }
    const uint32_t lines = inputs.outputHeight ? inputs.outputHeight : 1440;
    const uint32_t maxUseful = lines < 1000 ? 1 : lines < 1800 ? 2 : 3;
    // Slightly less than nominal is normal (16 GB cards report 15995 MB).
    const uint64_t slack = 512ull << 20;
    const auto memoryAllows = [&](uint32_t scale) {
        return !inputs.videoMemoryBytes ||
               inputs.videoMemoryBytes + slack >= ScaleVideoMemoryFloor(scale);
    };
    const auto fits = [&](uint32_t scale, double fps) {
        return choice.predictedFrameMs[scale] <= 1000.0 / fps * kAutoScaleHeadroom;
    };
    // Frames first: when no scale holds the target, 1x is the fastest choice.
    choice.scale = 1;
    choice.reason = "target_out_of_reach";
    for (uint32_t scale = maxUseful; scale >= 1; --scale) {
        if (memoryAllows(scale) && fits(scale, choice.targetFps)) {
            choice.scale = scale;
            choice.holdsTarget = true;
            choice.reason = "holds_target";
            break;
        }
    }
    choice.holds60 = fits(choice.scale, 60.0);
    return choice;
}

std::string RuntimeGpuCalibrationToToml(const RuntimeGpuCalibrationRecord& record) {
    toml::table table{
        {"model", int64_t(record.model)},
        {"gpu", record.gpu.description},
        {"vendor_id", int64_t(record.gpu.vendorId)},
        {"device_id", int64_t(record.gpu.deviceId)},
        {"video_memory_mb", int64_t(record.gpu.videoMemoryBytes >> 20)},
        {"scale", int64_t(record.backdrop.scale)},
        {"backdrop_median_us", int64_t(std::llround(record.backdrop.medianMs * 1000.0))},
        {"backdrop_p90_us", int64_t(std::llround(record.backdrop.p90Ms * 1000.0))},
        {"frames", int64_t(record.backdrop.frames)},
        {"measured", record.measured},
    };
    std::ostringstream stream;
    stream << "# Automatic settings: GPU time per frame of the title's menu backdrop on\n"
              "# this graphics card, measured by the game. Delete this file to measure\n"
              "# again at the next start.\n"
           << table << '\n';
    return stream.str();
}

std::optional<RuntimeGpuCalibrationRecord> RuntimeGpuCalibrationFromToml(std::string_view text) {
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }
    const auto integer = [&](const char* key) -> std::optional<int64_t> {
        return table[key].value<int64_t>();
    };
    const auto model = integer("model");
    const auto vendor = integer("vendor_id");
    const auto device = integer("device_id");
    const auto memory = integer("video_memory_mb");
    const auto scale = integer("scale");
    const auto median = integer("backdrop_median_us");
    const auto p90 = integer("backdrop_p90_us");
    const auto frames = integer("frames");
    if (!model || !vendor || !device || !memory || !scale || !median || !p90 || !frames ||
        *scale < 1 || *scale > RuntimeGpuCostModel::kMaxScale || *median <= 0 || *p90 < 0 ||
        *memory < 0 || *frames < 0) {
        return std::nullopt;
    }
    RuntimeGpuCalibrationRecord record;
    record.model = uint32_t(*model);
    record.gpu.description = table["gpu"].value_or(std::string{});
    record.gpu.vendorId = uint32_t(*vendor);
    record.gpu.deviceId = uint32_t(*device);
    record.gpu.videoMemoryBytes = uint64_t(*memory) << 20;
    record.backdrop.scale = uint32_t(*scale);
    record.backdrop.medianMs = double(*median) / 1000.0;
    record.backdrop.p90Ms = double(*p90) / 1000.0;
    record.backdrop.frames = uint32_t(*frames);
    record.measured = table["measured"].value_or(std::string{});
    return record;
}

bool RuntimeGpuCalibrationMatches(const RuntimeGpuCalibrationRecord& record,
                                  const RuntimeGpuIdentity& gpu) {
    return record.model == RuntimeGpuCostModel::kVersion && gpu.vendorId &&
           record.gpu.vendorId == gpu.vendorId && record.gpu.deviceId == gpu.deviceId &&
           (record.gpu.videoMemoryBytes >> 20) == (gpu.videoMemoryBytes >> 20);
}
