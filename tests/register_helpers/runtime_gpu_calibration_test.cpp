// Measured automatic settings (V338): decision logic with synthetic timings,
// since only one graphics card is available for real measurements.
#include "runtime_gpu_calibration.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const std::string& message) {
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message.c_str());
    return condition;
}

// A synthetic reference card (round numbers in the shape of the real one:
// the backdrop grows faster with the scale than the street does).
const RuntimeGpuCostModel kSyntheticModel{
    {0.0, 2.0, 7.0, 16.0},
    {0.0, 3.0, 6.0, 11.0},
    1.0,
};

// A card `factor` times slower than the synthetic reference, measured at
// `scale`.
RuntimeScaleChoice Choose(double factor, uint32_t scale, uint32_t outputHeight, double targetFps,
                          uint64_t memoryGb = 16) {
    const RuntimeGpuCostModel& model = kSyntheticModel;
    RuntimeScaleChoiceInputs inputs;
    inputs.backdrop.scale = scale;
    inputs.backdrop.medianMs = model.backdropMs[scale] * factor;
    inputs.backdrop.p90Ms = inputs.backdrop.medianMs * 1.05;
    inputs.backdrop.frames = 120;
    inputs.outputHeight = outputHeight;
    inputs.targetFps = targetFps;
    inputs.videoMemoryBytes = memoryGb << 30;
    return RuntimeChooseMeasuredScale(inputs, model);
}

}  // namespace

int main() {
    bool passed = true;
    const RuntimeGpuCostModel& model = RuntimeReferenceGpuCostModel();

    // The shipped model is ordered: more scale costs more on both workloads.
    for (uint32_t scale = 1; scale < RuntimeGpuCostModel::kMaxScale; ++scale) {
        passed &= Check(model.backdropMs[scale] > 0 &&
                            model.backdropMs[scale + 1] > model.backdropMs[scale] &&
                            model.streetMs[scale + 1] > model.streetMs[scale],
                        "reference costs must grow with the scale");
    }

    // Statistics: median/p90, stability, minimum frames.
    {
        std::vector<uint32_t> steady(120, 7800);
        for (size_t i = 0; i < 12; ++i) steady[i] = 8200;
        const RuntimeBackdropWindow window =
            RuntimeSummarizeBackdropWindow(steady.data(), steady.size());
        passed &= Check(window.frames == 120 && std::fabs(window.medianMs - 7.8) < 1e-9 &&
                            window.stable,
                        "a steady window is stable with the right median");
        std::vector<uint32_t> spiky(120, 2000);
        for (size_t i = 0; i < 30; ++i) spiky[i] = 9000;
        passed &= Check(!RuntimeSummarizeBackdropWindow(spiky.data(), spiky.size()).stable,
                        "a window with a heavy tail (screen change) is not stable");
        passed &= Check(!RuntimeSummarizeBackdropWindow(steady.data(), 40).stable,
                        "fewer than 60 frames are not a window");
        passed &= Check(RuntimeSummarizeBackdropWindow(nullptr, 0).frames == 0,
                        "an empty window is empty");
    }

    // Plausibility: logo movies and 2D screens (~0.1x the backdrop) are not
    // the backdrop; a card 2x faster than the reference still is.
    passed &= Check(!RuntimeIsPlausibleBackdrop(2, 0.7, kSyntheticModel),
                    "a 0.7 ms 2x window is not the backdrop");
    passed &= Check(RuntimeIsPlausibleBackdrop(2, kSyntheticModel.backdropMs[2] * 0.5,
                                               kSyntheticModel),
                    "a card twice as fast is plausible");
    passed &= Check(!RuntimeIsPlausibleBackdrop(0, 5.0, kSyntheticModel) &&
                        !RuntimeIsPlausibleBackdrop(4, 5.0, kSyntheticModel),
                    "only scales 1..3 are measured");

    // The reference card on a 1440p 144 Hz display: 2x (target clamped to 120).
    {
        const RuntimeScaleChoice choice = Choose(1.0, 2, 1440, 144.0);
        passed &= Check(choice.scale == 2 && choice.holdsTarget && choice.targetFps == 120.0 &&
                            std::fabs(choice.costFactor - 1.0) < 1e-9,
                        "the reference card keeps 2x at 1440p/144 Hz");
        // Measuring at 1x or 3x predicts the same.
        passed &= Check(Choose(1.0, 1, 1440, 144.0).scale == 2 &&
                            Choose(1.0, 3, 1440, 144.0).scale == 2,
                        "the measured scale does not change the choice");
        // A 10% noisier measurement must not flip it.
        passed &= Check(Choose(1.1, 2, 1440, 144.0).scale == 2,
                        "10% measurement noise keeps 2x on the reference card");
    }
    // 4K: 3x only where it holds the target (60 Hz yes, 144 Hz no).
    passed &= Check(Choose(1.0, 2, 2160, 60.0).scale == 3, "reference card, 4K 60 Hz: 3x");
    passed &= Check(Choose(1.0, 2, 2160, 144.0).scale == 2, "reference card, 4K 144 Hz: 2x");
    // Screens below 1000 lines stay at 1x; 1080p may use 2x (supersampling).
    passed &= Check(Choose(0.5, 2, 800, 60.0).scale == 1, "800p screens: 1x");
    passed &= Check(Choose(1.0, 2, 1080, 60.0).scale == 2, "1080p 60 Hz: 2x on the reference");
    // A mid card (2x slower): 2x on a 60 Hz display, 1x on 144 Hz.
    passed &= Check(Choose(2.0, 2, 1440, 60.0).scale == 2, "2x slower card, 60 Hz: 2x");
    passed &= Check(Choose(2.0, 2, 1440, 144.0).scale == 1, "2x slower card, 144 Hz: 1x");
    // A weak card (4x slower): 1x everywhere, still holding 60 at 1x.
    {
        const RuntimeScaleChoice choice = Choose(4.0, 2, 1440, 60.0);
        passed &= Check(choice.scale == 1 && choice.holdsTarget && choice.holds60,
                        "4x slower card: 1x holding 60");
    }
    // An integrated GPU (12x slower): 1x, and honestly below 60.
    {
        const RuntimeScaleChoice choice = Choose(12.0, 2, 1080, 60.0);
        passed &= Check(choice.scale == 1 && !choice.holdsTarget && !choice.holds60 &&
                            std::string(choice.reason) == "target_out_of_reach",
                        "12x slower GPU: 1x below 60");
    }
    // Frames first: when 1x misses a 120 target, 1x (not a higher scale).
    passed &= Check(Choose(3.0, 2, 1440, 144.0).scale == 1,
                    "a target out of reach picks the fastest scale");
    // Video memory floors: a fast 2 GB card stays at 1x.
    passed &= Check(Choose(0.5, 2, 1440, 60.0, 2).scale == 1, "2 GB: 1x");
    passed &= Check(Choose(0.5, 2, 2160, 60.0, 4).scale == 2, "4 GB at 4K: at most 2x");
    // Target: clamped to 60..120; unknown means 60.
    passed &= Check(Choose(1.0, 2, 1440, 30.0).targetFps == 60.0 &&
                        Choose(1.0, 2, 1440, 240.0).targetFps == 120.0 &&
                        Choose(1.0, 2, 1440, 0.0).targetFps == 60.0,
                    "the target is clamped to 60..120");
    // No measurement: 1x with a reason (callers keep their guess instead).
    {
        RuntimeScaleChoiceInputs inputs;
        passed &= Check(std::string(RuntimeChooseMeasuredScale(inputs, kSyntheticModel).reason) ==
                            "no_measurement",
                        "no measurement is reported");
    }

    // The shipped model keeps the reference card's own experience: 2x on its
    // 1440p 144 Hz display (with 5% measurement noise), 3x on a 4K 60 Hz
    // screen, 2x on a 4K 144 Hz screen; a card half as fast gets 1x at 144 Hz.
    {
        const auto reference = [&](double factor, uint32_t lines, double fps) {
            RuntimeScaleChoiceInputs inputs;
            inputs.backdrop.scale = 2;
            inputs.backdrop.medianMs = model.backdropMs[2] * factor;
            inputs.backdrop.p90Ms = inputs.backdrop.medianMs;
            inputs.backdrop.frames = 60;
            inputs.outputHeight = lines;
            inputs.targetFps = fps;
            inputs.videoMemoryBytes = 16ull << 30;
            return RuntimeChooseMeasuredScale(inputs).scale;
        };
        passed &= Check(reference(1.0, 1440, 144.0) == 2 && reference(1.05, 1440, 144.0) == 2,
                        "shipped model: the reference card keeps 2x at 1440p/144 Hz");
        passed &= Check(reference(1.0, 2160, 60.0) == 3, "shipped model: reference, 4K 60 Hz: 3x");
        passed &= Check(reference(1.0, 2160, 144.0) == 2, "shipped model: reference, 4K 144 Hz: 2x");
        passed &= Check(reference(2.0, 1440, 144.0) == 1 && reference(2.0, 1440, 60.0) == 2,
                        "shipped model: a card half as fast gets 1x at 144 Hz, 2x at 60 Hz");
    }

    // Target frame rate from display.frame_rate.
    passed &= Check(RuntimeAutomaticScaleTargetFps("refresh", 0, 144.0) == 144.0 &&
                        RuntimeAutomaticScaleTargetFps("uncapped", 0, 240.0) == 120.0 &&
                        RuntimeAutomaticScaleTargetFps("60", 0, 144.0) == 60.0 &&
                        RuntimeAutomaticScaleTargetFps("half_refresh", 0, 144.0) == 72.0 &&
                        RuntimeAutomaticScaleTargetFps("100", 0, 144.0) == 100.0 &&
                        RuntimeAutomaticScaleTargetFps("custom", 90, 144.0) == 90.0 &&
                        RuntimeAutomaticScaleTargetFps("original", 0, 144.0) == 60.0 &&
                        RuntimeAutomaticScaleTargetFps("refresh", 0, 0.0) == 60.0,
                    "target frame rates follow display.frame_rate");

    // Record round trip and matching.
    {
        RuntimeGpuCalibrationRecord record;
        record.gpu.description = "NVIDIA GeForce RTX 5070 Ti";
        record.gpu.vendorId = 0x10DE;
        record.gpu.deviceId = 0x2C05;
        record.gpu.videoMemoryBytes = 15995ull << 20;
        record.backdrop.scale = 2;
        record.backdrop.medianMs = 7.775;
        record.backdrop.p90Ms = 8.012;
        record.backdrop.frames = 120;
        record.measured = "2026-09-26T17:00:00Z";
        const std::string text = RuntimeGpuCalibrationToToml(record);
        const auto parsed = RuntimeGpuCalibrationFromToml(text);
        passed &= Check(parsed && parsed->gpu.description == record.gpu.description &&
                            parsed->gpu.vendorId == record.gpu.vendorId &&
                            parsed->gpu.deviceId == record.gpu.deviceId &&
                            parsed->gpu.videoMemoryBytes == record.gpu.videoMemoryBytes &&
                            parsed->backdrop.scale == 2 &&
                            std::fabs(parsed->backdrop.medianMs - 7.775) < 1e-9 &&
                            std::fabs(parsed->backdrop.p90Ms - 8.012) < 1e-9 &&
                            parsed->backdrop.frames == 120 && parsed->measured == record.measured &&
                            parsed->model == RuntimeGpuCostModel::kVersion,
                        "the record survives a round trip");
        passed &= Check(parsed && RuntimeGpuCalibrationMatches(*parsed, record.gpu),
                        "the record matches its card");
        RuntimeGpuIdentity other = record.gpu;
        other.deviceId = 0x2D04;
        passed &= Check(parsed && !RuntimeGpuCalibrationMatches(*parsed, other),
                        "another card measures again");
        RuntimeGpuCalibrationRecord oldModel = *parsed;
        oldModel.model = RuntimeGpuCostModel::kVersion + 1;
        passed &= Check(!RuntimeGpuCalibrationMatches(oldModel, record.gpu),
                        "another model version measures again");
        passed &= Check(!RuntimeGpuCalibrationFromToml("scale = 9\n") &&
                            !RuntimeGpuCalibrationFromToml("not toml [") &&
                            !RuntimeGpuCalibrationFromToml(""),
                        "invalid records are ignored");
    }

    std::printf(passed ? "runtime_gpu_calibration: all checks passed\n"
                       : "runtime_gpu_calibration: FAILED\n");
    return passed ? 0 : 1;
}
