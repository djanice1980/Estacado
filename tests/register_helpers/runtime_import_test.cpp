#include <cstdint>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include <ppc_recomp_shared.h>

#include "runtime_memory.h"
#include "runtime_modules.h"
#include "runtime_audio.h"
#include "runtime_camera.h"
#include "runtime_camera_policy.h"
#include "runtime_filesystem.h"
#include "runtime_function_trace.h"
#include "runtime_graphics.h"
#include "runtime_input.h"
#include "runtime_objects.h"
#include "runtime_sync.h"
#include "runtime_tls.h"
#include "runtime_threads.h"
#include "runtime_title_notifications.h"
#include "runtime_xam.h"

namespace {
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
constexpr uint32_t kStatusPending = 0x00000103;
constexpr uint32_t kStatsAddress = 0x100;
constexpr uint32_t kStatsSize = 104;

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool Query(PPCContext& context, uint8_t* base) {
    context.r3.u64 = kStatsAddress;
    __imp__MmQueryStatistics(context, base);
    return Check(context.r3.u32 == 0, "MmQueryStatistics returned failure");
}
}

int main(int argc, char** argv) {
    // Offline cost check of the real entry/interrupt gates, never a game FPS
    // measurement. This mode creates no guest threads, window or GPU device.
    if (argc == 2 && std::strcmp(argv[1], "--benchmark-empty-entry") == 0) {
        PPCContext context{};
        constexpr uint32_t iterations = 2000000;
        for (unsigned pass = 0; pass < 4; ++pass) {
            const auto begin = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < iterations; ++i)
                RuntimeFunctionEnter(context, nullptr, 0x820E2E50u);
            const auto middle = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < iterations; ++i) {
                if (DeliverRuntimeGraphicsInterrupts(context, nullptr)) return 1;
            }
            const auto end = std::chrono::steady_clock::now();
            std::cout << "EMPTY_ENTRY_BENCH pass=" << pass
                      << " iterations=" << iterations << " entry_ns="
                      << std::chrono::duration<double, std::nano>(middle - begin).count() / iterations
                      << " interrupt_ns="
                      << std::chrono::duration<double, std::nano>(end - middle).count() / iterations
                      << '\n';
        }
        return 0;
    }
    uint32_t translated = UINT32_MAX;
    if (!Check(RuntimeGraphicsGuestPhysicalRange(0xA0001234u, 64, translated) && translated == 0x1234u &&
               RuntimeGraphicsGuestPhysicalRange(0xC0001234u, 64, translated) && translated == 0x1234u &&
               RuntimeGraphicsGuestPhysicalRange(0xE0000234u, 64, translated) && translated == 0x1234u &&
               RuntimeGraphicsGuestPhysicalRange(0x7F001234u, 64, translated) && translated == 0x1234u,
               "physical source aliases disagree")) return 1;
    translated = UINT32_MAX;
    if (!Check(!RuntimeGraphicsGuestPhysicalRange(0x7FFFFFF0u, 32, translated) &&
               !RuntimeGraphicsGuestPhysicalRange(0xFFCFFFF0u, 32, translated) &&
               !RuntimeGraphicsGuestPhysicalRange(0xBFFFFFF0u, 32, translated) &&
               !RuntimeGraphicsGuestPhysicalRange(0xDFFFFFF0u, 32, translated) &&
               !RuntimeGraphicsGuestPhysicalRange(0x80000000u, 1, translated) &&
               !RuntimeGraphicsGuestPhysicalRange(0xA0000000u, 0, translated) && translated == UINT32_MAX,
               "cross-aperture source range accepted or output overwritten")) return 1;
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    alignas(32) uint8_t base[0x10000]{};
    PPCContext context{};

    context.r3.u64 = 0x123456789ABCDEF0ull;
    context.lr = 0x820E2E50u;
    if (!Check(!DeliverRuntimeGraphicsInterrupts(context, nullptr) &&
                   context.r3.u64 == 0x123456789ABCDEF0ull &&
                   context.lr == 0x820E2E50u,
               "empty interrupt dispatch accessed guest memory or changed registers")) return 1;
    RequestGuestRuntimeStop();
    bool entryStopped = false;
    try {
        RuntimeFunctionEnter(context, nullptr, 0x820E2E50u);
    } catch (const GuestRuntimeStop&) {
        entryStopped = true;
    }
    ResetGuestRuntimeStop();
    if (!Check(entryStopped, "ordinary entry no longer honors the stop request")) return 1;
    context = {};

    // Exercise the production camera dispatch: unrelated entries with tracing
    // disabled need no guest memory, while the real initial FOV setter remains
    // active and must still distinguish its owner/caller from other cameras.
    ConfigureRuntimeCamera(107.0f, false);
    context.r3.u32 = UINT32_MAX;
    context.f1.f64 = 61.0;
    for (uint32_t address : {0u, 0x820E2E50u, 0x82114838u, 0x8224A2E8u,
                             0x82389C9Cu, 0x82389CFCu, 0x82498F68u,
                             0x8259D3B8u, 0x825EB6B8u, 0x8285D208u, UINT32_MAX}) {
        RuntimeCameraFunctionEnter(context, nullptr, address);
        if (!Check(context.r3.u32 == UINT32_MAX && context.f1.f64 == 61.0,
                   "disabled camera observation changed an unrelated guest entry")) return 1;
    }
    constexpr uint32_t kTestViewport = 0xF000u;
    PPC_STORE_U32(kTestViewport, kMainViewportVtable);
    PPC_STORE_U32(kTestViewport + 276u, 0u);
    context.r3.u32 = kTestViewport;
    context.lr = kMainViewportInitialFovCaller;
    RuntimeCameraFunctionEnter(context, base, kViewportSetCurrentFovFunction);
    if (!Check(context.f1.f64 == 61.0 && PPC_LOAD_U32(kTestViewport + 276u) == 0u,
               "the main viewport's FOV setter is observed, never overridden (V376)")) return 1;
    // V376: the field of view scales the client's per-frame view copy only.
    constexpr uint32_t kTestView = 0xF400u;
    const auto viewFov = [&]() {
        const uint32_t bits = PPC_LOAD_U32(kTestView + kClientViewFovOffset);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const auto setViewFov = [&](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        PPC_STORE_U32(kTestView + kClientViewFovOffset, bits);
    };
    setViewFov(70.0f);
    context.r4.u32 = kTestView;
    context.r5.u32 = kClientViewBytes;
    context.lr = kClientViewCopyReturn;
    if (!Check(g_runtime_view_fov_active.load() == 1u,
               "a wider field of view left the view scaler off")) return 1;
    RuntimeScaleClientViewFov(context, base);
    const float expectedFov = RuntimeScaledViewFov(70.0f, RuntimeGameplayFovTangentScale(107.0f));
    if (!Check(std::fabs(viewFov() - expectedFov) < 1e-4f && viewFov() > 70.0f,
               "the client view copy was not scaled")) return 1;
    setViewFov(70.0f);
    context.lr = 0x8249A1ECu;  // the snapshot copy of the same view
    RuntimeScaleClientViewFov(context, base);
    if (!Check(viewFov() == 70.0f, "another memcpy caller was scaled")) return 1;
    ConfigureRuntimeCamera(kOriginalGameplayFovDegrees, false);
    if (!Check(g_runtime_view_fov_active.load() == 0u,
               "the original field of view left the view scaler on")) return 1;
    context.lr = kClientViewCopyReturn;
    RuntimeScaleClientViewFov(context, base);
    if (!Check(viewFov() == 70.0f, "the original field of view changed the view")) return 1;
    context = {};

    const uint32_t originalHostFpscr = context.fpscr.getcsr();
#if defined(__x86_64__) || defined(_M_X64)
    // Reproduce a zero-context-style unmasked host state without retaining
    // pending exception flags, then prove runtime initialization restores all
    // native exception masks before generated floating-point code can run.
    context.fpscr.setcsr(originalHostFpscr & ~(0x1F80u | 0x3Fu));
    InitializeGuestFloatingPointHostState(context);
    const uint32_t initializedHostFpscr = context.fpscr.getcsr();
    context.fpscr.setcsr(originalHostFpscr);
    if (!Check((context.fpscr.csr & 0x1F80u) == 0x1F80u &&
                   (initializedHostFpscr & 0x1F80u) == 0x1F80u,
               "guest context initialization left native floating-point exceptions enabled"))
        return 1;
#elif defined(__aarch64__) || defined(_M_ARM64)
    context.fpscr.setcsr((originalHostFpscr | 0x9F00u) & ~0x3Fu);
    InitializeGuestFloatingPointHostState(context);
    const uint32_t initializedHostFpscr = context.fpscr.getcsr();
    context.fpscr.setcsr(originalHostFpscr);
    if (!Check((context.fpscr.csr & 0x9F00u) == 0 &&
                   (initializedHostFpscr & 0x9F00u) == 0,
               "guest context initialization left native floating-point exceptions enabled"))
        return 1;
#endif

    for (uint8_t processor = 0; processor < 6; ++processor) {
        uint8_t observed = 0xFF;
        if (!Check(GuestProcessorNumberFromAffinity(1u << processor, &observed) &&
                       observed == processor,
                   "single-bit Xenon affinity did not map to its processor number"))
            return 1;
    }
    uint8_t invalidProcessor{};
    if (!Check(!GuestProcessorNumberFromAffinity(0, &invalidProcessor) &&
                   !GuestProcessorNumberFromAffinity(3, &invalidProcessor) &&
                   !GuestProcessorNumberFromAffinity(0x40, &invalidProcessor),
               "invalid Xenon affinity mask was accepted"))
        return 1;
    uint32_t creationAffinity = 0xFFFFFFFFu;
    if (!Check(GuestCreationAffinity(0x10000001u, &creationAffinity) &&
                   creationAffinity == 0x10u &&
                   GuestCreationAffinity(0x00000001u, &creationAffinity) &&
                   creationAffinity == 0u &&
                   !GuestCreationAffinity(0x03000001u, &creationAffinity) &&
                   !GuestCreationAffinity(0x40000001u, &creationAffinity),
               "ExCreateThread top-byte processor affinity was not decoded safely"))
        return 1;

    constexpr uint32_t kSystemCommandState = 0x100;
    constexpr uint32_t kSystemCommandToken = 0x200;
    std::memset(base + kSystemCommandState, 0xCD, 0x94);
    context.r3.u64 = kSystemCommandState;
    context.r4.u64 = kSystemCommandToken;
    __imp__VdGetSystemCommandBuffer(context, base);
    bool systemCommandTailZero = true;
    for (uint32_t offset = 4; offset < 0x94; ++offset) {
        systemCommandTailZero &= base[kSystemCommandState + offset] == 0;
    }
    if (!Check(PPC_LOAD_U32(kSystemCommandState) == 0xBEEF0000u &&
                   PPC_LOAD_U32(kSystemCommandToken) == 0xBEEF0001u &&
                   systemCommandTailZero,
               "VdGetSystemCommandBuffer did not publish its pinned 0x94-byte contract"))
        return 1;

    context = {};
    context.r3.u64 = 0x48000000u;
    __imp__VdSetDisplayMode(context, base);
    if (!Check(context.r3.u32 == 0,
               "VdSetDisplayMode did not preserve the pinned return contract"))
        return 1;

    constexpr uint32_t kDisplayInfoAddress = 0x280;
    constexpr uint32_t kDisplayInfoBytes = 0x58;
    std::memset(base + kDisplayInfoAddress, 0xA5, kDisplayInfoBytes);
    context = {};
    context.r3.u64 = kDisplayInfoAddress;
    __imp__VdGetCurrentDisplayInformation(context, base);
    if (!Check(PPC_LOAD_U16(kDisplayInfoAddress + 0x00) == 1280 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x02) == 720 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x10) == 1280 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x14) == 720 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x18) == 1280 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x1C) == 720 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x20) == 1 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x30) == 1 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x40) == 320 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x42) == 180 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x44) == 320 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x46) == 180 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x48) == 1280 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x4A) == 720 &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x4C) == 0x42700000u &&
                   PPC_LOAD_U32(kDisplayInfoAddress + 0x50) == 0 &&
                   PPC_LOAD_U16(kDisplayInfoAddress + 0x56) == 1280,
               "VdGetCurrentDisplayInformation returned an incorrect field or byte order"))
        return 1;

    constexpr uint32_t kFillAddress = 0x300;
    constexpr uint32_t kFillBytes = 0x10;
    std::memset(base + kFillAddress, 0xA5, kFillBytes);
    context = {};
    context.r3.u64 = kFillAddress;
    context.r4.u64 = kFillBytes;
    context.r5.u64 = 0xFFFFFFFF80000000ull;
    __imp__RtlFillMemoryUlong(context, base);
    if (!Check(PPC_LOAD_U32(kFillAddress + 0x00) == 0x80000000u &&
                   PPC_LOAD_U32(kFillAddress + 0x04) == 0x80000000u &&
                   PPC_LOAD_U32(kFillAddress + 0x08) == 0x80000000u &&
                   PPC_LOAD_U32(kFillAddress + 0x0C) == 0x80000000u,
               "RtlFillMemoryUlong did not repeat the guest ULONG pattern"))
        return 1;

    constexpr uint32_t kScalerStack = 0x80;
    constexpr uint32_t kVerticalFilterParams = 0x200;
    constexpr uint32_t kHorizontalFilterParams = 0x20C;
    constexpr uint32_t kScalerAuxiliary = 0x218;
    constexpr uint32_t kScalerCommands = 0x320;
    std::memset(base + kScalerCommands, 0xA5, 0x40);
    context = {};
    context.r1.u64 = kScalerStack;
    context.r3.u64 = 0;
    context.r4.u64 = 0x050002D0u;
    context.r5.u64 = 0;
    context.r6.u64 = 0x050002D0u;
    context.r7.u64 = 0x050002D0u;
    context.r8.u64 = 7;
    context.r9.u64 = kVerticalFilterParams;
    context.r10.u64 = 7;
    PPC_STORE_U32(kScalerStack + 0x54, kHorizontalFilterParams);
    PPC_STORE_U32(kScalerStack + 0x5C, kScalerAuxiliary);
    PPC_STORE_U32(kScalerStack + 0x64, kScalerCommands);
    PPC_STORE_U32(kScalerStack + 0x6C, 200);
    __imp__VdInitializeScalerCommandBuffer(context, base);
    constexpr uint32_t kExpectedScalerCommands[] = {
        0x00001973u, 1u,
        0x00001964u, 0u,
        0x00011960u, 0u, 0x050002D0u,
        0x00001973u, 0u,
    };
    bool scalerCommandsMatch = context.r3.u32 == std::size(kExpectedScalerCommands);
    for (uint32_t index = 0; index < std::size(kExpectedScalerCommands); ++index) {
        scalerCommandsMatch &=
            PPC_LOAD_U32(kScalerCommands + index * 4) == kExpectedScalerCommands[index];
    }
    scalerCommandsMatch &=
        PPC_LOAD_U32(kScalerCommands + sizeof(kExpectedScalerCommands)) == 0xA5A5A5A5u;
    if (!Check(scalerCommandsMatch,
               "VdInitializeScalerCommandBuffer did not emit the verified identity PM4 sequence"))
        return 1;

    constexpr uint32_t kPersistState = 0x240;
    constexpr uint32_t kPersistAllocationOutput = 0x24C;
    std::memset(base + kPersistState, 0, 12);
    PPC_STORE_U32(kPersistAllocationOutput, 0xA5A5A5A5u);
    const uint32_t pagesBeforePersist = GetGuestMemoryAccounting().used_physical_pages();
    context = {};
    context.r3.u64 = kPersistState;
    context.r4.u64 = kPersistAllocationOutput;
    __imp__VdPersistDisplay(context, base);
    const uint32_t persistAllocation = PPC_LOAD_U32(kPersistAllocationOutput);
    if (!Check(context.r3.u32 == 1 && persistAllocation >= 0xA0000000u &&
                   persistAllocation < 0xC0000000u &&
                   GetGuestMemoryAccounting().used_physical_pages() == pagesBeforePersist + 1,
               "VdPersistDisplay did not reserve a tracked physical allocation"))
        return 1;
    context = {};
    context.r3.u64 = 1;
    context.r4.u64 = persistAllocation;
    __imp__MmFreePhysicalMemory(context, base);
    if (!Check(GetGuestMemoryAccounting().used_physical_pages() == pagesBeforePersist,
               "VdPersistDisplay allocation did not follow the title's physical free path"))
        return 1;

    context = {};
    context.r3.u64 = 1;
    __imp__VdEnableDisableClockGating(context, base);
    if (!Check(context.r3.u32 == 0 && RuntimeGraphicsClockGatingEnabled(),
               "VdEnableDisableClockGating did not retain the enabled policy"))
        return 1;
    context = {};
    context.r3.u64 = 0;
    __imp__VdEnableDisableClockGating(context, base);
    if (!Check(context.r3.u32 == 0 && !RuntimeGraphicsClockGatingEnabled(),
               "VdEnableDisableClockGating did not retain the disabled policy"))
        return 1;

    size_t decodedAudioClient = SIZE_MAX;
    if (!Check(kRuntimeXAudioHostSampleRate == 48000 &&
                   kRuntimeXAudioHostChannelCount == 2 &&
                   kRuntimeXAudioHostBitsPerSample == 16 &&
                   kRuntimeXAudioHostBlockAlign == 4 &&
                   kRuntimeXAudioHostFrameBytes == 1024 &&
                   kRuntimeXAudioHostFrameCount == 256 &&
                   kRuntimeXAudioHostFrameDurationUs == 5333,
               "XAudio host buffer contract does not represent 256 frames at 48 kHz"))
        return 1;
    if (!Check(RuntimeAudioDriverToken(0) == 0x41550000 &&
                   RuntimeAudioDriverToken(7) == 0x41550007 &&
                   RuntimeAudioDriverToken(8) == 0 &&
                   RuntimeAudioDecodeDriverToken(0x41550007, &decodedAudioClient) &&
                   decodedAudioClient == 7 &&
                   !RuntimeAudioDecodeDriverToken(0x41550008, nullptr) &&
                   !RuntimeAudioDecodeDriverToken(0x41560000, nullptr),
               "XAudio render-driver token validation did not match the pinned contract"))
        return 1;
    uint32_t voiceCategoryChangeMask = 0xFFFFFFFFu;
    if (!Check(RuntimeAudioGetVoiceCategoryVolumeChangeMask(
                       RuntimeAudioDriverToken(0), &voiceCategoryChangeMask) == 0 &&
                   voiceCategoryChangeMask == 0 &&
                   RuntimeAudioGetVoiceCategoryVolumeChangeMask(
                       0x41560000u, &voiceCategoryChangeMask) == 0x80070057u &&
                   RuntimeAudioGetVoiceCategoryVolumeChangeMask(
                       RuntimeAudioDriverToken(0), nullptr) == 0x80070057u,
               "XAudio category-volume change query did not remain an immediate empty-mask contract"))
        return 1;

    // XMA contexts may only be created after the shared hardware adapter is
    // live. A failed setup must roll back its five-page context-array
    // reservation and must not publish a fabricated context pointer.
    constexpr uint32_t kXmaContextOut = 0x270;
    const uint32_t pagesBeforeXma = GetGuestMemoryAccounting().used_physical_pages();
    PPC_STORE_U32(kXmaContextOut, 0xA5A5A5A5u);
    if (!Check(RuntimeAudioCreateXmaContext(base, kXmaContextOut) == kStatusUnsuccessful &&
                   PPC_LOAD_U32(kXmaContextOut) == 0xA5A5A5A5u &&
                   GetGuestMemoryAccounting().used_physical_pages() == pagesBeforeXma,
               "XMA setup failure leaked physical pages or published a fake context"))
        return 1;
    if (!Check(RuntimeAudioReleaseXmaContext(0xA0000000u) == kStatusInvalidParameter,
               "XMA release accepted an unknown context")) return 1;

    std::array<uint8_t, kRuntimeXAudioFrameBytes> audioFrame{};
    const auto storeBigEndianFloat = [&](size_t channel, size_t sample, float value) {
        uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        const size_t offset =
            (channel * kRuntimeXAudioChannelSamples + sample) * sizeof(float);
        audioFrame[offset + 0] = static_cast<uint8_t>(bits >> 24);
        audioFrame[offset + 1] = static_cast<uint8_t>(bits >> 16);
        audioFrame[offset + 2] = static_cast<uint8_t>(bits >> 8);
        audioFrame[offset + 3] = static_cast<uint8_t>(bits);
    };
    storeBigEndianFloat(0, 0, 1.0f);
    storeBigEndianFloat(1, 0, -1.0f);
    storeBigEndianFloat(2, 0, 0.5f);
    storeBigEndianFloat(3, 0, 1.0f);  // LFE is deliberately excluded.
    storeBigEndianFloat(4, 0, 0.25f);
    storeBigEndianFloat(5, 0, -0.25f);
    std::array<int16_t, kRuntimeXAudioStereoSamples> stereoFrame{};
    RuntimeAudioConvertFrameToStereoPcm16(audioFrame.data(), stereoFrame.data());
    if (!Check(std::abs(int(stereoFrame[0]) - 19660) <= 1 &&
                   std::abs(int(stereoFrame[1]) + 13107) <= 1 &&
                   stereoFrame[2] == 0 && stereoFrame[3] == 0,
               "XAudio 5.1 big-endian frame downmix produced incorrect stereo samples"))
        return 1;

    // Host PCM conversion must never leak invalid guest floats into the
    // integer conversion path.  The real mixer may produce over-range or
    // non-finite intermediates while voices are being initialized or torn
    // down, so verify the reusable saturation/silence policy independently of
    // the live title probe.
    storeBigEndianFloat(0, 1, 10.0f);
    storeBigEndianFloat(1, 1, -10.0f);
    storeBigEndianFloat(0, 2, std::numeric_limits<float>::quiet_NaN());
    storeBigEndianFloat(1, 2, std::numeric_limits<float>::infinity());
    RuntimeAudioConvertFrameToStereoPcm16(audioFrame.data(), stereoFrame.data());
    if (!Check(stereoFrame[2] == INT16_MAX && stereoFrame[3] == -INT16_MAX &&
                   stereoFrame[4] == 0 && stereoFrame[5] == 0,
               "XAudio PCM conversion did not saturate or silence invalid samples"))
        return 1;

    const std::array<int16_t, 5> volumeSource = {
        INT16_MAX, -INT16_MAX, 1, -1, 0};
    auto volumeIdentity = volumeSource;
    RuntimeAudioScaleHostPcm16(volumeIdentity.data(), volumeIdentity.size(),
                               kRuntimeAudioMasterVolumeOneQ16);
    if (!Check(volumeIdentity == volumeSource,
               "identity PC master volume changed host PCM samples"))
        return 1;
    auto volumeHalf = volumeSource;
    RuntimeAudioScaleHostPcm16(volumeHalf.data(), volumeHalf.size(), 32768u);
    if (!Check(volumeHalf ==
                   std::array<int16_t, 5>{16384, -16384, 1, -1, 0},
               "half PC master volume did not use deterministic symmetric rounding"))
        return 1;
    auto volumeOff = volumeSource;
    RuntimeAudioScaleHostPcm16(volumeOff.data(), volumeOff.size(), 0u);
    if (!Check(volumeOff == std::array<int16_t, 5>{0, 0, 0, 0, 0},
               "zero PC master volume did not silence only the host PCM buffer"))
        return 1;
    ConfigureRuntimeAudioMasterVolume(0.5);
    if (!Check(RuntimeAudioMasterVolumeQ16() == 32768u,
               "PC master volume did not retain its exact startup Q16 value"))
        return 1;
    ConfigureRuntimeAudioMasterVolume(1.0);
    if (!Check(RuntimeAudioMasterVolumeQ16() ==
                   kRuntimeAudioMasterVolumeOneQ16,
               "PC master volume did not restore the exact identity value"))
        return 1;
    if (!Check(RuntimeAudioRunSynchronousCompletionRegression(),
               "XAudio submission held the client lock across a synchronous completion"))
        return 1;

    const GuestThreadDispatchPlan rawThread =
        ResolveGuestThreadDispatch(0, 0x828B52C8, 0x12345678);
    if (!Check(rawThread.rawThread && rawThread.address == 0x828B52C8 &&
                   rawThread.r3 == 0x12345678 && rawThread.r4 == 0,
               "raw ExCreateThread dispatch did not invoke StartAddress with StartContext"))
        return 1;
    const GuestThreadDispatchPlan xapiThread =
        ResolveGuestThreadDispatch(0x828ADA98, 0x821FBB00, 0xA0165F78);
    if (!Check(!xapiThread.rawThread && xapiThread.address == 0x828ADA98 &&
                   xapiThread.r3 == 0x821FBB00 && xapiThread.r4 == 0xA0165F78,
               "XAPI ExCreateThread dispatch did not preserve trampoline ABI"))
        return 1;

    constexpr uint32_t kSpinLockAddress = 0x3D0;
    constexpr uint32_t kTestPcr = 0x100;
    context.r13.u64 = kTestPcr;
    PPC_STORE_U8(kTestPcr + 0x18, 0);
    context.r3.u64 = kSpinLockAddress;
    __imp__KfAcquireSpinLock(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U8(kTestPcr + 0x18) == 2 &&
                   PPC_LOAD_U32(kSpinLockAddress) == kTestPcr,
               "KfAcquireSpinLock did not publish ownership and raise IRQL")) return 1;
    context.r3.u64 = kSpinLockAddress;
    context.r4.u64 = 0;
    __imp__KfReleaseSpinLock(context, base);
    if (!Check(PPC_LOAD_U32(kSpinLockAddress) == 0 && PPC_LOAD_U8(kTestPcr + 0x18) == 0,
               "KfReleaseSpinLock did not release ownership and restore IRQL")) return 1;

    PPC_STORE_U8(kTestPcr + 0x18, 0x44);
    context.r3.u64 = kSpinLockAddress;
    __imp__KeAcquireSpinLockAtRaisedIrql(context, base);
    if (!Check(PPC_LOAD_U32(kSpinLockAddress) == kTestPcr &&
                   PPC_LOAD_U8(kTestPcr + 0x18) == 0x44,
               "KeAcquireSpinLockAtRaisedIrql changed caller IRQL")) return 1;
    context.r3.u64 = kSpinLockAddress;
    __imp__KeReleaseSpinLockFromRaisedIrql(context, base);
    if (!Check(PPC_LOAD_U32(kSpinLockAddress) == 0 && PPC_LOAD_U8(kTestPcr + 0x18) == 0x44,
               "KeReleaseSpinLockFromRaisedIrql changed caller IRQL")) return 1;
    PPC_STORE_U8(kTestPcr + 0x18, 0);

    __imp__KeQueryPerformanceFrequency(context, base);
    if (!Check(context.r3.u64 == 50'000'000u, "unexpected KeQueryPerformanceFrequency result")) return 1;
    __imp__KeGetCurrentProcessType(context, base);
    if (!Check(context.r3.u32 == 1,
               "KeGetCurrentProcessType did not report the title user process")) return 1;
    PPC_STORE_U8(kTestPcr + 0x18, 0);
    context.r13.u64 = kTestPcr;
    __imp__KeRaiseIrqlToDpcLevel(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U8(kTestPcr + 0x18) == 2,
               "KeRaiseIrqlToDpcLevel did not return and raise the guest IRQL")) return 1;
    context.r3.u64 = 0;
    __imp__KfLowerIrql(context, base);
    if (!Check(PPC_LOAD_U8(kTestPcr + 0x18) == 0,
               "KfLowerIrql did not restore the guest IRQL")) return 1;

    constexpr uint32_t kInputCapabilities = 0x200;
    RuntimeInputCapabilities syntheticCapabilities{};
    syntheticCapabilities.type = 1;
    syntheticCapabilities.subType = 1;
    syntheticCapabilities.flags = 0x1234;
    syntheticCapabilities.buttons = 0xA55A;
    syntheticCapabilities.leftTrigger = 0x12;
    syntheticCapabilities.rightTrigger = 0x34;
    syntheticCapabilities.thumbLX = -32768;
    syntheticCapabilities.thumbLY = 32767;
    syntheticCapabilities.thumbRX = -1234;
    syntheticCapabilities.thumbRY = 5678;
    syntheticCapabilities.leftMotorSpeed = 0x1357;
    syntheticCapabilities.rightMotorSpeed = 0x2468;
    StoreGuestInputCapabilities(base, kInputCapabilities, syntheticCapabilities);
    if (!Check(base[kInputCapabilities] == 1 && base[kInputCapabilities + 1] == 1 &&
                   PPC_LOAD_U16(kInputCapabilities + 2) == 0x1234 &&
                   PPC_LOAD_U16(kInputCapabilities + 4) == 0xA55A &&
                   base[kInputCapabilities + 6] == 0x12 &&
                   base[kInputCapabilities + 7] == 0x34 &&
                   PPC_LOAD_U16(kInputCapabilities + 8) == 0x8000 &&
                   PPC_LOAD_U16(kInputCapabilities + 10) == 0x7FFF &&
                   PPC_LOAD_U16(kInputCapabilities + 12) == static_cast<uint16_t>(-1234) &&
                   PPC_LOAD_U16(kInputCapabilities + 14) == 5678 &&
                   PPC_LOAD_U16(kInputCapabilities + 16) == 0x1357 &&
                   PPC_LOAD_U16(kInputCapabilities + 18) == 0x2468,
               "X_INPUT_CAPABILITIES guest-endian encoding is incorrect")) return 1;

    constexpr uint32_t kInputState = 0x218;
    RuntimeInputState syntheticState{};
    syntheticState.packetNumber = 0x12345678u;
    syntheticState.buttons = 0xA55A;
    syntheticState.leftTrigger = 0x12;
    syntheticState.rightTrigger = 0x34;
    syntheticState.thumbLX = -32768;
    syntheticState.thumbLY = 32767;
    syntheticState.thumbRX = -1234;
    syntheticState.thumbRY = 5678;
    StoreGuestInputState(base, kInputState, syntheticState);
    if (!Check(PPC_LOAD_U32(kInputState + 0) == 0x12345678u &&
                   PPC_LOAD_U16(kInputState + 4) == 0xA55A &&
                   base[kInputState + 6] == 0x12 && base[kInputState + 7] == 0x34 &&
                   PPC_LOAD_U16(kInputState + 8) == 0x8000 &&
                   PPC_LOAD_U16(kInputState + 10) == 0x7FFF &&
                   PPC_LOAD_U16(kInputState + 12) == static_cast<uint16_t>(-1234) &&
                   PPC_LOAD_U16(kInputState + 14) == 5678,
               "X_INPUT_STATE guest-endian encoding is incorrect")) return 1;

    constexpr uint32_t kInputVibration = 0x228;
    PPC_STORE_U16(kInputVibration + 0, 0x1357);
    PPC_STORE_U16(kInputVibration + 2, 0x2468);
    const RuntimeInputVibration decodedVibration =
        LoadGuestInputVibration(base, kInputVibration);
    if (!Check(decodedVibration.leftMotorSpeed == 0x1357 &&
                   decodedVibration.rightMotorSpeed == 0x2468,
               "X_INPUT_VIBRATION guest-endian decoding is incorrect")) return 1;
    ConfigureRuntimeInputVibrationScale(1.0);
    if (!Check(RuntimeInputVibrationScaleQ16() == 65536u &&
                   ScaleRuntimeInputMotorSpeed(0x1357, 65536u) == 0x1357 &&
                   ScaleRuntimeInputMotorSpeed(0xFFFF, 32768u) == 0x8000 &&
                   ScaleRuntimeInputMotorSpeed(0x2468, 0u) == 0,
               "PC vibration scaling did not preserve identity/half/off semantics"))
        return 1;
    ConfigureRuntimeInputVibrationScale(0.5);
    if (!Check(RuntimeInputVibrationScaleQ16() == 32768u,
               "PC vibration scale did not retain its exact Q16 startup value"))
        return 1;
    ConfigureRuntimeInputVibrationScale(1.0);

    RuntimeInputButtonMap buttonMap;
    buttonMap.sourceForGuest[static_cast<size_t>(RuntimeInputButton::A)] =
        static_cast<uint8_t>(RuntimeInputButton::B);
    buttonMap.sourceForGuest[static_cast<size_t>(RuntimeInputButton::B)] =
        static_cast<uint8_t>(RuntimeInputButton::None);
    ConfigureRuntimeInputButtonMap(buttonMap);
    if (!Check(RemapRuntimeInputButtons(0x2000, RuntimeInputButtonMapCode()) ==
                       0x1000,
               "configured physical controller map did not publish B as guest A"))
        return 1;
    ConfigureRuntimeInputButtonMap(RuntimeInputButtonMap{});
    if (!Check(RuntimeInputButtonMapCode() == kRuntimeInputIdentityButtonMap,
               "restored physical controller map did not preserve exact identity"))
        return 1;

    RuntimeInputTransitionTracker inputTransitions;
    if (!Check(inputTransitions.Update(0, 0) &&
                   !inputTransitions.Update(0, 0) &&
                   inputTransitions.Update(0, 0x10) &&
                   !inputTransitions.Update(0, 0x10) &&
                   inputTransitions.Update(0, 0) &&
                   inputTransitions.Update(0x48F, 0) &&
                   !inputTransitions.Update(0x48F, 0),
               "input diagnostics did not preserve transition-only edge detection")) return 1;

    context = {};
    __imp__XamGetSystemVersion(context, base);
    if (!Check(context.r3.u32 == 0x20167F20u,
                "XamGetSystemVersion did not return the local XEX-required 2.0.5759.32")) return 1;

    // Publish the exact XEX execution-info representation and verify that XAM
    // returns a guest pointer to it. The title's reached caller dereferences
    // the title-id halfword at +0x0C, so this also covers guest byte order.
    constexpr uint32_t kExecutionInfoOutput = 0x328;
    constexpr uint32_t kExecutionInfo = 0x340;
    std::array<uint8_t, 0x80> syntheticXex{};
    const auto storeRawBe32 = [&syntheticXex](size_t offset, uint32_t value) {
        syntheticXex[offset + 0] = static_cast<uint8_t>(value >> 24);
        syntheticXex[offset + 1] = static_cast<uint8_t>(value >> 16);
        syntheticXex[offset + 2] = static_cast<uint8_t>(value >> 8);
        syntheticXex[offset + 3] = static_cast<uint8_t>(value);
    };
    storeRawBe32(0x00, 0x58455832u); // XEX2
    storeRawBe32(0x14, 1);
    storeRawBe32(0x18, 0x00040006u);
    storeRawBe32(0x1C, 0x40);
    storeRawBe32(0x40, 0x0F213645u);
    storeRawBe32(0x44, 0x00000001u);
    storeRawBe32(0x48, 0x00000001u);
    storeRawBe32(0x4C, 0x545407EEu);
    syntheticXex[0x52] = 1;
    syntheticXex[0x53] = 1;
    ConfigureExecutableSystemFlags(syntheticXex.data(), syntheticXex.size());
    if (!Check(PublishExecutableExecutionInfo(base, kExecutionInfo),
               "XEX execution info could not be published into guest memory")) return 1;
    PPC_STORE_U32(kExecutionInfoOutput, 0xA5A5A5A5u);
    context = {};
    context.r3.u64 = kExecutionInfoOutput;
    __imp__XamGetExecutionId(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U32(kExecutionInfoOutput) == kExecutionInfo &&
                   PPC_LOAD_U32(kExecutionInfo + 0x00) == 0x0F213645u &&
                   PPC_LOAD_U32(kExecutionInfo + 0x0C) == 0x545407EEu &&
                   base[kExecutionInfo + 0x12] == 1 &&
                   base[kExecutionInfo + 0x13] == 1,
               "XamGetExecutionId did not return the exact guest XEX metadata")) return 1;
    context = {};
    context.r3.u64 = 0;
    __imp__XamGetExecutionId(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidParameter,
               "XamGetExecutionId accepted a null output pointer")) return 1;

    // The native profile is deliberately local-only. Slot 0 must be visible to
    // the title after a real Start press, while the other three console slots
    // remain unsigned; no Xbox Live identity is fabricated.
    for (uint32_t userIndex = 0; userIndex < 5; ++userIndex) {
        context = {};
        context.r3.u64 = userIndex;
        __imp__XamUserGetSigninState(context, base);
        const uint32_t expectedState = userIndex == 0 ? 1u : 0u;
        if (!Check(context.r3.u32 == expectedState,
                   "XamUserGetSigninState returned an incorrect local profile state")) return 1;
    }

    // The first post-Start profile query uses the standard two-call XAM
    // contract for six integer preferences. Verify both the required-size
    // response and every byte of the returned fixed-size records.
    GetGuestXamState().ResetForTests();
    if (!Check(GetGuestXamState().ConfigureControllerProfile(2, true) &&
                   !GetGuestXamState().ConfigureControllerProfile(3, false),
               "controller profile must accept only the standard Xbox sensitivity enum"))
        return 1;
    constexpr uint32_t kProfileStack = 0x20;
    constexpr uint32_t kProfileSettingIds = 0x80;
    constexpr uint32_t kProfileBufferSize = 0xC0;
    constexpr uint32_t kProfileBuffer = 0x140;
    constexpr std::array<uint32_t, 6> kRequestedProfileSettings = {
        0x10040018u, 0x10040015u, 0x10040022u,
        0x10040002u, 0x10040024u, 0x10040003u,
    };
    constexpr std::array<uint32_t, 6> kExpectedProfileValues = {2, 0, 1, 1, 0, 3};
    for (size_t index = 0; index < kRequestedProfileSettings.size(); ++index) {
        PPC_STORE_U32(kProfileSettingIds + static_cast<uint32_t>(index) * 4u,
                      kRequestedProfileSettings[index]);
    }
    PPC_STORE_U32(kProfileBufferSize, 0);
    PPC_STORE_U32(kProfileStack + 84, 0);
    context = {};
    context.r1.u64 = kProfileStack;
    context.r3.u64 = 0x545407EEu;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    context.r6.u64 = 0;
    context.r7.u64 = static_cast<uint32_t>(kRequestedProfileSettings.size());
    context.r8.u64 = kProfileSettingIds;
    context.r9.u64 = kProfileBufferSize;
    context.r10.u64 = 0;
    __imp__XamUserReadProfileSettings(context, base);
    if (!Check(context.r3.u32 == 0x7A && PPC_LOAD_U32(kProfileBufferSize) == 0xF8,
               "XamUserReadProfileSettings size query violated the two-call contract"))
        return 1;

    std::memset(base + kProfileBuffer, 0xA5, 0xF8);
    context.r3.u64 = 0x545407EEu;
    context.r10.u64 = kProfileBuffer;
    __imp__XamUserReadProfileSettings(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kProfileBuffer) == 6 &&
                   PPC_LOAD_U32(kProfileBuffer + 4) == kProfileBuffer + 8,
               "XamUserReadProfileSettings returned an invalid result header"))
        return 1;
    for (size_t index = 0; index < kRequestedProfileSettings.size(); ++index) {
        const uint32_t setting = kProfileBuffer + 8 + static_cast<uint32_t>(index) * 40u;
        if (!Check(PPC_LOAD_U32(setting + 0) == 1 &&
                       PPC_LOAD_U32(setting + 4) == 0 &&
                       PPC_LOAD_U32(setting + 8) == 0 &&
                       PPC_LOAD_U32(setting + 16) == kRequestedProfileSettings[index] &&
                       PPC_LOAD_U32(setting + 20) == 0 &&
                       base[setting + 24] == 1 &&
                       PPC_LOAD_U32(setting + 28) == 0 &&
                       PPC_LOAD_U32(setting + 32) == kExpectedProfileValues[index] &&
                       PPC_LOAD_U32(setting + 36) == 0,
                   "XamUserReadProfileSettings returned an invalid integer setting record"))
            return 1;
    }
    GetGuestXamState().ResetForTests();

    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    __imp__XamInputGetState(context, base);
    if (!Check(context.r3.u32 == 0xA0,
               "XamInputGetState did not reject a null output buffer")) return 1;
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 2;
    context.r5.u64 = kInputState;
    __imp__XamInputGetState(context, base);
    if (!Check(context.r3.u32 == 0x48F,
               "XamInputGetState accepted a non-gamepad device query")) return 1;
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    __imp__XamInputSetState(context, base);
    if (!Check(context.r3.u32 == 0xA0,
               "XamInputSetState did not reject a null vibration buffer")) return 1;

    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 1;
    context.r5.u64 = 0;
    __imp__XamInputGetCapabilities(context, base);
    if (!Check(context.r3.u32 == 0xA0,
               "XamInputGetCapabilities did not reject a null output buffer")) return 1;
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 2;
    context.r5.u64 = kInputCapabilities;
    __imp__XamInputGetCapabilities(context, base);
    if (!Check(context.r3.u32 == 0x48F,
               "XamInputGetCapabilities accepted a non-gamepad device query")) return 1;

    GetRuntimeTitleTerminateNotifications().ResetForTests();
    constexpr uint32_t kTerminateRegistration = 0x300;
    PPC_STORE_U32(kTerminateRegistration, 0x12345678);
    PPC_STORE_U32(kTerminateRegistration + 4, 0x90);
    PPC_STORE_U32(kTerminateRegistration + 8, 0xAABBCCDD);
    PPC_STORE_U32(kTerminateRegistration + 12, 0xEEFF0011);
    context = {};
    context.r3.u64 = kTerminateRegistration;
    context.r4.u64 = 1;
    __imp__ExRegisterTitleTerminateNotification(context, base);
    auto terminateNotifications = GetRuntimeTitleTerminateNotifications().Snapshot();
    if (!Check(terminateNotifications.size() == 1 &&
                   terminateNotifications[0].routine == 0x12345678 &&
                   terminateNotifications[0].priority == 0x90 &&
                   PPC_LOAD_U32(kTerminateRegistration + 8) == 0xAABBCCDD &&
                   PPC_LOAD_U32(kTerminateRegistration + 12) == 0xEEFF0011,
               "title terminate registration was not retained without rewriting guest links")) return 1;
    context.r4.u64 = 0;
    __imp__ExRegisterTitleTerminateNotification(context, base);
    if (!Check(GetRuntimeTitleTerminateNotifications().Snapshot().empty(),
               "title terminate notification removal failed")) return 1;

    GetGuestFileSystem().ResetForTests();
    GetGuestFileSystem().SetGameRoot(std::filesystem::path(__FILE__).parent_path());
    constexpr uint32_t kLinkPath = 0x100;
    constexpr uint32_t kLinkTarget = 0x120;
    constexpr uint32_t kLinkPathString = 0x180;
    constexpr uint32_t kLinkTargetString = 0x188;
    constexpr char kLinkPathValue[] = "\\??\\xbmovie:";
    constexpr char kLinkTargetValue[] = "D:";
    std::memcpy(base + kLinkPath, kLinkPathValue, sizeof(kLinkPathValue));
    std::memcpy(base + kLinkTarget, kLinkTargetValue, sizeof(kLinkTargetValue));
    PPC_STORE_U16(kLinkPathString, sizeof(kLinkPathValue) - 1);
    PPC_STORE_U16(kLinkPathString + 2, sizeof(kLinkPathValue));
    PPC_STORE_U32(kLinkPathString + 4, kLinkPath);
    PPC_STORE_U16(kLinkTargetString, sizeof(kLinkTargetValue) - 1);
    PPC_STORE_U16(kLinkTargetString + 2, sizeof(kLinkTargetValue));
    PPC_STORE_U32(kLinkTargetString + 4, kLinkTarget);
    context = {};
    context.r3.u64 = kLinkPathString;
    context.r4.u64 = kLinkTargetString;
    __imp__ObCreateSymbolicLink(context, base);
    GuestFileAttributes linkedAttributes{};
    if (!Check(context.r3.u32 == 0 &&
                   GetGuestFileSystem().QueryGamePath(
                       "xbmovie:\\runtime_import_test.cpp", &linkedAttributes),
               "created Xbox symbolic link did not participate in path resolution")) return 1;
    context = {};
    context.r3.u64 = kLinkPathString;
    __imp__ObDeleteSymbolicLink(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   !GetGuestFileSystem().QueryGamePath(
                       "xbmovie:\\runtime_import_test.cpp", &linkedAttributes),
               "deleted Xbox symbolic link remained resolvable")) return 1;
    __imp__ObDeleteSymbolicLink(context, base);
    if (!Check(context.r3.u32 == kStatusUnsuccessful,
               "deleting an absent Xbox symbolic link did not fail")) return 1;

    constexpr uint32_t kRandomAddress = 0x3E0;
    constexpr uint32_t kRandomBytes = 16;
    std::memset(base + kRandomAddress, 0xA5, kRandomBytes);
    context = {};
    context.r3.u64 = 1;
    context.r4.u64 = kRandomAddress;
    context.r5.u64 = kRandomBytes;
    __imp__NetDll_XNetRandom(context, base);
    bool randomBufferChanged = false;
    for (uint32_t index = 0; index < kRandomBytes; ++index)
        randomBufferChanged |= base[kRandomAddress + index] != 0xA5;
    if (!Check(context.r3.u32 == 0 && randomBufferChanged,
               "NetDll_XNetRandom did not fill the guest buffer")) return 1;

    GetGuestXamState().ResetForTests();
    for (const uint32_t language : {1u, 3u, 4u, 5u, 6u}) {
        if (!Check(GetGuestXamState().ConfigureLanguage(language),
                   "valid extracted-content language was rejected")) return 1;
        context = {};
        __imp__XGetLanguage(context, base);
        if (!Check(context.r3.u32 == language,
                   "XGetLanguage did not return configured Xbox language")) return 1;
        PPC_STORE_U32(0x330, 0xFFFFFFFFu);
        PPC_STORE_U16(0x334, 0);
        context = {};
        context.r3.u64 = 3;
        context.r4.u64 = 9;
        context.r5.u64 = 0x330;
        context.r6.u64 = 4;
        context.r7.u64 = 0x334;
        __imp__ExGetXConfigSetting(context, base);
        if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x330) == language &&
                       PPC_LOAD_U16(0x334) == 4,
                   "language APIs returned inconsistent configured values")) return 1;
    }
    if (!Check(!GetGuestXamState().ConfigureLanguage(2) &&
                   GetGuestXamState().Language() == 6,
               "unsupported language changed retained XAM state")) return 1;
    GetGuestXamState().ResetForTests();
    context = {};
    context.r3.u64 = 3;
    context.r4.u64 = 2;
    __imp__XamNotifyCreateListener(context, base);
    const uint32_t notifyHandle = context.r3.u32;
    GuestObjectInfo notifyInfo{};
    if (!Check(notifyHandle != 0 && GetGuestObjects().GetObjectInfo(notifyHandle, &notifyInfo) &&
                   notifyInfo.type == GuestObjectType::NotifyListener,
               "XamNotifyCreateListener did not create a real listener handle")) return 1;
    PPC_STORE_U32(0x3D8, 0xFFFFFFFFu);
    PPC_STORE_U32(0x3DC, 0xFFFFFFFFu);
    context = {};
    context.r3.u64 = notifyHandle;
    context.r4.u64 = 0;
    context.r5.u64 = 0x3D8;
    context.r6.u64 = 0x3DC;
    __imp__XNotifyGetNext(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x3D8) == 0 &&
                   PPC_LOAD_U32(0x3DC) == 0,
               "empty notification listener did not return a clean empty result")) return 1;
    context = {};
    context.r3.u64 = 10;
    __imp__XNotifyPositionUI(context, base);

    constexpr uint32_t kXgiContextPayload = 0x300;
    PPC_STORE_U32(kXgiContextPayload + 0, 0);
    PPC_STORE_U32(kXgiContextPayload + 4, 0);
    PPC_STORE_U64(kXgiContextPayload + 8, 0);
    PPC_STORE_U32(kXgiContextPayload + 16, 0x00010001u);
    PPC_STORE_U32(kXgiContextPayload + 20, 3);
    context = {};
    context.r3.u64 = 0xFB;
    context.r4.u64 = 0x000B0006;
    context.r5.u64 = 0;
    context.r6.u64 = kXgiContextPayload;
    context.r7.u64 = 24;
    __imp__XMsgStartIORequest(context, base);
    uint32_t retainedContext{};
    if (!Check(context.r3.u32 == 0 &&
                   GetGuestXamState().GetUserContext(0, 0x00010001u, &retainedContext) &&
                   retainedContext == 3,
               "XGI user context message was not retained")) return 1;

    // XMPCaptureOutput is unavailable until the real audio subsystem exists.
    // The XAM contract must expose that failure through the caller's
    // overlapped object, signal its real event, and return IO_PENDING rather
    // than claiming capture success synchronously.
    GetGuestObjects().ResetForTests();
    const auto xamThread = GetGuestObjects().CreateThread(1, 0x70000000, 0x20000);
    SetCurrentGuestThreadForTests(xamThread.guestObject, 1, xamThread.handle);
    GetGuestXamState().ResetForTests();

    // Probe 60 requests 50 achievement records with all three string fields.
    // Exercise the real packed ABI: details form a contiguous 36-byte array,
    // while UTF-16BE strings begin after all 50 reserved detail slots.
    ConfigureExecutableAchievementsForTests({
        RuntimeAchievement{1, "First Step", "Unlocked one", "Locked one",
                           0x101, 10, 0x11},
        RuntimeAchievement{2, "Second Step", "Unlocked two", "Locked two",
                           0x102, 20, 0x12},
    });
    constexpr uint32_t kAchievementBufferSize = 0x400;
    constexpr uint32_t kAchievementHandle = 0x404;
    constexpr uint32_t kAchievementItemsReturned = 0x408;
    constexpr uint32_t kAchievementBuffer = 0x1000;
    constexpr uint32_t kAchievementItemsPerCall = 50;
    constexpr uint32_t kAchievementItemBytes = 36 + 464;
    constexpr uint32_t kAchievementBufferBytes =
        kAchievementItemsPerCall * kAchievementItemBytes;
    PPC_STORE_U32(kAchievementBufferSize, 0xFFFFFFFFu);
    PPC_STORE_U32(kAchievementHandle, 0);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    context.r6.u64 = 0x27;
    context.r7.u64 = 0;
    context.r8.u64 = kAchievementItemsPerCall;
    context.r9.u64 = kAchievementBufferSize;
    context.r10.u64 = kAchievementHandle;
    __imp__XamUserCreateAchievementEnumerator(context, base);
    const uint32_t achievementHandle = PPC_LOAD_U32(kAchievementHandle);
    if (!Check(context.r3.u32 == 0 && achievementHandle != 0 &&
                   PPC_LOAD_U32(kAchievementBufferSize) == kAchievementBufferBytes,
               "XamUserCreateAchievementEnumerator returned an invalid contract")) return 1;

    std::memset(base + kAchievementBuffer, 0xA5, kAchievementBufferBytes);
    PPC_STORE_U32(kAchievementItemsReturned, 0);
    context = {};
    context.r3.u64 = achievementHandle;
    context.r4.u64 = 0;
    context.r5.u64 = kAchievementBuffer;
    context.r6.u64 = kAchievementBufferBytes;
    context.r7.u64 = kAchievementItemsReturned;
    context.r8.u64 = 0;
    __imp__XamEnumerate(context, base);
    const uint32_t firstLabel = PPC_LOAD_U32(kAchievementBuffer + 4);
    const uint32_t expectedStringBase = kAchievementBuffer +
                                        kAchievementItemsPerCall * 36;
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U32(kAchievementItemsReturned) == 2 &&
                   PPC_LOAD_U32(kAchievementBuffer + 0) == 1 &&
                   PPC_LOAD_U32(kAchievementBuffer + 16) == 0x101 &&
                   PPC_LOAD_U32(kAchievementBuffer + 20) == 10 &&
                   PPC_LOAD_U32(kAchievementBuffer + 32) == 0x11 &&
                   PPC_LOAD_U32(kAchievementBuffer + 36) == 2 &&
                   PPC_LOAD_U32(kAchievementBuffer + 36 + 16) == 0x102 &&
                   firstLabel == expectedStringBase &&
                   base[firstLabel] == 0 && base[firstLabel + 1] == 'F' &&
                   base[firstLabel + 2] == 0 && base[firstLabel + 3] == 'i',
               "achievement enumeration did not return packed XDBF-derived records")) return 1;
    context = {};
    context.r3.u64 = achievementHandle;
    context.r4.u64 = 0;
    context.r5.u64 = kAchievementBuffer;
    context.r6.u64 = kAchievementBufferBytes;
    context.r7.u64 = kAchievementItemsReturned;
    context.r8.u64 = 0;
    __imp__XamEnumerate(context, base);
    if (!Check(context.r3.u32 == 0x12 &&
                   PPC_LOAD_U32(kAchievementItemsReturned) == 0,
               "achievement enumerator did not terminate with no-more-files")) return 1;
    if (!Check(GetGuestObjects().Close(achievementHandle),
               "achievement enumerator handle did not close")) return 1;
    const auto testPortableContentRoot = std::filesystem::temp_directory_path() /
                                         "TheDarknessRecomp_runtime_import_test_content";
    std::error_code portableContentError;
    std::filesystem::remove_all(testPortableContentRoot, portableContentError);
    if (!Check(!portableContentError,
               "could not reset the isolated portable-content test root")) return 1;
    // Match a packaged candidate with existing title save content but no
    // Profile/achievements.txt yet. This must initialize as an empty store.
    std::filesystem::create_directories(testPortableContentRoot / "545407EE",
                                        portableContentError);
    if (!Check(!portableContentError,
               "could not prepare the missing-profile achievement-store regression"))
        return 1;
    ConfigureGuestPortableContentDeviceRoot(testPortableContentRoot);

    // Probe73 reached XGIUserWriteAchievements synchronously with one standard
    // 8-byte {user index, achievement id} record. Exercise the real title-
    // catalog validation and portable persistence contract, then reload it and
    // verify that a fresh achievement enumerator exposes achieved flags and the
    // persisted FILETIME. A duplicate overlapped write must complete normally
    // without inventing a second unlock.
    ConfigureExecutableAchievementsForTests({
        RuntimeAchievement{1, "First Step", "Unlocked one", "Locked one",
                           0x101, 10, 0x11},
        RuntimeAchievement{2, "Second Step", "Unlocked two", "Locked two",
                           0x102, 20, 0x12},
    });
    GetGuestXamState().ResetForTests();
    if (!Check(GetGuestXamState().ConfigureAchievementCatalog(
                   0x545407EEu, {0, 1, 2}),
               "could not configure the isolated portable achievement store"))
        return 1;
    constexpr uint32_t kAchievementWritePayload = 0x500;
    constexpr uint32_t kAchievementWriteEntry = 0x520;
    constexpr uint32_t kAchievementWriteOverlapped = 0x540;
    PPC_STORE_U32(kAchievementWritePayload + 0, 1);
    PPC_STORE_U32(kAchievementWritePayload + 4, kAchievementWriteEntry);
    PPC_STORE_U32(kAchievementWriteEntry + 0, 0);
    PPC_STORE_U32(kAchievementWriteEntry + 4, 2);
    context = {};
    context.r3.u64 = 0xFB;
    context.r4.u64 = 0x000B0008;
    context.r5.u64 = 0;
    context.r6.u64 = kAchievementWritePayload;
    context.r7.u64 = 8;
    __imp__XMsgStartIORequest(context, base);
    uint64_t firstUnlockTime{};
    const auto achievementStorePath = testPortableContentRoot / "545407EE" /
                                      "Profile" / "achievements.txt";
    if (!Check(context.r3.u32 == 0 &&
                   GetGuestXamState().IsAchievementUnlocked(2, &firstUnlockTime) &&
                   firstUnlockTime != 0 && std::filesystem::is_regular_file(achievementStorePath),
               "XGI achievement write did not persist the verified title achievement"))
        return 1;

    const auto achievementEvent = GetGuestObjects().CreateEvent(false, false);
    std::memset(base + kAchievementWriteOverlapped, 0, 28);
    PPC_STORE_U32(kAchievementWriteOverlapped + 0x0C, achievementEvent.handle);
    context = {};
    context.r3.u64 = 0xFB;
    context.r4.u64 = 0x000B0008;
    context.r5.u64 = kAchievementWriteOverlapped;
    context.r6.u64 = kAchievementWritePayload;
    context.r7.u64 = 8;
    __imp__XMsgStartIORequest(context, base);
    uint64_t duplicateUnlockTime{};
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kAchievementWriteOverlapped + 0x00) == 0 &&
                   PPC_LOAD_U32(kAchievementWriteOverlapped + 0x18) == 0 &&
                   GetGuestObjects().WaitForSingleObject(
                       achievementEvent.handle, true, 0) == 0 &&
                   GetGuestXamState().IsAchievementUnlocked(2, &duplicateUnlockTime) &&
                   duplicateUnlockTime == firstUnlockTime,
               "duplicate overlapped achievement write violated XAM completion/state"))
        return 1;

    // The real title XDBF includes achievement ID 0. It is a valid catalog key,
    // not a null/sentinel value, and must survive the portable-store round trip.
    PPC_STORE_U32(kAchievementWriteEntry + 4, 0);
    context = {};
    context.r3.u64 = 0xFB;
    context.r4.u64 = 0x000B0008;
    context.r5.u64 = 0;
    context.r6.u64 = kAchievementWritePayload;
    context.r7.u64 = 8;
    __imp__XMsgStartIORequest(context, base);
    uint64_t zeroAchievementUnlockTime{};
    if (!Check(context.r3.u32 == 0 &&
                   GetGuestXamState().IsAchievementUnlocked(
                       0, &zeroAchievementUnlockTime) &&
                   zeroAchievementUnlockTime != 0,
               "XGI achievement write rejected the title's valid ID zero"))
        return 1;

    GetGuestXamState().ResetForTests();
    if (!Check(GetGuestXamState().ConfigureAchievementCatalog(
                   0x545407EEu, {0, 1, 2}) &&
                   GetGuestXamState().IsAchievementUnlocked(2, &duplicateUnlockTime) &&
                   duplicateUnlockTime == firstUnlockTime &&
                   GetGuestXamState().IsAchievementUnlocked(
                       0, &zeroAchievementUnlockTime),
               "portable achievement unlock did not survive a store reload"))
        return 1;

    PPC_STORE_U32(kAchievementBufferSize, 0);
    PPC_STORE_U32(kAchievementHandle, 0);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    context.r6.u64 = 0;
    context.r7.u64 = 0;
    context.r8.u64 = 2;
    context.r9.u64 = kAchievementBufferSize;
    context.r10.u64 = kAchievementHandle;
    __imp__XamUserCreateAchievementEnumerator(context, base);
    const uint32_t persistedAchievementHandle = PPC_LOAD_U32(kAchievementHandle);
    std::memset(base + kAchievementBuffer, 0, 72);
    PPC_STORE_U32(kAchievementItemsReturned, 0);
    context = {};
    context.r3.u64 = persistedAchievementHandle;
    context.r4.u64 = 0;
    context.r5.u64 = kAchievementBuffer;
    context.r6.u64 = 72;
    context.r7.u64 = kAchievementItemsReturned;
    context.r8.u64 = 0;
    __imp__XamEnumerate(context, base);
    const uint64_t enumeratedUnlockTime =
        uint64_t(PPC_LOAD_U32(kAchievementBuffer + 36 + 24)) |
        (uint64_t(PPC_LOAD_U32(kAchievementBuffer + 36 + 28)) << 32);
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U32(kAchievementItemsReturned) == 2 &&
                   enumeratedUnlockTime == firstUnlockTime &&
                   PPC_LOAD_U32(kAchievementBuffer + 36 + 32) == (0x12u | 0x00030000u),
               "achievement enumeration did not expose persisted unlock state"))
        return 1;
    if (!Check(GetGuestObjects().Close(persistedAchievementHandle),
               "persisted achievement enumerator handle did not close"))
        return 1;

    PPC_STORE_U32(kAchievementWriteEntry + 4, 99);
    context = {};
    context.r3.u64 = 0xFB;
    context.r4.u64 = 0x000B0008;
    context.r5.u64 = 0;
    context.r6.u64 = kAchievementWritePayload;
    context.r7.u64 = 8;
    __imp__XMsgStartIORequest(context, base);
    if (!Check(context.r3.u32 == 0x80004005u &&
                   !GetGuestXamState().IsAchievementUnlocked(99),
               "XGI achievement write accepted an ID absent from the title XDBF"))
        return 1;
    ConfigureExecutableAchievementsForTests({});

    // The post-Start title path requests a storage device asynchronously. The
    // selected ID must name the runtime's real portable content directory and
    // must complete the caller's event-backed XAM_OVERLAPPED contract.
    const auto deviceEvent = GetGuestObjects().CreateEvent(false, false);
    constexpr uint32_t kDeviceIdOutput = 0x2A0;
    constexpr uint32_t kDeviceOverlapped = 0x2B0;
    PPC_STORE_U32(kDeviceIdOutput, 0xA5A5A5A5u);
    std::memset(base + kDeviceOverlapped, 0, 28);
    PPC_STORE_U32(kDeviceOverlapped + 0x0C, deviceEvent.handle);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 1;
    context.r5.u64 = 0;
    context.r6.u64 = 0x838000;
    context.r7.u64 = kDeviceIdOutput;
    context.r8.u64 = kDeviceOverlapped;
    __imp__XamShowDeviceSelectorUI(context, base);
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kDeviceIdOutput) == kGuestPortableContentDeviceId &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x00) == 0 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x04) == 0 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x08) == xamThread.handle &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x18) == 0 &&
                   GetGuestObjects().WaitForSingleObject(deviceEvent.handle, true, 0) == 0 &&
                   std::filesystem::is_directory(GuestPortableContentDeviceRoot()),
               "XamShowDeviceSelectorUI did not select and complete the real portable device"))
        return 1;

    // The selected portable device must remain connected after content
    // create/close. Cover both the exact reached synchronous query and the
    // pinned event-backed overlapped result, including disconnected-device
    // error fields.
    context = {};
    context.r3.u64 = kGuestPortableContentDeviceId;
    context.r4.u64 = 0;
    __imp__XamContentGetDeviceState(context, base);
    if (!Check(context.r3.u32 == 0,
               "XamContentGetDeviceState rejected the real portable device")) return 1;

    std::memset(base + kDeviceOverlapped, 0, 28);
    PPC_STORE_U32(kDeviceOverlapped + 0x0C, deviceEvent.handle);
    context = {};
    context.r3.u64 = kGuestPortableContentDeviceId;
    context.r4.u64 = kDeviceOverlapped;
    __imp__XamContentGetDeviceState(context, base);
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x00) == 0 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x04) == 0 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x18) == 0 &&
                   GetGuestObjects().WaitForSingleObject(deviceEvent.handle, true, 0) == 0,
               "XamContentGetDeviceState did not complete the connected overlapped query"))
        return 1;

    std::memset(base + kDeviceOverlapped, 0, 28);
    PPC_STORE_U32(kDeviceOverlapped + 0x0C, deviceEvent.handle);
    context = {};
    context.r3.u64 = 0xDEADBEEFu;
    context.r4.u64 = kDeviceOverlapped;
    __imp__XamContentGetDeviceState(context, base);
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x00) == 0x65B &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x04) == 0 &&
                   PPC_LOAD_U32(kDeviceOverlapped + 0x18) == 0x48F &&
                   GetGuestObjects().WaitForSingleObject(deviceEvent.handle, true, 0) == 0,
               "XamContentGetDeviceState did not expose the disconnected overlapped result"))
        return 1;

    // The first post-selector title call scans the selected device for saved
    // games. A fresh portable device must produce a valid empty enumerator,
    // followed by the standard Win32 NO_MORE_FILES result and zero items.
    constexpr uint32_t kContentBufferSize = 0x2D0;
    constexpr uint32_t kContentHandleOut = 0x2D4;
    constexpr uint32_t kContentBuffer = 0x3FC;
    constexpr uint32_t kContentItemsOut = 0x2D8;
    PPC_STORE_U32(kContentBufferSize, 0xFFFFFFFFu);
    PPC_STORE_U32(kContentHandleOut, 0);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kGuestPortableContentDeviceId;
    context.r5.u64 = 1;
    context.r6.u64 = 0x1000;
    context.r7.u64 = 1;
    context.r8.u64 = kContentBufferSize;
    context.r9.u64 = kContentHandleOut;
    __imp__XamContentCreateEnumerator(context, base);
    const uint32_t contentEnumerator = PPC_LOAD_U32(kContentHandleOut);
    GuestObjectInfo contentEnumeratorInfo{};
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U32(kContentBufferSize) == 0x134 &&
                   contentEnumerator != 0 &&
                   GetGuestObjects().GetObjectInfo(contentEnumerator, &contentEnumeratorInfo) &&
                   contentEnumeratorInfo.type == GuestObjectType::Enumerator,
               "XamContentCreateEnumerator did not create the real empty content enumerator"))
        return 1;
    base[kContentBuffer] = 0xA5;
    PPC_STORE_U32(kContentItemsOut, 0xFFFFFFFFu);
    context = {};
    context.r3.u64 = contentEnumerator;
    context.r4.u64 = 0;
    context.r5.u64 = kContentBuffer;
    context.r6.u64 = 0x134;
    context.r7.u64 = kContentItemsOut;
    context.r8.u64 = 0;
    __imp__XamEnumerate(context, base);
    if (!Check(context.r3.u32 == 18 && PPC_LOAD_U32(kContentItemsOut) == 0 &&
                   base[kContentBuffer] == 0xA5,
               "XamEnumerate did not report the empty portable content catalog"))
        return 1;
    context = {};
    context.r3.u64 = contentEnumerator;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0,
               "NtClose did not release the content enumerator handle")) return 1;

    // The empty catalog causes The Darkness to create its initial saved-game
    // package. Preserve the raw content metadata, create a real host-backed
    // directory, mount it at the requested root, and make it enumerable on
    // the next scan.
    constexpr uint32_t kContentRootName = 0x40;
    constexpr uint32_t kCreatedContentData = 0x80;
    constexpr uint32_t kContentDisposition = 0x1C0;
    constexpr uint32_t kContentLicense = 0x1C4;
    constexpr uint32_t kContentCreateStack = 0x200;
    constexpr char kContentRootText[] = "savedrive";
    constexpr char kContentFileName[] = "runtime_import_save";
    const auto hostContentPackage = testPortableContentRoot / "545407EE" /
                                    "00000001" / kContentFileName;
    const auto hostContentHeader = testPortableContentRoot / "545407EE" /
                                   "Headers" / "00000001" /
                                   (std::string(kContentFileName) + ".header");
    std::memcpy(base + kContentRootName, kContentRootText, sizeof(kContentRootText));
    std::memset(base + kCreatedContentData, 0, kGuestXContentDataBytes);
    PPC_STORE_U32(kCreatedContentData + 0, kGuestPortableContentDeviceId);
    PPC_STORE_U32(kCreatedContentData + 4, 1);
    PPC_STORE_U16(kCreatedContentData + 8, 'S');
    std::memcpy(base + kCreatedContentData + 0x108,
                kContentFileName, sizeof(kContentFileName));
    PPC_STORE_U32(kContentDisposition, 0xA5A5A5A5u);
    PPC_STORE_U32(kContentLicense, 0xA5A5A5A5u);
    PPC_STORE_U32(kContentCreateStack + 84, 0);
    context = {};
    context.r1.u64 = kContentCreateStack;
    context.r3.u64 = 0;
    context.r4.u64 = kContentRootName;
    context.r5.u64 = kCreatedContentData;
    context.r6.u64 = 0x11;
    context.r7.u64 = kContentDisposition;
    context.r8.u64 = kContentLicense;
    context.r9.u64 = 0;
    context.r10.u64 = 0x838000;
    __imp__XamContentCreateEx(context, base);
    GuestFileAttributes mountedContentAttributes{};
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kContentDisposition) == 1 &&
                    PPC_LOAD_U32(kContentLicense) == 0 &&
                    GetGuestFileSystem().QueryGamePath(
                        "savedrive:\\", &mountedContentAttributes),
                "XamContentCreateEx did not create and mount the portable package"))
        return 1;

    // XamContentDelete is a separate state in the title's content worker.
    // The pinned manager rejects deletion while the package is mounted; this
    // must leave both the real package and its enumerator header intact.
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kCreatedContentData;
    context.r5.u64 = 0;
    __imp__XamContentDelete(context, base);
    if (!Check(context.r3.u32 == 5 &&
                   std::filesystem::is_directory(hostContentPackage) &&
                   std::filesystem::is_regular_file(hostContentHeader),
               "XamContentDelete removed or accepted a mounted package")) return 1;

    context = {};
    context.r1.u64 = kContentCreateStack;
    context.r3.u64 = 0;
    context.r4.u64 = kContentRootName;
    context.r5.u64 = kCreatedContentData;
    context.r6.u64 = 0x11;
    context.r7.u64 = kContentDisposition;
    context.r8.u64 = kContentLicense;
    context.r9.u64 = 0;
    context.r10.u64 = 0x838000;
    PPC_STORE_U32(kContentCreateStack + 84, 0);
    PPC_STORE_U32(kContentDisposition, 0xA5A5A5A5u);
    __imp__XamContentCreateEx(context, base);
    if (!Check(context.r3.u32 == 0xB7 && PPC_LOAD_U32(kContentDisposition) == 0,
               "XamContentCreateEx CREATE_NEW did not reject an existing package"))
        return 1;

    // Probe 57 reached the title's first file inside the newly mounted package:
    // NtCreateFile("savedrive:\\_profile", FILE_OVERWRITE_IF). Validate the
    // exact create action, immediate-backed asynchronous write/read contract,
    // overwrite action, and the immutable extracted-title boundary.
    constexpr uint32_t kSaveObjectAttributes = 0x300;
    constexpr uint32_t kSaveAnsiString = 0x310;
    constexpr uint32_t kSaveHandleOut = 0x31C;
    constexpr uint32_t kSaveIoStatus = 0x320;
    constexpr uint32_t kSaveOffset = 0x328;
    constexpr uint32_t kSaveBuffer = 0x338;
    constexpr uint32_t kSavePath = 0x350;
    constexpr char kSaveProfilePath[] = "savedrive:\\_profile";
    const auto hostSaveProfile = testPortableContentRoot / "545407EE" /
                                 "00000001" / kContentFileName / "_profile";
    auto SetSavePath = [&](const char* path, size_t length) {
        std::memcpy(base + kSavePath, path, length);
        PPC_STORE_U32(kSaveObjectAttributes + 0, 0xFFFFFFFDu);
        PPC_STORE_U32(kSaveObjectAttributes + 4, kSaveAnsiString);
        PPC_STORE_U32(kSaveObjectAttributes + 8, 0x40);
        PPC_STORE_U16(kSaveAnsiString + 0, static_cast<uint16_t>(length));
        PPC_STORE_U16(kSaveAnsiString + 2, static_cast<uint16_t>(length));
        PPC_STORE_U32(kSaveAnsiString + 4, kSavePath);
    };
    auto CreateSaveFile = [&](uint32_t disposition, uint32_t options = 0x48,
                              uint32_t desiredAccess = 0xC0100080u) {
        PPC_STORE_U32(kSaveHandleOut, 0);
        PPC_STORE_U32(kSaveIoStatus, 0xA5A5A5A5u);
        PPC_STORE_U32(kSaveIoStatus + 4, 0xA5A5A5A5u);
        context = {};
        context.r1.u64 = 0x100;
        context.r3.u64 = kSaveHandleOut;
        context.r4.u64 = desiredAccess;
        context.r5.u64 = kSaveObjectAttributes;
        context.r6.u64 = kSaveIoStatus;
        context.r7.u64 = 0;
        context.r8.u64 = 0x80;
        context.r9.u64 = 1;
        context.r10.u64 = disposition;
        PPC_STORE_U32(context.r1.u32 + 0x54, options);
        __imp__NtCreateFile(context, base);
        return PPC_LOAD_U32(kSaveHandleOut);
    };
    SetSavePath(kSaveProfilePath, sizeof(kSaveProfilePath) - 1);
    uint32_t saveProfileHandle = CreateSaveFile(5);
    if (!Check(context.r3.u32 == 0 && saveProfileHandle != 0 &&
                   PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 2 &&
                   std::filesystem::is_regular_file(hostSaveProfile) &&
                   std::filesystem::file_size(hostSaveProfile) == 0,
               "NtCreateFile FILE_OVERWRITE_IF did not create savedrive:_profile"))
        return 1;

    constexpr char kProfileBytes[] = "TDPR";
    std::memcpy(base + kSaveBuffer, kProfileBytes, sizeof(kProfileBytes) - 1);
    PPC_STORE_U64(kSaveOffset, 0);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r7.u64 = kSaveIoStatus;
    context.r8.u64 = kSaveBuffer;
    context.r9.u64 = sizeof(kProfileBytes) - 1;
    context.r10.u64 = kSaveOffset;
    __imp__NtWriteFile(context, base);
    if (!Check(context.r3.u32 == kStatusPending &&
                   PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == sizeof(kProfileBytes) - 1 &&
                   std::filesystem::file_size(hostSaveProfile) == sizeof(kProfileBytes) - 1,
               "asynchronous NtWriteFile did not persist the mounted save bytes"))
        return 1;

    PPC_STORE_U32(kSaveIoStatus, 0xA5A5A5A5u);
    PPC_STORE_U32(kSaveIoStatus + 4, 0xA5A5A5A5u);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r4.u64 = kSaveIoStatus;
    __imp__NtFlushBuffersFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 0,
               "NtFlushBuffersFile did not synchronously flush the portable save"))
        return 1;
    context = {};
    context.r3.u64 = 0xDEADBEEFu;
    context.r4.u64 = kSaveIoStatus;
    __imp__NtFlushBuffersFile(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle &&
                   PPC_LOAD_U32(kSaveIoStatus) == kStatusInvalidHandle &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 0,
               "NtFlushBuffersFile accepted an invalid file handle"))
        return 1;

    std::memset(base + kSaveBuffer, 0, sizeof(kProfileBytes));
    PPC_STORE_U64(kSaveOffset, 0);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r7.u64 = kSaveIoStatus;
    context.r8.u64 = kSaveBuffer;
    context.r9.u64 = sizeof(kProfileBytes) - 1;
    context.r10.u64 = kSaveOffset;
    __imp__NtReadFile(context, base);
    if (!Check(context.r3.u32 == kStatusPending &&
                   PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == sizeof(kProfileBytes) - 1 &&
                   std::memcmp(base + kSaveBuffer, kProfileBytes,
                               sizeof(kProfileBytes) - 1) == 0,
               "mounted save bytes did not round-trip through NtReadFile"))
        return 1;
    context = {};
    context.r3.u64 = saveProfileHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0, "NtClose did not release savedrive:_profile")) return 1;

    saveProfileHandle = CreateSaveFile(1, 0x60);
    if (!Check(context.r3.u32 == 0 && saveProfileHandle != 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 1,
               "synchronous FILE_OPEN did not reopen savedrive:_profile")) return 1;
    PPC_STORE_U8(kSaveOffset, 0);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r4.u64 = kSaveIoStatus;
    context.r5.u64 = kSaveOffset;
    context.r6.u64 = 1;
    context.r7.u64 = 13;
    __imp__NtSetInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 0 &&
                   std::filesystem::is_regular_file(hostSaveProfile),
               "FileDispositionInformation did not clear delete-on-close")) return 1;
    PPC_STORE_U64(kSaveOffset, 2);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r4.u64 = kSaveIoStatus;
    context.r5.u64 = kSaveOffset;
    context.r6.u64 = 8;
    context.r7.u64 = 14;
    __imp__NtSetInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 8,
               "NtSetInformationFile did not set FilePositionInformation")) return 1;
    PPC_STORE_U64(kSaveOffset, 0);
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r4.u64 = kSaveIoStatus;
    context.r5.u64 = kSaveOffset;
    context.r6.u64 = 8;
    context.r7.u64 = 14;
    __imp__NtQueryInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 8 &&
                   PPC_LOAD_U64(kSaveOffset) == 2,
               "NtQueryInformationFile did not return FilePositionInformation")) return 1;
    std::memset(base + kSaveBuffer, 0, sizeof(kProfileBytes));
    context = {};
    context.r3.u64 = saveProfileHandle;
    context.r7.u64 = kSaveIoStatus;
    context.r8.u64 = kSaveBuffer;
    context.r9.u64 = 2;
    context.r10.u64 = 0;
    __imp__NtReadFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 2 &&
                   base[kSaveBuffer] == 'P' && base[kSaveBuffer + 1] == 'R',
               "null-offset synchronous read ignored the set file position")) return 1;
    PPC_STORE_U64(kSaveOffset, 2);
    for (const uint32_t informationClass : {20u, 19u}) {
        context = {};
        context.r3.u64 = saveProfileHandle;
        context.r4.u64 = kSaveIoStatus;
        context.r5.u64 = kSaveOffset;
        context.r6.u64 = 8;
        context.r7.u64 = informationClass;
        __imp__NtSetInformationFile(context, base);
        if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                       PPC_LOAD_U32(kSaveIoStatus + 4) == 8 &&
                       std::filesystem::file_size(hostSaveProfile) == 2,
                   "NtSetInformationFile did not apply the host-backed file length")) return 1;
    }
    context = {};
    context.r3.u64 = saveProfileHandle;
    __imp__NtClose(context, base);

    constexpr char kDeleteSavePath[] = "savedrive:\\delete_me";
    const auto hostDeleteSave = testPortableContentRoot / "545407EE" /
                                "00000001" / kContentFileName / "delete_me";
    SetSavePath(kDeleteSavePath, sizeof(kDeleteSavePath) - 1);
    const uint32_t deleteSaveHandle =
        CreateSaveFile(5, 0x48, 0xC0110080u); // Includes DELETE.
    PPC_STORE_U8(kSaveOffset, 1);
    context = {};
    context.r3.u64 = deleteSaveHandle;
    context.r4.u64 = kSaveIoStatus;
    context.r5.u64 = kSaveOffset;
    context.r6.u64 = 1;
    context.r7.u64 = 13;
    __imp__NtSetInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 0 &&
                   std::filesystem::is_regular_file(hostDeleteSave),
               "FileDispositionInformation did not set delete-on-close")) return 1;
    context = {};
    context.r3.u64 = deleteSaveHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0 && !std::filesystem::exists(hostDeleteSave),
               "NtClose did not apply the pending portable-file deletion")) return 1;

    // Probe 66 reaches the title's real deletion helper: NtOpenFile requests
    // DELETE without data-write access, then FileDispositionInformation marks
    // Chapter1 for deletion. DELETE is an independent granted access right and
    // must remain available on the resulting portable-content handle.
    constexpr char kDeleteViaOpenPath[] = "savedrive:\\delete_via_open";
    const auto hostDeleteViaOpen = testPortableContentRoot / "545407EE" /
                                   "00000001" / kContentFileName /
                                   "delete_via_open";
    SetSavePath(kDeleteViaOpenPath, sizeof(kDeleteViaOpenPath) - 1);
    uint32_t deleteViaOpenHandle = CreateSaveFile(5);
    context = {};
    context.r3.u64 = deleteViaOpenHandle;
    __imp__NtClose(context, base);
    PPC_STORE_U32(kSaveHandleOut, 0);
    PPC_STORE_U32(kSaveIoStatus, 0xA5A5A5A5u);
    PPC_STORE_U32(kSaveIoStatus + 4, 0xA5A5A5A5u);
    context = {};
    context.r3.u64 = kSaveHandleOut;
    context.r4.u64 = 0x00010000u; // DELETE.
    context.r5.u64 = kSaveObjectAttributes;
    context.r6.u64 = kSaveIoStatus;
    context.r7.u64 = 7;
    context.r8.u64 = 0x4040;
    __imp__NtOpenFile(context, base);
    deleteViaOpenHandle = PPC_LOAD_U32(kSaveHandleOut);
    if (!Check(context.r3.u32 == 0 && deleteViaOpenHandle != 0 &&
                   PPC_LOAD_U32(kSaveIoStatus) == 0 &&
                   std::filesystem::is_regular_file(hostDeleteViaOpen),
               "NtOpenFile did not grant DELETE on portable content")) return 1;
    PPC_STORE_U8(kSaveOffset, 1);
    context = {};
    context.r3.u64 = deleteViaOpenHandle;
    context.r4.u64 = kSaveIoStatus;
    context.r5.u64 = kSaveOffset;
    context.r6.u64 = 1;
    context.r7.u64 = 13;
    __imp__NtSetInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kSaveIoStatus) == 0,
               "DELETE-only handle rejected FileDispositionInformation")) return 1;
    context = {};
    context.r3.u64 = deleteViaOpenHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0 && !std::filesystem::exists(hostDeleteViaOpen),
               "DELETE-only NtOpenFile handle did not delete on close")) return 1;

    SetSavePath(kSaveProfilePath, sizeof(kSaveProfilePath) - 1);
    saveProfileHandle = CreateSaveFile(5);
    if (!Check(context.r3.u32 == 0 && saveProfileHandle != 0 &&
                   PPC_LOAD_U32(kSaveIoStatus + 4) == 3 &&
                   std::filesystem::file_size(hostSaveProfile) == 0,
               "FILE_OVERWRITE_IF did not report and perform an existing-file overwrite"))
        return 1;
    context = {};
    context.r3.u64 = saveProfileHandle;
    __imp__NtClose(context, base);

    constexpr char kReadOnlyCreatePath[] =
        "D:\\runtime_import_readonly_create_denied.bin";
    const auto readOnlyCreateHost = std::filesystem::current_path() /
                                    "runtime_import_readonly_create_denied.bin";
    if (!Check(!std::filesystem::exists(readOnlyCreateHost),
               "read-only create probe path unexpectedly exists before the test")) return 1;
    SetSavePath(kReadOnlyCreatePath, sizeof(kReadOnlyCreatePath) - 1);
    const uint32_t readOnlyCreateHandle = CreateSaveFile(5);
    if (!Check(context.r3.u32 == 0xC0000022u && readOnlyCreateHandle == 0 &&
                   PPC_LOAD_U32(kSaveIoStatus) == 0xC0000022u &&
                   !std::filesystem::exists(readOnlyCreateHost),
               "NtCreateFile escaped the XAM mount and modified extracted title content"))
        return 1;

    constexpr uint32_t kCatalogSize = 0x1D0;
    constexpr uint32_t kCatalogHandle = 0x1D4;
    constexpr uint32_t kCatalogItems = 0x1D8;
    constexpr uint32_t kCatalogBuffer = 0x2A0;
    PPC_STORE_U32(kCatalogSize, 0);
    PPC_STORE_U32(kCatalogHandle, 0);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kGuestPortableContentDeviceId;
    context.r5.u64 = 1;
    context.r6.u64 = 0x1000;
    context.r7.u64 = 1;
    context.r8.u64 = kCatalogSize;
    context.r9.u64 = kCatalogHandle;
    __imp__XamContentCreateEnumerator(context, base);
    const uint32_t persistedCatalogHandle = PPC_LOAD_U32(kCatalogHandle);
    context = {};
    context.r3.u64 = persistedCatalogHandle;
    context.r4.u64 = 0;
    context.r5.u64 = kCatalogBuffer;
    context.r6.u64 = kGuestXContentDataBytes;
    context.r7.u64 = kCatalogItems;
    context.r8.u64 = 0;
    __imp__XamEnumerate(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kCatalogItems) == 1 &&
                   PPC_LOAD_U32(kCatalogBuffer) == kGuestPortableContentDeviceId &&
                   PPC_LOAD_U32(kCatalogBuffer + 4) == 1 &&
                   std::memcmp(base + kCatalogBuffer + 0x108,
                               kContentFileName, sizeof(kContentFileName)) == 0,
               "created portable content was not preserved in the catalog"))
        return 1;
    context = {};
    context.r3.u64 = persistedCatalogHandle;
    __imp__NtClose(context, base);
    context = {};
    context.r3.u64 = kContentRootName;
    context.r4.u64 = 0;
    __imp__XamContentClose(context, base);
    if (!Check(context.r3.u32 == 0 &&
                    !GetGuestFileSystem().QueryGamePath(
                        "savedrive:\\", &mountedContentAttributes) &&
                    std::filesystem::is_directory(hostContentPackage),
                "XamContentClose deleted the package or left its mount active"))
        return 1;


    // Once unmounted, synchronous deletion removes both representations so a
    // future content scan cannot return an orphan header. Repeating the call
    // reports FILE_NOT_FOUND, including the pinned immediate-overlapped layout.
    // Use the record just returned by XamEnumerate, as the title does; the
    // earlier creation buffer shares scratch space with intervening file I/O.
    std::memcpy(base + kCreatedContentData, base + kCatalogBuffer,
                kGuestXContentDataBytes);
    if (!Check(PublishExecutableExecutionInfo(base, kExecutionInfo),
               "could not republish execution info after overlapping scratch use")) return 1;
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kCreatedContentData;
    context.r5.u64 = 0;
    __imp__XamContentDelete(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   !std::filesystem::exists(hostContentPackage) &&
                   !std::filesystem::exists(hostContentHeader),
               "XamContentDelete did not remove the portable package and header")) return 1;
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kCreatedContentData;
    context.r5.u64 = 0;
    __imp__XamContentDelete(context, base);
    if (!Check(context.r3.u32 == 2,
               "XamContentDelete did not report a missing package")) return 1;

    constexpr uint32_t kContentDeleteOverlapped = 0x2E0;
    std::memset(base + kContentDeleteOverlapped, 0, 28);
    PPC_STORE_U32(kContentDeleteOverlapped + 0x0C, deviceEvent.handle);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kCreatedContentData;
    context.r5.u64 = kContentDeleteOverlapped;
    __imp__XamContentDelete(context, base);
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kContentDeleteOverlapped + 0x00) == 2 &&
                   PPC_LOAD_U32(kContentDeleteOverlapped + 0x04) == 0xFFFFFFFFu &&
                   PPC_LOAD_U32(kContentDeleteOverlapped + 0x18) == 2 &&
                   GetGuestObjects().WaitForSingleObject(deviceEvent.handle, true, 0) == 0,
               "XamContentDelete did not complete the missing overlapped delete")) return 1;

    std::memset(base + kCreatedContentData + 0x108, 0, 42);
    std::memcpy(base + kCreatedContentData + 0x108, "..", 3);
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = kCreatedContentData;
    context.r5.u64 = 0;
    __imp__XamContentDelete(context, base);
    if (!Check(context.r3.u32 == 87 &&
                   std::filesystem::is_directory(testPortableContentRoot),
               "XamContentDelete accepted an unsafe package component")) return 1;
    std::filesystem::remove_all(testPortableContentRoot, portableContentError);
    if (!Check(!portableContentError,
               "could not clean the isolated portable-content test root")) return 1;

    PPC_STORE_U32(kContentItemsOut, 0xFFFFFFFFu);
    context = {};
    context.r3.u64 = contentEnumerator;
    context.r4.u64 = 0;
    context.r5.u64 = kContentBuffer;
    context.r6.u64 = 0x134;
    context.r7.u64 = kContentItemsOut;
    __imp__XamEnumerate(context, base);
    if (!Check(context.r3.u32 == 6 && PPC_LOAD_U32(kContentItemsOut) == 0,
               "XamEnumerate accepted a closed enumerator handle")) return 1;

    const auto xmpNotify = GetGuestObjects().CreateNotifyListener(uint64_t{1} << 5, 0);
    constexpr uint32_t kXmpControllerPayload = 0x360;
    PPC_STORE_U32(kXmpControllerPayload + 0, 2);
    PPC_STORE_U32(kXmpControllerPayload + 4, 0);
    PPC_STORE_U32(kXmpControllerPayload + 8, 1);
    context = {};
    context.r3.u64 = 0xFA;
    context.r4.u64 = 0x0007001A;
    context.r5.u64 = 0;
    context.r6.u64 = kXmpControllerPayload;
    context.r7.u64 = 12;
    context.r8.u64 = 0;
    __imp__XMsgStartIORequestEx(context, base);
    uint32_t xmpNotification{};
    uint32_t xmpParameter{};
    const bool xmpNotified = GetGuestObjects().DequeueNotification(
        xmpNotify.handle, 0, &xmpNotification, &xmpParameter);
    if (!Check(context.r3.u32 == 0 && GetGuestXamState().XmpPlaybackClient() == 1 &&
                   xmpNotified && xmpNotification == 0x0A000003u && xmpParameter == 0,
               "XMPSetPlaybackController did not retain state and broadcast notification")) return 1;

    // The Darkness sub_828D5510 uses the exact (client 2, controller 4,
    // playback client 0) payload to release playback control. Verify the
    // observed title path without broadening acceptance to unknown selectors.
    PPC_STORE_U32(kXmpControllerPayload + 0, 2);
    PPC_STORE_U32(kXmpControllerPayload + 4, 4);
    PPC_STORE_U32(kXmpControllerPayload + 8, 0);
    context = {};
    context.r3.u64 = 0xFA;
    context.r4.u64 = 0x0007001A;
    context.r5.u64 = 0;
    context.r6.u64 = kXmpControllerPayload;
    context.r7.u64 = 12;
    context.r8.u64 = 0;
    __imp__XMsgStartIORequestEx(context, base);
    xmpNotification = 0;
    xmpParameter = 0;
    const bool xmpReleaseNotified = GetGuestObjects().DequeueNotification(
        xmpNotify.handle, 0, &xmpNotification, &xmpParameter);
    if (!Check(context.r3.u32 == 0 && GetGuestXamState().XmpPlaybackClient() == 0 &&
                   xmpReleaseNotified && xmpNotification == 0x0A000003u &&
                   xmpParameter == 1,
               "XMP playback-control release did not retain state and broadcast notification"))
        return 1;
    const auto xamEvent = GetGuestObjects().CreateEvent(false, false);
    constexpr uint32_t kXmpPayload = 0x320;
    constexpr uint32_t kXmpOverlapped = 0x340;
    PPC_STORE_U32(kXmpPayload + 0, 2);
    PPC_STORE_U32(kXmpPayload + 4, 0x827B7630u);
    PPC_STORE_U32(kXmpPayload + 8, 0x12345678u);
    PPC_STORE_U32(kXmpPayload + 12, 1);
    std::memset(base + kXmpOverlapped, 0, 28);
    PPC_STORE_U32(kXmpOverlapped + 0x0C, xamEvent.handle);
    context = {};
    context.r3.u64 = 0xFA;
    context.r4.u64 = 0x0007003D;
    context.r5.u64 = kXmpOverlapped;
    context.r6.u64 = kXmpPayload;
    context.r7.u64 = 16;
    __imp__XMsgStartIORequest(context, base);
    if (!Check(context.r3.u32 == 997 &&
                   PPC_LOAD_U32(kXmpOverlapped + 0x00) == 0x80004005u &&
                   PPC_LOAD_U32(kXmpOverlapped + 0x04) == UINT32_MAX &&
                   PPC_LOAD_U32(kXmpOverlapped + 0x08) == xamThread.handle &&
                   PPC_LOAD_U32(kXmpOverlapped + 0x0C) == xamEvent.handle &&
                   PPC_LOAD_U32(kXmpOverlapped + 0x18) == 0x80004005u &&
                   GetGuestObjects().WaitForSingleObject(xamEvent.handle, true, 0) == 0,
               "XMPCaptureOutput did not complete the XAM failure contract")) return 1;

    PPC_STORE_U32(kXmpPayload + 0, 2);
    PPC_STORE_U32(kXmpPayload + 4, 0x3F000000u);
    context = {};
    context.r3.u64 = 0xFA;
    context.r4.u64 = 0x0007000C;
    context.r5.u64 = 0;
    context.r6.u64 = kXmpPayload;
    context.r7.u64 = 8;
    __imp__XMsgStartIORequest(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   GetGuestXamState().XmpVolumeBits() == 0x3F000000u,
               "XMPSetVolume did not retain the exact guest float bits")) return 1;

    // X_VIDEO_MODE consists of twelve big-endian 32-bit fields. RefreshRate
    // is a float at +0x14, so 60.0f must be stored as 0x42700000 rather than
    // the integer 60. Verify the complete game-visible 48-byte result.
    constexpr uint32_t kVideoModeAddress = 0x280;
    constexpr uint32_t kVideoModeBytes = 48;
    constexpr uint32_t kVideoModeExpected[kVideoModeBytes / 4] = {
        1280, 720, 0, 1, 1, 0x42700000, 1, 0x4A, 1, 0, 0, 0,
    };
    static_assert(sizeof(kVideoModeExpected) == kVideoModeBytes);
    std::memset(base + kVideoModeAddress, 0xA5, kVideoModeBytes);
    context = {};
    context.r3.u64 = kVideoModeAddress;
    __imp__XGetVideoMode(context, base);
    for (uint32_t index = 0; index < kVideoModeBytes / 4; ++index) {
        if (!Check(PPC_LOAD_U32(kVideoModeAddress + index * 4) == kVideoModeExpected[index],
                   "XGetVideoMode returned an incorrect field or byte order")) return 1;
    }
    std::memset(base + kVideoModeAddress, 0xA5, kVideoModeBytes);
    context = {};
    context.r3.u64 = kVideoModeAddress;
    __imp__VdQueryVideoMode(context, base);
    for (uint32_t index = 0; index < kVideoModeBytes / 4; ++index) {
        if (!Check(PPC_LOAD_U32(kVideoModeAddress + index * 4) == kVideoModeExpected[index],
                   "VdQueryVideoMode diverged from the verified X_VIDEO_MODE contract")) return 1;
    }
    context = {};
    __imp__VdQueryVideoFlags(context, base);
    if (!Check(context.r3.u32 == 3,
               "VdQueryVideoFlags did not derive the 1280x720 widescreen flags")) return 1;
    constexpr uint32_t kGammaTypeAddress = 0x260;
    constexpr uint32_t kGammaPowerAddress = 0x264;
    PPC_STORE_U32(kGammaTypeAddress, 0xA5A5A5A5u);
    PPC_STORE_U32(kGammaPowerAddress, 0xA5A5A5A5u);
    context = {};
    context.r3.u64 = kGammaTypeAddress;
    context.r4.u64 = kGammaPowerAddress;
    __imp__VdGetCurrentDisplayGamma(context, base);
    if (!Check(PPC_LOAD_U32(kGammaTypeAddress) == 2 &&
                   PPC_LOAD_U32(kGammaPowerAddress) == 0x400E38E4u,
               "VdGetCurrentDisplayGamma returned the wrong BT.709 contract")) return 1;
    context = {};
    context.r3.u64 = 0xDEADBEEFu;
    __imp__VdRetrainEDRAMWorker(context, base);
    if (!Check(context.r3.u32 == 0,
               "VdRetrainEDRAMWorker reported a fabricated physical retrain")) return 1;
    constexpr uint32_t kEdramOutput0 = 0x300;
    constexpr uint32_t kEdramOutput1 = 0x304;
    constexpr uint32_t kEdramOutput2 = 0x308;
    PPC_STORE_U32(kEdramOutput0, 0x11111111u);
    PPC_STORE_U32(kEdramOutput1, 0x22222222u);
    PPC_STORE_U32(kEdramOutput2, 0x33333333u);
    context = {};
    context.r3.u64 = 0x12345678u;
    context.r4.u64 = kEdramOutput0;
    context.r5.u64 = 0x1000;
    context.r6.u64 = kEdramOutput1;
    context.r7.u64 = kEdramOutput2;
    context.r8.u64 = 0x800;
    __imp__VdRetrainEDRAM(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kEdramOutput0) == 0x11111111u &&
                   PPC_LOAD_U32(kEdramOutput1) == 0x22222222u &&
                   PPC_LOAD_U32(kEdramOutput2) == 0x33333333u,
               "VdRetrainEDRAM fabricated hardware success or output data")) return 1;
    constexpr uint32_t kScalingNotification = 0x2C0;
    PPC_STORE_U16(kScalingNotification + 0, 1280);
    PPC_STORE_U16(kScalingNotification + 2, 720);
    PPC_STORE_U16(kScalingNotification + 4, 640);
    PPC_STORE_U16(kScalingNotification + 6, 360);
    context = {};
    context.r3.u64 = 1;
    context.r4.u64 = kScalingNotification;
    __imp__VdCallGraphicsNotificationRoutines(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U16(kScalingNotification + 0) == 1280 &&
                   PPC_LOAD_U16(kScalingNotification + 2) == 720 &&
                   PPC_LOAD_U16(kScalingNotification + 4) == 640 &&
                   PPC_LOAD_U16(kScalingNotification + 6) == 360,
               "graphics notification did not preserve the verified scaling record")) return 1;
    if (!Check(RuntimeGeneratedAddressInRange(PPC_CODE_BASE) &&
                   RuntimeGeneratedAddressInRange(PPC_CODE_BASE + 4) &&
                   !RuntimeGeneratedAddressInRange(PPC_CODE_BASE - 4) &&
                   !RuntimeGeneratedAddressInRange(PPC_CODE_BASE + 2) &&
                   !RuntimeGeneratedAddressInRange(
                       static_cast<uint32_t>(uint64_t(PPC_CODE_BASE) + PPC_CODE_SIZE)),
               "generated-address validation accepted an unsafe lookup index")) return 1;
    if (!Check(!RuntimeInterruptDeliveryAllowedAtFunction(0x828999F0u) &&
                   !RuntimeInterruptDeliveryAllowedAtFunction(0x82899A78u) &&
                   !RuntimeInterruptDeliveryAllowedAtFunction(0x8289A1DCu) &&
                   !RuntimeInterruptDeliveryAllowedAtFunction(0x829B8D04u) &&
                   !RuntimeInterruptDeliveryAllowedAtFunction(0x829B9194u) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x821F1260u) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x82899A38u) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x82899A88u) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x8289A1D8u) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x829B8CFCu) &&
                   RuntimeInterruptDeliveryAllowedAtFunction(0x829B9198u),
               "compiler register helpers were exposed as interrupt delivery boundaries")) return 1;
    const PPCContext criticalRegionOriginal = context;
    __imp__KeEnterCriticalRegion(context, base);
    __imp__KeEnterCriticalRegion(context, base);
    if (!Check(CurrentGuestApcDisableCount() == -2 &&
                   std::memcmp(&context, &criticalRegionOriginal, sizeof(context)) == 0,
               "KeEnterCriticalRegion did not preserve context or nesting")) return 1;
    __imp__KeLeaveCriticalRegion(context, base);
    __imp__KeLeaveCriticalRegion(context, base);
    if (!Check(CurrentGuestApcDisableCount() == 0 &&
                   std::memcmp(&context, &criticalRegionOriginal, sizeof(context)) == 0,
               "KeLeaveCriticalRegion did not restore the APC-disable nesting")) return 1;

    context = {};
    context.r3.u64 = 0xBFFF3008u;
    __imp__VdSetSystemCommandBufferGpuIdentifierAddress(context, base);
    if (!Check(RuntimeGraphicsSystemCommandBufferGpuIdentifierAddress() == 0xBFFF3008u,
               "system command-buffer GPU identifier address was not retained")) return 1;
    context = {};
    context.r3.u64 = 0x280;
    context.r4.u64 = 0x13;
    std::memset(base + 0x280, 0xA5, 32);
    __imp__KiApcNormalRoutineNop(context, base);
    if (!Check(context.r3.u64 == 0 && base[0x280] == 0xA5,
               "KiApcNormalRoutineNop did not preserve its verified return-only contract")) return 1;
    __imp__VdShutdownEngines(context, base);
    if (!Check(RuntimeGraphicsSystemCommandBufferGpuIdentifierAddress() == 0,
               "graphics shutdown did not clear the system command-buffer identifier address")) return 1;

    constexpr uint32_t kSystemTimeAddress = 0x80;
    constexpr uint64_t kWindowsToUnixEpochTicks = 11644473600ULL * 10'000'000ULL;
    context.r3.u64 = kSystemTimeAddress;
    __imp__KeQuerySystemTime(context, base);
    if (!Check(PPC_LOAD_U64(kSystemTimeAddress) > kWindowsToUnixEpochTicks,
               "KeQuerySystemTime did not write a FILETIME-epoch system time")) return 1;

    constexpr uint32_t kTimeFieldsAddress = 0x90;
    PPC_STORE_U64(kSystemTimeAddress, kWindowsToUnixEpochTicks);
    context.r3.u64 = kSystemTimeAddress;
    context.r4.u64 = kTimeFieldsAddress;
    __imp__RtlTimeToTimeFields(context, base);
    if (!Check(PPC_LOAD_U16(kTimeFieldsAddress + 0) == 1970 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 2) == 1 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 4) == 1 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 6) == 0 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 8) == 0 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 10) == 0 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 12) == 0 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 14) == 4,
               "RtlTimeToTimeFields did not convert the Unix epoch to Thursday 1970-01-01"))
        return 1;

    constexpr uint32_t kConvertedTimeAddress = 0xA0;
    context = {};
    context.r3.u64 = kTimeFieldsAddress;
    context.r4.u64 = kConvertedTimeAddress;
    PPC_STORE_U64(kConvertedTimeAddress, 0xAAAAAAAA55555555ull);
    __imp__RtlTimeFieldsToTime(context, base);
    if (!Check(context.r3.u32 == 1 &&
                   PPC_LOAD_U64(kConvertedTimeAddress) == kWindowsToUnixEpochTicks,
               "RtlTimeFieldsToTime did not invert the Unix epoch fields")) return 1;

    PPC_STORE_U16(kTimeFieldsAddress + 0, 2000);
    PPC_STORE_U16(kTimeFieldsAddress + 2, 2);
    PPC_STORE_U16(kTimeFieldsAddress + 4, 29);
    PPC_STORE_U16(kTimeFieldsAddress + 6, 23);
    PPC_STORE_U16(kTimeFieldsAddress + 8, 59);
    PPC_STORE_U16(kTimeFieldsAddress + 10, 59);
    PPC_STORE_U16(kTimeFieldsAddress + 12, 999);
    PPC_STORE_U16(kTimeFieldsAddress + 14, 0xFFFF);
    context.r3.u64 = kTimeFieldsAddress;
    context.r4.u64 = kConvertedTimeAddress;
    __imp__RtlTimeFieldsToTime(context, base);
    if (!Check(context.r3.u32 == 1,
               "RtlTimeFieldsToTime rejected a valid Gregorian leap day")) return 1;
    context.r3.u64 = kConvertedTimeAddress;
    context.r4.u64 = kTimeFieldsAddress;
    __imp__RtlTimeToTimeFields(context, base);
    if (!Check(PPC_LOAD_U16(kTimeFieldsAddress + 0) == 2000 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 2) == 2 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 4) == 29 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 6) == 23 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 8) == 59 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 10) == 59 &&
                   PPC_LOAD_U16(kTimeFieldsAddress + 12) == 999,
               "RtlTimeFieldsToTime leap-day result did not round-trip")) return 1;

    PPC_STORE_U16(kTimeFieldsAddress + 0, 1900);
    PPC_STORE_U16(kTimeFieldsAddress + 2, 2);
    PPC_STORE_U16(kTimeFieldsAddress + 4, 29);
    PPC_STORE_U64(kConvertedTimeAddress, 0xAAAAAAAA55555555ull);
    context.r3.u64 = kTimeFieldsAddress;
    context.r4.u64 = kConvertedTimeAddress;
    __imp__RtlTimeFieldsToTime(context, base);
    if (!Check(context.r3.u32 == 0 &&
                   PPC_LOAD_U64(kConvertedTimeAddress) == 0xAAAAAAAA55555555ull,
               "RtlTimeFieldsToTime accepted an invalid date or modified failed output"))
        return 1;

    // DbgBreakPoint is a void Xbox debug-stub export. It must preserve the
    // guest context and return; treating it as an unresolved fatal import
    // makes normal retail startup timing-dependent.
    context = {};
    context.r3.u64 = 0x12345678;
    __imp__DbgBreakPoint(context, base);
    if (!Check(context.r3.u64 == 0x12345678, "DbgBreakPoint changed guest result state")) return 1;

    context.r3.u64 = 0;
    __imp__MmQueryStatistics(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidParameter, "null MmQueryStatistics buffer was accepted")) return 1;

    std::memset(base + kStatsAddress, 0xA5, kStatsSize);
    PPC_STORE_U32(kStatsAddress, kStatsSize - 4);
    context.r3.u64 = kStatsAddress;
    __imp__MmQueryStatistics(context, base);
    if (!Check(context.r3.u32 == kStatusBufferTooSmall, "invalid MmQueryStatistics size was accepted") ||
        !Check(base[kStatsAddress + 8] == 0xA5, "invalid-size buffer was modified")) return 1;

    auto& memory = GetGuestMemoryAccounting();
    memory.Reset();
    PPC_STORE_U32(kStatsAddress, kStatsSize);
    if (!Query(context, base) ||
        !Check(PPC_LOAD_U32(kStatsAddress + 0) == kStatsSize, "result size is wrong") ||
        !Check(PPC_LOAD_U32(kStatsAddress + 4) == 0x20000, "total pages are wrong") ||
        !Check(PPC_LOAD_U32(kStatsAddress + 12) == 0x20000, "initial available pages are wrong") ||
        !Check(PPC_LOAD_U32(kStatsAddress + 100) == 0x1FFFF, "highest page is wrong") ||
        !Check(base[kStatsAddress + 0] == 0 && base[kStatsAddress + 3] == 104 &&
               base[kStatsAddress + 4] == 0 && base[kStatsAddress + 5] == 2,
               "statistics fields are not big-endian")) return 1;

    uint32_t firstPage{};
    if (!Check(memory.ReservePhysicalPages(3, 1, 0, 0x1FFFF, 0x4, &firstPage), "physical page reservation failed") ||
        !Check(firstPage == 0, "first physical allocation was not lowest available page")) return 1;
    PPC_STORE_U32(kStatsAddress, kStatsSize);
    if (!Query(context, base) ||
        !Check(PPC_LOAD_U32(kStatsAddress + 12) == 0x1FFFD, "allocation was not reflected in availability")) return 1;
    if (!Check(memory.ReleasePhysicalPages(firstPage), "physical page release failed")) return 1;
    PPC_STORE_U32(kStatsAddress, kStatsSize);
    if (!Query(context, base) ||
        !Check(PPC_LOAD_U32(kStatsAddress + 12) == 0x20000, "release was not reflected in availability")) return 1;

    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0x1F000000;
    context.r5.u64 = 0x80000004;
    context.r6.u64 = 0;
    context.r7.u64 = 0xFFFFFFFF;
    context.r8.u64 = 0;
    __imp__MmAllocatePhysicalMemoryEx(context, base);
    if (!Check(context.r3.u32 == 0xA0000000, "16 MiB physical allocation returned wrong alias")) return 1;
    context = {};
    context.r3.u64 = 0xA0000000;
    __imp__MmQueryAllocationSize(context, base);
    if (!Check(context.r3.u32 == 0x1F000000, "MmQueryAllocationSize did not return tracked allocation size")) return 1;
    context.r3.u64 = 0xA0001001;
    __imp__MmQueryAddressProtect(context, base);
    if (!Check(context.r3.u32 == 0x4, "MmQueryAddressProtect did not resolve an interior allocation address")) return 1;
    context.r3.u64 = 0xBFFFFFFF;
    __imp__MmQueryAddressProtect(context, base);
    if (!Check(context.r3.u32 == 0, "MmQueryAddressProtect accepted an untracked physical address")) return 1;
    context.r3.u64 = 0xA0001000;
    __imp__MmQueryAllocationSize(context, base);
    if (!Check(context.r3.u32 == 0, "MmQueryAllocationSize accepted an interior allocation address")) return 1;
    context.r3.u64 = 0x7D000000;
    __imp__MmQueryAllocationSize(context, base);
    if (!Check(context.r3.u32 == 0, "MmQueryAllocationSize accepted an untracked address")) return 1;
    std::memset(base + 0x280, 0xA5, 32);
    context = {};
    context.r3.u64 = 0x280;
    context.r4.u64 = 1020;
    __imp__XamLoaderGetLaunchData(context, base);
    if (!Check(context.r3.u32 == 0x490 && base[0x280] == 0xA5 && base[0x29F] == 0xA5,
               "absent XAM launch data did not report not-found without modifying the buffer")) return 1;
    GetGuestFileCacheConfiguration().ResetForTests();
    context = {};
    context.r3.u64 = 0;
    context.r4.u64 = 0x20;
    __imp__FscSetCacheElementCount(context, base);
    if (!Check(context.r3.u32 == 0 && GetGuestFileCacheConfiguration().ElementCount(0) == 0x20,
               "FSC cache element configuration was not retained")) return 1;
    context = {};
    context.r3.u64 = 0xC0000034u;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 2, "RtlNtStatusToDosError did not map object-name-not-found to file-not-found")) return 1;
    context = {};
    context.r3.u64 = 0xC000000Fu;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 2, "RtlNtStatusToDosError did not map no-such-file to file-not-found")) return 1;
    context = {};
    context.r3.u64 = kStatusNoMoreFiles;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 18, "RtlNtStatusToDosError did not map no-more-files to ERROR_NO_MORE_FILES")) return 1;
    context = {};
    context.r3.u64 = kStatusInvalidHandle;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 6, "RtlNtStatusToDosError did not map invalid handle")) return 1;
    context = {};
    context.r3.u64 = 0xC0000022u;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 5, "RtlNtStatusToDosError did not map access denied")) return 1;
    context = {};
    context.r3.u64 = 0x80070005u;
    __imp__RtlNtStatusToDosError(context, base);
    if (!Check(context.r3.u32 == 5, "RtlNtStatusToDosError did not preserve DOS-encoded status")) return 1;
    std::memcpy(base + 0x380, "cache", 6);
    context = {};
    context.r3.u64 = 0x370;
    context.r4.u64 = 0x380;
    __imp__RtlInitAnsiString(context, base);
    if (!Check(PPC_LOAD_U16(0x370) == 5 && PPC_LOAD_U16(0x372) == 6 && PPC_LOAD_U32(0x374) == 0x380,
               "RtlInitAnsiString did not create the expected guest ANSI string")) return 1;
    context = {};
    context.r3.u64 = 0x370;
    context.r4.u64 = 0;
    __imp__RtlInitAnsiString(context, base);
    if (!Check(PPC_LOAD_U16(0x370) == 0 && PPC_LOAD_U16(0x372) == 0 && PPC_LOAD_U32(0x374) == 0,
               "RtlInitAnsiString did not reset a null-source guest string")) return 1;
    PPC_STORE_U16(0x390, u'H');
    PPC_STORE_U16(0x392, u'i');
    PPC_STORE_U16(0x394, 0);
    context = {};
    context.r3.u64 = 0x3A0;
    context.r4.u64 = 0x390;
    __imp__RtlInitUnicodeString(context, base);
    if (!Check(PPC_LOAD_U16(0x3A0) == 4 && PPC_LOAD_U16(0x3A2) == 6 &&
                   PPC_LOAD_U32(0x3A4) == 0x390,
               "RtlInitUnicodeString did not create the expected guest Unicode string")) return 1;
    context = {};
    context.r3.u64 = 0x3A0;
    context.r4.u64 = 0;
    __imp__RtlInitUnicodeString(context, base);
    if (!Check(PPC_LOAD_U16(0x3A0) == 0 && PPC_LOAD_U16(0x3A2) == 0 &&
                   PPC_LOAD_U32(0x3A4) == 0,
               "RtlInitUnicodeString did not reset a null-source guest string")) return 1;
    base[0x3B0] = 'A';
    base[0x3B1] = 0xE9;
    base[0x3B2] = 'Z';
    std::memset(base + 0x3C0, 0xA5, 8);
    PPC_STORE_U32(0x3D0, 0xA5A5A5A5u);
    context = {};
    context.r3.u64 = 0x3C0;
    context.r4.u64 = 4;
    context.r5.u64 = 0x3D0;
    context.r6.u64 = 0x3B0;
    context.r7.u64 = 3;
    __imp__RtlMultiByteToUnicodeN(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U16(0x3C0) == 0x0041 &&
                   PPC_LOAD_U16(0x3C2) == 0x00E9 && PPC_LOAD_U32(0x3D0) == 4 &&
                   PPC_LOAD_U32(0x3C4) == 0xA5A5A5A5u,
               "RtlMultiByteToUnicodeN did not widen and bound the guest input exactly")) return 1;
    constexpr char kMissingEnvironmentPath[] = "D:\\EnvironmentXbox.cfg";
    std::memcpy(base + 0x300, kMissingEnvironmentPath, sizeof(kMissingEnvironmentPath) - 1);
    PPC_STORE_U32(0x2B0 + 0, 0xFFFFFFFDu);
    PPC_STORE_U32(0x2B0 + 4, 0x2C0);
    PPC_STORE_U32(0x2B0 + 8, 0x40);
    PPC_STORE_U16(0x2C0 + 0, sizeof(kMissingEnvironmentPath) - 1);
    PPC_STORE_U32(0x2C0 + 4, 0x300);
    std::memset(base + 0x330, 0xA5, 32);
    context = {};
    context.r3.u64 = 0x2B0;
    context.r4.u64 = 0x330;
    __imp__NtQueryFullAttributesFile(context, base);
    if (!Check(context.r3.u32 == 0xC0000034 && base[0x330] == 0xA5,
                "missing EnvironmentXbox.cfg did not return object-name-not-found without output mutation")) return 1;
    constexpr char kBareQueryPath[] = "runtime_import_test.cpp";
    std::memcpy(base + 0x300, kBareQueryPath, sizeof(kBareQueryPath) - 1);
    PPC_STORE_U16(0x2C0 + 0, sizeof(kBareQueryPath) - 1);
    std::memset(base + 0x330, 0xA5, 56);
    context = {};
    context.r3.u64 = 0x2B0;
    context.r4.u64 = 0x330;
    __imp__NtQueryFullAttributesFile(context, base);
    if (!Check(context.r3.u32 == 0xC000000F && base[0x330] == 0xA5,
               "bare NtQueryFullAttributesFile name was incorrectly bound to the game root")) return 1;
    constexpr char kQualifiedQueryPath[] = "D:\\runtime_import_test.cpp";
    std::memcpy(base + 0x300, kQualifiedQueryPath, sizeof(kQualifiedQueryPath) - 1);
    PPC_STORE_U16(0x2C0 + 0, sizeof(kQualifiedQueryPath) - 1);
    std::memset(base + 0x330, 0, 56);
    context = {};
    context.r3.u64 = 0x2B0;
    context.r4.u64 = 0x330;
    __imp__NtQueryFullAttributesFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U64(0x330 + 40) != 0,
               "drive-qualified NtQueryFullAttributesFile path did not resolve against the game root")) return 1;
    context = {};
    context.r3.u64 = 3;
    context.r4.u64 = 9;
    context.r5.u64 = 0x330;
    context.r6.u64 = 4;
    context.r7.u64 = 0x334;
    __imp__ExGetXConfigSetting(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x330) == 1 && PPC_LOAD_U16(0x334) == 4,
               "ExGetXConfigSetting did not return the selected English language setting")) return 1;
    context = {};
    context.r3.u64 = 3;
    context.r4.u64 = 9;
    context.r5.u64 = 0x330;
    context.r6.u64 = 3;
    context.r7.u64 = 0x334;
    __imp__ExGetXConfigSetting(context, base);
    if (!Check(context.r3.u32 == kStatusBufferTooSmall && PPC_LOAD_U16(0x334) == 4,
               "ExGetXConfigSetting did not report its required language-setting size")) return 1;
    for (const uint16_t setting : {uint16_t{1}, uint16_t{2}, uint16_t{3}, uint16_t{4},
                                   uint16_t{5}, uint16_t{6}, uint16_t{7}}) {
        PPC_STORE_U32(0x330, 0xFFFFFFFFu);
        PPC_STORE_U16(0x334, 0);
        context = {};
        context.r3.u64 = 3;
        context.r4.u64 = setting;
        context.r5.u64 = 0x330;
        context.r6.u64 = 4;
        context.r7.u64 = 0x334;
        __imp__ExGetXConfigSetting(context, base);
        if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x330) == 0 &&
                       PPC_LOAD_U16(0x334) == 4,
                   "ExGetXConfigSetting did not return a reached zero-valued user setting"))
            return 1;
    }
    for (const auto [setting, expected] :
         {std::pair<uint16_t, uint32_t>{10, 0x00040000u},
          std::pair<uint16_t, uint32_t>{12, 0x00000040u}}) {
        PPC_STORE_U32(0x330, 0xFFFFFFFFu);
        PPC_STORE_U16(0x334, 0);
        context = {};
        context.r3.u64 = 3;
        context.r4.u64 = setting;
        context.r5.u64 = 0x330;
        context.r6.u64 = 4;
        context.r7.u64 = 0x334;
        __imp__ExGetXConfigSetting(context, base);
        if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x330) == expected &&
                       PPC_LOAD_U16(0x334) == 4,
                   "ExGetXConfigSetting did not return the verified user flag value"))
            return 1;
    }
    GetGuestFileSystem().ResetForTests();
    constexpr uint32_t kFileHandleOut = 0x2A0;
    constexpr uint32_t kFileAttributes = 0x2B0;
    constexpr uint32_t kFileAnsiString = 0x2C0;
    constexpr uint32_t kFileName = 0x300;
    constexpr uint32_t kFileIoStatus = 0x340;
    constexpr uint32_t kFileBuffer = 0x360;
    constexpr char kRawCachePath[] = "\\Device\\Harddisk0\\partition0";
    std::memcpy(base + kFileName, kRawCachePath, sizeof(kRawCachePath) - 1);
    PPC_STORE_U32(kFileAttributes + 0, 0);
    PPC_STORE_U32(kFileAttributes + 4, kFileAnsiString);
    PPC_STORE_U32(kFileAttributes + 8, 0x40);
    PPC_STORE_U16(kFileAnsiString + 0, sizeof(kRawCachePath) - 1);
    PPC_STORE_U16(kFileAnsiString + 2, sizeof(kRawCachePath));
    PPC_STORE_U32(kFileAnsiString + 4, kFileName);
    context = {};
    context.r3.u64 = kFileHandleOut;
    context.r4.u64 = 0xC0100000;
    context.r5.u64 = kFileAttributes;
    context.r6.u64 = kFileIoStatus;
    context.r7.u64 = 0;
    context.r8.u64 = 4;
    context.r9.u64 = 1;
    context.r10.u64 = 1;
    __imp__NtCreateFile(context, base);
    if (!Check(context.r3.u32 == 0xC0000034 && PPC_LOAD_U32(kFileHandleOut) == 0 &&
               PPC_LOAD_U32(kFileIoStatus) == 0xC0000034 && PPC_LOAD_U32(kFileIoStatus + 4) == 0,
               "portable raw-cache policy did not expose the real missing-device result")) return 1;
    // NtOpenFile is an open-only path. Verify that an existing directory is
    // represented by a real guest handle and that FileFsSizeInformation uses
    // a complete 24-byte big-endian result rather than a synthetic value.
    GetGuestFileSystem().SetGameRoot(std::filesystem::current_path());
    constexpr uint32_t kDirectoryHandleOut = 0x200;
    constexpr uint32_t kDirectoryAttributes = 0x210;
    constexpr uint32_t kDirectoryAnsiString = 0x220;
    constexpr uint32_t kDirectoryName = 0x230;
    constexpr uint32_t kDirectoryIoStatus = 0x240;
    constexpr uint32_t kVolumeOutput = 0x260;
    base[kDirectoryName] = '.';
    PPC_STORE_U32(kDirectoryAttributes + 0, 0xFFFFFFFDu);
    PPC_STORE_U32(kDirectoryAttributes + 4, kDirectoryAnsiString);
    PPC_STORE_U32(kDirectoryAttributes + 8, 0x40);
    PPC_STORE_U16(kDirectoryAnsiString + 0, 1);
    PPC_STORE_U16(kDirectoryAnsiString + 2, 1);
    PPC_STORE_U32(kDirectoryAnsiString + 4, kDirectoryName);
    context = {};
    context.r3.u64 = kDirectoryHandleOut;
    context.r4.u64 = 0x100001;
    context.r5.u64 = kDirectoryAttributes;
    context.r6.u64 = kDirectoryIoStatus;
    context.r7.u64 = 3;
    context.r8.u64 = 0x4021;
    __imp__NtOpenFile(context, base);
    const uint32_t directoryHandle = PPC_LOAD_U32(kDirectoryHandleOut);
    if (!Check(context.r3.u32 == 0 && directoryHandle != 0 &&
               PPC_LOAD_U32(kDirectoryIoStatus) == 0 && PPC_LOAD_U32(kDirectoryIoStatus + 4) == 1,
               "NtOpenFile did not open the existing game-root directory")) return 1;
    context = {};
    context.r3.u64 = directoryHandle;
    context.r4.u64 = kDirectoryIoStatus;
    context.r5.u64 = kVolumeOutput;
    context.r6.u64 = 24;
    context.r7.u64 = 3;
    __imp__NtQueryVolumeInformationFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDirectoryIoStatus + 4) == 24 &&
               PPC_LOAD_U32(kVolumeOutput + 16) != 0 && PPC_LOAD_U32(kVolumeOutput + 20) != 0,
               "NtQueryVolumeInformationFile did not return FileFsSizeInformation")) return 1;
    base[kDirectoryName] = '*';
    PPC_STORE_U16(kDirectoryAnsiString + 0, 1);
    PPC_STORE_U16(kDirectoryAnsiString + 2, 1);
    PPC_STORE_U32(kDirectoryAnsiString + 4, kDirectoryName);
    context = {};
    context.r3.u64 = directoryHandle;
    context.r7.u64 = kDirectoryIoStatus;
    context.r8.u64 = kVolumeOutput;
    context.r9.u64 = 128;
    context.r10.u64 = kDirectoryAnsiString;
    context.r11.u64 = 0xDEADBEEFu;
    context.r1.u64 = 0x100;
    PPC_STORE_U32(context.r1.u32 + 0x54, 0);
    __imp__NtQueryDirectoryFile(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDirectoryIoStatus + 4) == 128 &&
               PPC_LOAD_U32(kVolumeOutput + 0) == 0 && PPC_LOAD_U32(kVolumeOutput + 60) != 0,
               "NtQueryDirectoryFile did not return a real directory child")) return 1;
    // The dynamically used Xbox enumeration protocol supplies the wildcard
    // only on the first query, then passes a null name until NO_MORE_FILES.
    // The terminal call must clear IO_STATUS_BLOCK.Information.
    uint32_t continuationQueries = 0;
    do {
        context = {};
        context.r3.u64 = directoryHandle;
        context.r7.u64 = kDirectoryIoStatus;
        context.r8.u64 = kVolumeOutput;
        context.r9.u64 = 128;
        context.r10.u64 = 0;
        context.r11.u64 = 0xDEADBEEFu;
        context.r1.u64 = 0x100;
        PPC_STORE_U32(context.r1.u32 + 0x54, 0);
        __imp__NtQueryDirectoryFile(context, base);
        if (++continuationQueries > 4096) {
            std::cerr << "NtQueryDirectoryFile enumeration did not terminate\n";
            return 1;
        }
    } while (context.r3.u32 == 0);
    if (!Check(context.r3.u32 == kStatusNoMoreFiles &&
               PPC_LOAD_U32(kDirectoryIoStatus) == kStatusNoMoreFiles &&
               PPC_LOAD_U32(kDirectoryIoStatus + 4) == 0,
               "NtQueryDirectoryFile did not report a clean end of enumeration")) return 1;
    // A null-name continuation must retain the wildcard established by the
    // preceding query. This is the exact protocol used by the title's
    // Registry\MultiplayerMaps* search; losing the rule leaks unrelated .xcr
    // files into the result set.
    constexpr char kRestrictedPattern[] = "runtime_import_test.exe";
    std::memcpy(base + kDirectoryName, kRestrictedPattern,
                sizeof(kRestrictedPattern) - 1);
    PPC_STORE_U16(kDirectoryAnsiString + 0, sizeof(kRestrictedPattern) - 1);
    PPC_STORE_U16(kDirectoryAnsiString + 2, sizeof(kRestrictedPattern) - 1);
    PPC_STORE_U32(kDirectoryAnsiString + 4, kDirectoryName);
    context = {};
    context.r3.u64 = directoryHandle;
    context.r7.u64 = kDirectoryIoStatus;
    context.r8.u64 = kVolumeOutput;
    context.r9.u64 = 128;
    context.r10.u64 = kDirectoryAnsiString;
    context.r1.u64 = 0x100;
    PPC_STORE_U32(context.r1.u32 + 0x54, 0);
    __imp__NtQueryDirectoryFile(context, base);
    if (!Check(context.r3.u32 == 0,
               "NtQueryDirectoryFile restricted search did not find its exact child")) return 1;
    context = {};
    context.r3.u64 = directoryHandle;
    context.r7.u64 = kDirectoryIoStatus;
    context.r8.u64 = kVolumeOutput;
    context.r9.u64 = 128;
    context.r10.u64 = 0;
    context.r1.u64 = 0x100;
    PPC_STORE_U32(context.r1.u32 + 0x54, 0);
    __imp__NtQueryDirectoryFile(context, base);
    if (!Check(context.r3.u32 == kStatusNoMoreFiles &&
               PPC_LOAD_U32(kDirectoryIoStatus) == kStatusNoMoreFiles &&
               PPC_LOAD_U32(kDirectoryIoStatus + 4) == 0,
               "NtQueryDirectoryFile continuation forgot the established wildcard")) return 1;
    if (!Check(GetGuestFileSystem().Close(directoryHandle), "opened directory handle could not be closed")) return 1;
    // The title opens XDF packages with FILE_NON_DIRECTORY_FILE |
    // FILE_NO_INTERMEDIATE_BUFFERING (0x48), without either synchronous-I/O
    // flag. The host read may complete immediately and populate the IOSB, but
    // the API return must remain STATUS_PENDING. A synchronous handle returns
    // the completion status directly.
    constexpr char kRuntimeFilePath[] = "runtime_import_test.exe";
    std::memcpy(base + kFileName, kRuntimeFilePath, sizeof(kRuntimeFilePath) - 1);
    PPC_STORE_U32(kFileAttributes + 0, 0xFFFFFFFDu);
    PPC_STORE_U32(kFileAttributes + 4, kFileAnsiString);
    PPC_STORE_U32(kFileAttributes + 8, 0x40);
    PPC_STORE_U16(kFileAnsiString + 0, sizeof(kRuntimeFilePath) - 1);
    PPC_STORE_U16(kFileAnsiString + 2, sizeof(kRuntimeFilePath));
    PPC_STORE_U32(kFileAnsiString + 4, kFileName);
    PPC_STORE_U64(0x350, 0);
    auto OpenRuntimeFile = [&](uint32_t options) {
        context = {};
        context.r1.u64 = 0x100;
        context.r3.u64 = kFileHandleOut;
        context.r4.u64 = 0x80100080;
        context.r5.u64 = kFileAttributes;
        context.r6.u64 = kFileIoStatus;
        context.r7.u64 = 0;
        context.r8.u64 = 0;
        context.r9.u64 = 1;
        context.r10.u64 = 1;
        PPC_STORE_U32(context.r1.u32 + 0x54, options);
        __imp__NtCreateFile(context, base);
        return PPC_LOAD_U32(kFileHandleOut);
    };
    auto ReadRuntimeFile = [&](uint32_t handle) {
        std::memset(base + kFileBuffer, 0, 16);
        context = {};
        context.r3.u64 = handle;
        context.r7.u64 = kFileIoStatus;
        context.r8.u64 = kFileBuffer;
        context.r9.u64 = 16;
        context.r10.u64 = 0x350;
        __imp__NtReadFile(context, base);
    };
    uint32_t runtimeFileHandle = OpenRuntimeFile(0x48);
    if (!Check(context.r3.u32 == 0 && runtimeFileHandle != 0,
               "asynchronous runtime test file did not open")) return 1;
    ReadRuntimeFile(runtimeFileHandle);
    if (!Check(context.r3.u32 == kStatusPending && PPC_LOAD_U32(kFileIoStatus) == 0 &&
                   PPC_LOAD_U32(kFileIoStatus + 4) == 16 && base[kFileBuffer] == 'M' &&
                   base[kFileBuffer + 1] == 'Z',
               "immediate asynchronous NtReadFile did not return STATUS_PENDING with a completed IOSB")) return 1;
    if (!Check(GetGuestFileSystem().Close(runtimeFileHandle),
               "asynchronous runtime test file handle could not be closed")) return 1;
    runtimeFileHandle = OpenRuntimeFile(0x60);
    if (!Check(context.r3.u32 == 0 && runtimeFileHandle != 0,
               "synchronous runtime test file did not open")) return 1;
    ReadRuntimeFile(runtimeFileHandle);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kFileIoStatus) == 0 &&
                   PPC_LOAD_U32(kFileIoStatus + 4) == 16,
               "synchronous NtReadFile did not return its immediate completion status")) return 1;
    if (!Check(GetGuestFileSystem().Close(runtimeFileHandle),
               "synchronous runtime test file handle could not be closed")) return 1;
    std::memcpy(base + 0x3A0, "abc", 3);
    context = {};
    context.r3.u64 = 0x3A0;
    context.r4.u64 = 3;
    context.r9.u64 = 0x3C0;
    context.r10.u64 = 20;
    __imp__XeCryptSha(context, base);
    constexpr uint8_t kSha1Abc[] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
                                     0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
    if (!Check(std::memcmp(base + 0x3C0, kSha1Abc, sizeof(kSha1Abc)) == 0,
               "XeCryptSha did not produce the SHA-1 digest for abc")) return 1;
    PPC_STORE_U32(kStatsAddress, kStatsSize);
    if (!Query(context, base) ||
        !Check(PPC_LOAD_U32(kStatsAddress + 12) == 0x1000, "large physical allocation was not reflected in availability")) return 1;
    context.r4.u64 = 0xA0000000;
    __imp__MmFreePhysicalMemory(context, base);
    PPC_STORE_U32(kStatsAddress, kStatsSize);
    if (!Query(context, base) ||
        !Check(PPC_LOAD_U32(kStatsAddress + 12) == 0x20000, "MmFreePhysicalMemory did not restore availability")) return 1;

    context = {};
    context.r4.u64 = 0x1000;
    context.r5.u64 = 0;
    context.r6.u64 = 0;
    context.r7.u64 = 0xFFFFFFFF;
    __imp__MmAllocatePhysicalMemoryEx(context, base);
    if (!Check(context.r3.u32 == 0, "invalid physical-allocation protection was accepted")) return 1;

    auto& tls = GetGuestTls();
    tls.ResetForTests();
    context = {};
    __imp__KeTlsAlloc(context, base);
    const uint32_t tlsSlot = context.r3.u32;
    if (!Check(tlsSlot == 0, "first TLS slot is not zero")) return 1;
    context.r3.u64 = tlsSlot;
    __imp__KeTlsGetValue(context, base);
    if (!Check(context.r3.u32 == 0, "new TLS slot is not initialized to zero")) return 1;
    context.r3.u64 = tlsSlot;
    context.r4.u64 = 0x12345678;
    __imp__KeTlsSetValue(context, base);
    if (!Check(context.r3.u32 == 1, "KeTlsSetValue failed")) return 1;
    context.r3.u64 = tlsSlot;
    __imp__KeTlsGetValue(context, base);
    if (!Check(context.r3.u32 == 0x12345678, "TLS value was not preserved")) return 1;
    context.r3.u64 = tlsSlot;
    __imp__KeTlsFree(context, base);
    if (!Check(context.r3.u32 == 1, "KeTlsFree failed")) return 1;
    context.r3.u64 = tlsSlot;
    __imp__KeTlsGetValue(context, base);
    if (!Check(context.r3.u32 == 0, "freed TLS slot returned a value")) return 1;

    GetGuestObjects().ResetForTests();
    context = {};
    context.r3.u64 = 0x200;
    context.r4.u64 = 0;
    context.r5.u64 = 1;
    context.r6.u64 = 0;
    __imp__NtCreateEvent(context, base);
    if (!Check(context.r3.u32 == 0, "NtCreateEvent failed")) return 1;
    const uint32_t eventHandle = PPC_LOAD_U32(0x200);
    if (!Check(eventHandle != 0, "NtCreateEvent returned an invalid handle")) return 1;
    context.r3.u64 = eventHandle;
    context.r4.u64 = 0x204;
    __imp__NtSetEvent(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x204) == 0, "NtSetEvent did not report initial clear state")) return 1;
    context.r3.u64 = eventHandle;
    context.r4.u64 = 0x208;
    __imp__NtSetEvent(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x208) == 1, "NtSetEvent did not report previous signal state")) return 1;
    context.r3.u64 = eventHandle;
    __imp__NtClearEvent(context, base);
    if (!Check(context.r3.u32 == 0, "NtClearEvent failed")) return 1;
    context.r3.u64 = eventHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0, "NtClose did not release event handle")) return 1;
    context.r3.u64 = eventHandle;
    __imp__NtClearEvent(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle, "closed event was accepted")) return 1;

    // NtResumeThread is handle-based and must reject non-thread or stale
    // handles while still zeroing the optional previous-count output, as the
    // pinned kernel implementation does before attempting object lookup.
    const auto resumeWrongType = GetGuestObjects().CreateEvent(false, false);
    PPC_STORE_U32(0x204, 0xFFFFFFFFu);
    context = {};
    context.r3.u64 = resumeWrongType.handle;
    context.r4.u64 = 0x204;
    __imp__NtResumeThread(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle && PPC_LOAD_U32(0x204) == 0,
               "NtResumeThread accepted a non-thread handle or did not clear its count output"))
        return 1;
    context = {};
    context.r3.u64 = 0xDEADBEEFu;
    context.r4.u64 = 0;
    __imp__NtResumeThread(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle,
               "NtResumeThread accepted an unknown handle")) return 1;

    PPC_STORE_U32(0x208, 0xFFFFFFFFu);
    context = {};
    context.r3.u64 = resumeWrongType.handle;
    context.r4.u64 = 0x208;
    __imp__NtSuspendThread(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle && PPC_LOAD_U32(0x208) == 0,
               "NtSuspendThread accepted a non-thread handle or did not clear its count output"))
        return 1;

    context = {};
    context.r3.u64 = 0x20C;
    context.r4.u64 = 0;
    context.r5.u64 = 0;
    context.r6.u64 = 0x100000;
    __imp__NtCreateSemaphore(context, base);
    const uint32_t semaphoreHandle = PPC_LOAD_U32(0x20C);
    if (!Check(context.r3.u32 == 0 && semaphoreHandle != 0,
               "NtCreateSemaphore did not create a handle")) return 1;
    context.r3.u64 = semaphoreHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0, "NtClose did not release semaphore handle")) return 1;

    GetGuestObjects().ResetForTests();
    const auto threadOne = GetGuestObjects().CreateThread(1, 0x70000000, 0x20000);
    const auto threadTwo = GetGuestObjects().CreateThread(2, 0x6FFE0000, 0x20000);
    const auto threadClosing = GetGuestObjects().CreateThread(3, 0x6FFC0000, 0x20000);
    context = {};
    context.r3.u64 = threadOne.handle;
    context.r4.u64 = GuestObjects::kThreadObjectTypeToken;
    context.r5.u64 = 0x220;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x220) == threadOne.guestObject,
               "ObReferenceObjectByHandle did not return the thread guest object")) return 1;
    GuestObjectInfo objectInfo{};
    if (!Check(GetGuestObjects().GetObjectInfo(threadOne.handle, &objectInfo) && objectInfo.referenceCount == 1,
               "thread reference count did not increment")) return 1;
    context = {};
    context.r3.u64 = threadOne.guestObject;
    context.r4.u64 = 0x22C;
    __imp__ObOpenObjectByPointer(context, base);
    const uint32_t duplicateHandle = PPC_LOAD_U32(0x22C);
    if (!Check(context.r3.u32 == 0 && duplicateHandle != threadOne.handle,
               "ObOpenObjectByPointer did not allocate an independent handle")) return 1;
    if (!Check(GetGuestObjects().GetObjectInfo(duplicateHandle, &objectInfo) &&
               objectInfo.handle == duplicateHandle && objectInfo.handleCount == 2 &&
               objectInfo.referenceCount == 1,
               "new handle did not retain independent handle lifetime state")) return 1;
    context = {};
    context.r3.u64 = duplicateHandle;
    context.r4.u64 = GuestObjects::kThreadObjectTypeToken;
    context.r5.u64 = 0x230;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x230) == threadOne.guestObject,
               "duplicate handle did not resolve to the original object")) return 1;
    context.r3.u64 = threadOne.guestObject;
    __imp__ObDereferenceObject(context, base);
    context.r3.u64 = threadOne.handle;
    __imp__NtClose(context, base);
    if (!Check(GetGuestObjects().GetObjectInfo(duplicateHandle, &objectInfo) &&
               objectInfo.handleCount == 1 && objectInfo.referenceCount == 1,
               "closing original handle corrupted duplicate-handle lifetime")) return 1;
    context.r3.u64 = duplicateHandle;
    context.r4.u64 = GuestObjects::kThreadObjectTypeToken;
    context.r5.u64 = 0x234;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == 0, "duplicate handle was invalidated by closing original handle")) return 1;
    context.r3.u64 = threadOne.guestObject;
    __imp__ObDereferenceObject(context, base);
    context = {};
    context.r3.u64 = threadOne.guestObject;
    context.r4.u64 = 0x23C;
    __imp__ObOpenObjectByPointer(context, base);
    const uint32_t secondDuplicateHandle = PPC_LOAD_U32(0x23C);
    if (!Check(context.r3.u32 == 0 && secondDuplicateHandle != duplicateHandle &&
               GetGuestObjects().GetObjectInfo(secondDuplicateHandle, &objectInfo) &&
               objectInfo.handleCount == 2,
               "repeated object-pointer opens did not create a second live handle")) return 1;
    context.r3.u64 = duplicateHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0 && GetGuestObjects().GetObjectInfo(secondDuplicateHandle, &objectInfo) &&
               objectInfo.handleCount == 1,
               "closing one duplicate handle invalidated another")) return 1;
    context = {};
    context.r3.u64 = 0x7D00DEAD;
    context.r4.u64 = 0x240;
    PPC_STORE_U32(0x240, 0xFFFFFFFFu);
    __imp__ObOpenObjectByPointer(context, base);
    if (!Check(context.r3.u32 == 0xC0000001 && PPC_LOAD_U32(0x240) == 0xFFFFFFFFu,
               "invalid guest object pointer was accepted or wrote an output handle")) return 1;
    const auto staleThread = GetGuestObjects().CreateThread(4, 0x6FFA0000, 0x20000);
    context.r3.u64 = staleThread.handle;
    __imp__NtClose(context, base);
    context = {};
    context.r3.u64 = staleThread.guestObject;
    context.r4.u64 = 0x244;
    __imp__ObOpenObjectByPointer(context, base);
    if (!Check(context.r3.u32 == 0xC0000001,
               "stale guest object pointer was accepted")) return 1;
    SetCurrentGuestThreadForTests(threadOne.guestObject, 1);
    context.r3.u64 = 0xFFFFFFFEu;
    context.r4.u64 = GuestObjects::kThreadObjectTypeToken;
    context.r5.u64 = 0x228;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x228) == threadOne.guestObject,
               "current-thread pseudo-handle did not resolve to the caller thread")) return 1;
    context.r3.u64 = threadOne.guestObject;
    __imp__ObDereferenceObject(context, base);
    context.r3.u64 = secondDuplicateHandle;
    context.r4.u64 = 0xDEADBEEF;
    context.r5.u64 = 0x224;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == 0xC0000024, "wrong object type was accepted")) return 1;
    context.r3.u64 = threadClosing.handle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0, "thread handle close failed")) return 1;
    context.r3.u64 = threadClosing.handle;
    context.r4.u64 = GuestObjects::kThreadObjectTypeToken;
    __imp__ObReferenceObjectByHandle(context, base);
    if (!Check(context.r3.u32 == kStatusInvalidHandle, "closed thread handle was accepted")) return 1;

    PPC_STORE_U64(0x230, static_cast<uint64_t>(-10'000));
    context = {};
    context.r3.u64 = 1;
    context.r4.u64 = 0;
    context.r5.u64 = 0x230;
    __imp__KeDelayExecutionThread(context, base);
    if (!Check(context.r3.u32 == 0, "KeDelayExecutionThread did not complete a relative wait")) return 1;

    auto& criticalSections = GetGuestCriticalSections();
    criticalSections.ResetForTests();
    constexpr uint32_t kCriticalSectionAddress = 0x240;
    context = {};
    context.r3.u64 = kCriticalSectionAddress;
    __imp__RtlInitializeCriticalSection(context, base);
    SetCurrentGuestThreadForTests(threadOne.guestObject, 1);
    context.r3.u64 = kCriticalSectionAddress;
    __imp__RtlTryEnterCriticalSection(context, base);
    if (!Check(context.r3.u32 == 1, "unowned critical-section try-enter failed")) return 1;
    context.r3.u64 = kCriticalSectionAddress;
    __imp__RtlTryEnterCriticalSection(context, base);
    if (!Check(context.r3.u32 == 1, "recursive critical-section try-enter failed")) return 1;
    if (!Check(PPC_LOAD_U32(kCriticalSectionAddress + 20) == 2 &&
               PPC_LOAD_U32(kCriticalSectionAddress + 24) == threadOne.guestObject,
               "recursive critical-section ownership was not recorded")) return 1;
    SetCurrentGuestThreadForTests(threadTwo.guestObject, 2);
    context.r3.u64 = kCriticalSectionAddress;
    __imp__RtlTryEnterCriticalSection(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kCriticalSectionAddress + 20) == 2 &&
               PPC_LOAD_U32(kCriticalSectionAddress + 24) == threadOne.guestObject,
               "contended critical-section try-enter changed ownership")) return 1;
    SetCurrentGuestThreadForTests(threadOne.guestObject, 1);
    std::atomic<bool> workerEntered{false};
    std::thread contender([&] {
        SetCurrentGuestThreadForTests(threadTwo.guestObject, 2);
        PPCContext worker{};
        worker.r3.u64 = kCriticalSectionAddress;
        __imp__RtlEnterCriticalSection(worker, base);
        workerEntered.store(true);
        __imp__RtlLeaveCriticalSection(worker, base);
    });
    for (int spin = 0; spin < 10000 && !workerEntered.load(); ++spin) std::this_thread::yield();
    if (!Check(!workerEntered.load(), "contended critical section did not block")) return 1;
    context.r3.u64 = kCriticalSectionAddress;
    __imp__RtlLeaveCriticalSection(context, base);
    __imp__RtlLeaveCriticalSection(context, base);
    contender.join();
    if (!Check(workerEntered.load() && PPC_LOAD_U32(kCriticalSectionAddress + 20) == 0 &&
               PPC_LOAD_U32(kCriticalSectionAddress + 24) == 0,
               "critical-section leave did not wake and release")) return 1;
    context = {};
    context.r3.u64 = 1;
    __imp__KeEnableFpuExceptions(context, base);
    GuestThreadInfo threadInfo{};
    if (!Check(GetGuestObjects().GetThreadInfo(threadOne.guestObject, &threadInfo) &&
               threadInfo.fpuExceptionsEnabled, "thread FPU exception mode was not retained")) return 1;
    context.r3.u64 = 0;
    __imp__KeEnableFpuExceptions(context, base);
    if (!Check(GetGuestObjects().GetThreadInfo(threadOne.guestObject, &threadInfo) &&
               !threadInfo.fpuExceptionsEnabled, "thread FPU exception mode was not cleared")) return 1;
    const auto readyEvent = GetGuestObjects().CreateEvent(false, true);
    context = {};
    context.r3.u64 = readyEvent.handle;
    context.r4.u64 = 1;
    __imp__NtWaitForSingleObjectEx(context, base);
    if (!Check(context.r3.u32 == 0, "signalled event did not satisfy single-object wait")) return 1;
    const auto emptySemaphore = GetGuestObjects().CreateSemaphore(0, 1);
    PPC_STORE_U64(0x238, 0);
    context = {};
    context.r3.u64 = emptySemaphore.handle;
    context.r4.u64 = 1;
    context.r5.u64 = 0;
    context.r6.u64 = 0x238;
    __imp__NtWaitForSingleObjectEx(context, base);
    if (!Check(context.r3.u32 == 0x102, "zero-time semaphore wait did not time out")) return 1;
    context = {};
    context.r3.u64 = emptySemaphore.handle;
    context.r4.u64 = 1;
    context.r5.u64 = 0x23C;
    __imp__NtReleaseSemaphore(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(0x23C) == 0,
               "semaphore release did not return its previous count")) return 1;
    context = {};
    context.r3.u64 = emptySemaphore.handle;
    context.r4.u64 = 1;
    __imp__NtWaitForSingleObjectEx(context, base);
    if (!Check(context.r3.u32 == 0, "released semaphore did not wake single-object wait")) return 1;
    context.r3.u64 = secondDuplicateHandle;
    __imp__NtClose(context, base);
    if (!Check(context.r3.u32 == 0, "closing final duplicate handle failed")) return 1;
    context.r3.u64 = threadOne.guestObject;
    __imp__ObDereferenceObject(context, base);
    context = {};
    context.r3.u64 = threadOne.guestObject;
    context.r4.u64 = 0x248;
    __imp__ObOpenObjectByPointer(context, base);
    if (!Check(context.r3.u32 == 0xC0000001,
               "last-handle close plus final dereference did not retire object")) return 1;

    // The live title polls the referenced thread object's +0x04 state byte and
    // +0x140 exit code. Exercise the complete ExTerminateThread publication
    // path at a test-local guest address that fits this bounded memory image.
    GetGuestObjects().ResetForTests(0x100);
    const auto exitingThread = GetGuestObjects().CreateThread(15, 0x6FF80000, 0x20000);
    SetCurrentGuestThreadForTests(exitingThread.guestObject, 15);
    context = {};
    context.r3.u64 = 0x12345678;
    bool sawThreadExit = false;
    try {
        __imp__ExTerminateThread(context, base);
    } catch (const GuestThreadExit&) {
        sawThreadExit = true;
    }
    GuestThreadInfo exitedInfo{};
    if (!Check(sawThreadExit &&
                   (PPC_LOAD_U32(exitingThread.guestObject + 4) & 0xFF) == 1 &&
                   PPC_LOAD_U32(exitingThread.guestObject + 320) == 0x12345678 &&
                   GetGuestObjects().GetThreadInfo(exitingThread.guestObject, &exitedInfo) &&
                   exitedInfo.state == GuestThreadState::Terminated &&
                   exitedInfo.exitCode == 0x12345678,
               "thread termination was not published to guest and host object state")) return 1;

    // Ke events are 16-byte dispatcher headers embedded in title-owned
    // objects. Synchronization events consume one signal; notification events
    // remain signalled until reset. Exercise the pointer-based ABI separately
    // from the handle-based Nt event tests above.
    ResetGuestRuntimeStop();
    GetGuestObjects().ResetForTests(0x100);
    GetGuestDispatcherEvents().ResetForTests();
    const auto dispatcherThread = GetGuestObjects().CreateThread(16, 0x6FF40000, 0x20000);
    SetCurrentGuestThreadForTests(dispatcherThread.guestObject, 16);
    constexpr uint32_t kDispatcherEvent = 0x300;
    constexpr uint32_t kDispatcherTimeout = 0x3F0;
    GetGuestDispatcherEvents().Initialize(base, kDispatcherEvent, 1, false);
    context = {};
    context.r3.u64 = kDispatcherEvent;
    context.r4.u64 = 1;
    context.r5.u64 = 0;
    __imp__KeSetEvent(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDispatcherEvent + 4) == 1,
               "KeSetEvent did not publish a synchronization-event signal")) return 1;
    context = {};
    context.r3.u64 = kDispatcherEvent;
    context.r4.u64 = 3;
    context.r5.u64 = 1;
    context.r6.u64 = 0;
    context.r7.u64 = 0;
    __imp__KeWaitForSingleObject(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDispatcherEvent + 4) == 0,
               "synchronization-event wait did not consume exactly one signal")) return 1;
    PPC_STORE_U64(kDispatcherTimeout, 0);
    context = {};
    context.r3.u64 = kDispatcherEvent;
    context.r4.u64 = 3;
    context.r5.u64 = 1;
    context.r7.u64 = kDispatcherTimeout;
    __imp__KeWaitForSingleObject(context, base);
    if (!Check(context.r3.u32 == 0x102,
               "unsignalled dispatcher event did not honor an immediate timeout")) return 1;
    context = {};
    context.r3.u64 = kDispatcherEvent;
    context.r4.u64 = 1;
    __imp__KeSetEvent(context, base);
    context = {};
    context.r3.u64 = kDispatcherEvent;
    __imp__KeResetEvent(context, base);
    if (!Check(context.r3.u32 == 1 && PPC_LOAD_U32(kDispatcherEvent + 4) == 0,
               "KeResetEvent did not return and clear the prior signal state")) return 1;

    GetGuestDispatcherEvents().Initialize(base, kDispatcherEvent, 0, true);
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        context = {};
        context.r3.u64 = kDispatcherEvent;
        context.r4.u64 = 3;
        context.r5.u64 = 1;
        __imp__KeWaitForSingleObject(context, base);
        if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDispatcherEvent + 4) == 1,
                   "notification-event wait incorrectly consumed its signal")) return 1;
    }

    constexpr uint32_t kSecondDispatcherEvent = 0x340;
    constexpr uint32_t kDispatcherObjectArray = 0x380;
    GetGuestDispatcherEvents().Initialize(base, kDispatcherEvent, 1, false);
    GetGuestDispatcherEvents().Initialize(base, kSecondDispatcherEvent, 1, true);
    PPC_STORE_U32(kDispatcherObjectArray, kDispatcherEvent);
    PPC_STORE_U32(kDispatcherObjectArray + 4, kSecondDispatcherEvent);
    context = {};
    context.r3.u64 = 2;
    context.r4.u64 = kDispatcherObjectArray;
    context.r5.u64 = 1;  // WaitAny.
    context.r6.u64 = 3;
    context.r7.u64 = 1;
    __imp__KeWaitForMultipleObjects(context, base);
    if (!Check(context.r3.u32 == 1 && PPC_LOAD_U32(kDispatcherEvent + 4) == 0 &&
                   PPC_LOAD_U32(kSecondDispatcherEvent + 4) == 0,
               "KeWaitForMultipleObjects did not return and consume the signalled WaitAny index"))
        return 1;

    GetGuestDispatcherSemaphores().ResetForTests();
    constexpr uint32_t kDispatcherSemaphore = 0x320;
    context = {};
    context.r3.u64 = kDispatcherSemaphore;
    context.r4.u64 = 0;
    context.r5.u64 = 6;
    __imp__KeInitializeSemaphore(context, base);
    if (!Check(PPC_LOAD_U8(kDispatcherSemaphore) == 5 &&
                   PPC_LOAD_U32(kDispatcherSemaphore + 4) == 0 &&
                   PPC_LOAD_U32(kDispatcherSemaphore + 16) == 6,
               "KeInitializeSemaphore did not publish the X_KSEMAPHORE layout")) return 1;
    context = {};
    context.r3.u64 = kDispatcherSemaphore;
    context.r4.u64 = 1;
    context.r5.u64 = 2;
    context.r6.u64 = 0;
    __imp__KeReleaseSemaphore(context, base);
    if (!Check(context.r3.u32 == 0 && PPC_LOAD_U32(kDispatcherSemaphore + 4) == 2,
               "KeReleaseSemaphore did not return and publish the previous count")) return 1;
    for (uint32_t expectedCount : {1u, 0u}) {
        context = {};
        context.r3.u64 = kDispatcherSemaphore;
        context.r4.u64 = 3;
        context.r5.u64 = 1;
        __imp__KeWaitForSingleObject(context, base);
        if (!Check(context.r3.u32 == 0 &&
                       PPC_LOAD_U32(kDispatcherSemaphore + 4) == expectedCount,
                   "dispatcher semaphore wait did not consume exactly one count")) return 1;
    }
    PPC_STORE_U64(kDispatcherTimeout, 0);
    context = {};
    context.r3.u64 = kDispatcherSemaphore;
    context.r4.u64 = 3;
    context.r5.u64 = 1;
    context.r7.u64 = kDispatcherTimeout;
    __imp__KeWaitForSingleObject(context, base);
    if (!Check(context.r3.u32 == 0x102,
               "empty dispatcher semaphore did not honor an immediate timeout")) return 1;

    // A runtime blocker must not strand host workers in infinite guest waits.
    // Requesting the teardown stop wakes every object waiter and produces a
    // private host-control exception, without inventing a guest event signal.
    ResetGuestRuntimeStop();
    GetGuestObjects().ResetForTests(0x100);
    const auto teardownEvent = GetGuestObjects().CreateEvent(false, false);
    std::atomic<bool> teardownWaitEntered{false};
    std::atomic<bool> teardownWaitStopped{false};
    std::thread teardownWaiter([&] {
        teardownWaitEntered.store(true, std::memory_order_release);
        try {
            GetGuestObjects().WaitForSingleObject(teardownEvent.handle, false, 0);
        } catch (const GuestRuntimeStop&) {
            teardownWaitStopped.store(true, std::memory_order_release);
        }
    });
    while (!teardownWaitEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    RequestGuestRuntimeStop();
    teardownWaiter.join();
    if (!Check(teardownWaitStopped.load(std::memory_order_acquire),
               "runtime stop did not wake an infinite guest object wait")) return 1;
    ResetGuestRuntimeStop();

    GetGuestDispatcherEvents().ResetForTests();
    GetGuestDispatcherEvents().Initialize(base, kDispatcherEvent, 1, false);
    std::atomic<bool> dispatcherWaitEntered{false};
    std::atomic<bool> dispatcherWaitStopped{false};
    std::thread dispatcherWaiter([&] {
        dispatcherWaitEntered.store(true, std::memory_order_release);
        try {
            GetGuestDispatcherEvents().Wait(base, kDispatcherEvent, false, 0);
        } catch (const GuestRuntimeStop&) {
            dispatcherWaitStopped.store(true, std::memory_order_release);
        }
    });
    while (!dispatcherWaitEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    RequestGuestRuntimeStop();
    dispatcherWaiter.join();
    if (!Check(dispatcherWaitStopped.load(std::memory_order_acquire) &&
                   PPC_LOAD_U32(kDispatcherEvent + 4) == 0,
               "runtime stop did not wake an embedded event without signalling it")) return 1;
    ResetGuestRuntimeStop();

    // db16cyc is architecturally a delay hint, but it must also be a host
    // lifecycle cancellation point when guest code uses it in an endless
    // polling loop. No guest register or memory state is synthesized.
    RuntimeDb16Cyc();
    RequestGuestRuntimeStop();
    bool db16cycStopped = false;
    try {
        RuntimeDb16Cyc();
    } catch (const GuestRuntimeStop&) {
        db16cycStopped = true;
    }
    if (!Check(db16cycStopped,
               "db16cyc did not observe the runtime teardown request")) return 1;
    ResetGuestRuntimeStop();

    // V433 kernel timers (the voice engine's 10 ms worker timer) and handle
    // duplication.
    {
        auto& objects = GetGuestObjects();
        const uint32_t oneShot = objects.CreateTimer(false).handle;
        bool previous = true;
        if (!Check(objects.WaitForSingleObject(oneShot, true, 0) == 0x102,
                   "an unset timer was signalled")) return 1;
        if (!Check(objects.SetTimer(oneShot, -100000, 0, &previous) == 0 && !previous,
                   "NtSetTimerEx did not arm a relative one-shot timer")) return 1;
        if (!Check(objects.WaitForSingleObject(oneShot, true, 0) == 0x102,
                   "a timer fired before its due time")) return 1;
        const auto waitStart = std::chrono::steady_clock::now();
        if (!Check(objects.WaitForSingleObject(oneShot, false, 0) == 0 &&
                       std::chrono::steady_clock::now() - waitStart >= std::chrono::milliseconds(5),
                   "an infinite wait did not end at the timer's due time")) return 1;
        if (!Check(objects.WaitForSingleObject(oneShot, true, 0) == 0x102,
                   "a synchronization timer stayed signalled after releasing a waiter")) return 1;
        const uint32_t periodic = objects.CreateTimer(false).handle;
        if (!Check(objects.SetTimer(periodic, -10000, 20, nullptr) == 0 &&
                       objects.WaitForSingleObject(periodic, true, -1000000) == 0 &&
                       objects.WaitForSingleObject(periodic, true, -1000000) == 0,
                   "a periodic timer did not expire twice")) return 1;
        bool current = true;
        if (!Check(objects.CancelTimer(periodic, &current) == 0 &&
                       objects.WaitForSingleObject(periodic, true, -200000) == 0x102,
                   "a cancelled timer still expired")) return 1;
        const uint32_t notification = objects.CreateTimer(true).handle;
        if (!Check(objects.SetTimer(notification, 0, 0, nullptr) == 0 &&
                       objects.WaitForSingleObject(notification, true, 0) == 0 &&
                       objects.WaitForSingleObject(notification, true, 0) == 0,
                   "a notification timer did not stay signalled")) return 1;
        const uint32_t event = objects.CreateEvent(true, false).handle;
        uint32_t duplicate = 0;
        if (!Check(objects.DuplicateHandle(event, 0, false, &duplicate) == 0 && duplicate &&
                       duplicate != event && objects.SetEvent(duplicate, nullptr) &&
                       objects.WaitForSingleObject(event, true, 0) == 0,
                   "a duplicated handle does not reach the same event")) return 1;
        uint32_t moved = 0;
        if (!Check(objects.Close(event) && objects.WaitForSingleObject(duplicate, true, 0) == 0 &&
                       objects.DuplicateHandle(duplicate, 0, true, &moved) == 0 &&
                       !objects.Close(duplicate) && objects.Close(moved),
                   "handle duplication with DUPLICATE_CLOSE_SOURCE misbehaved")) return 1;
        uint32_t none = 0;
        if (!Check(objects.DuplicateHandle(0xFFFFFFF0u, 0, false, &none) == kStatusInvalidHandle,
                   "an invalid handle was duplicated")) return 1;
        objects.Close(oneShot);
        objects.Close(periodic);
        objects.Close(notification);
    }

    // V433 offline network adapter: sockets bind locally, nothing arrives.
    {
        constexpr uint32_t kData = 0x9000, kAddress = 0x9200, kArgument = 0x9220,
                           kSet = 0x9300, kXnAddr = 0x9500, kXuid = 0x9540;
        PPCContext net{};
        net.r3.u64 = 1;
        net.r4.u64 = 0x0002;
        net.r5.u64 = kData;
        __imp__NetDll_WSAStartup(net, base);
        if (!Check(net.r3.u32 == 0 && PPC_LOAD_U16(kData) == 0x0002 &&
                       PPC_LOAD_U16(kData + 2) == 0x0202,
                   "WSAStartup did not report Winsock 2")) return 1;
        net.r3.u64 = 1;
        net.r4.u64 = 2;
        net.r5.u64 = 2;
        net.r6.u64 = 17;
        __imp__NetDll_socket(net, base);
        const uint32_t socket = net.r3.u32;
        if (!Check(socket && socket != 0xFFFFFFFFu, "a UDP socket did not open")) return 1;
        PPC_STORE_U16(kAddress + 0, 2);
        PPC_STORE_U16(kAddress + 2, 1000);
        PPC_STORE_U32(kAddress + 4, 0);
        net.r3.u64 = 1;
        net.r4.u64 = socket;
        net.r5.u64 = kAddress;
        net.r6.u64 = 16;
        __imp__NetDll_bind(net, base);
        if (!Check(net.r3.u32 == 0, "bind failed on the offline adapter")) return 1;
        PPC_STORE_U32(kArgument, 1);
        net.r3.u64 = 1;
        net.r4.u64 = socket;
        net.r5.u64 = 0x8004667Eu;
        net.r6.u64 = kArgument;
        __imp__NetDll_ioctlsocket(net, base);
        if (!Check(net.r3.u32 == 0, "FIONBIO failed")) return 1;
        net.r3.u64 = 1;
        net.r4.u64 = socket;
        net.r5.u64 = kData;
        net.r6.u64 = 64;
        net.r7.u64 = 0;
        net.r8.u64 = 0;
        net.r9.u64 = 0;
        __imp__NetDll_recvfrom(net, base);
        const uint32_t received = net.r3.u32;
        __imp__NetDll_WSAGetLastError(net, base);
        if (!Check(received == 0xFFFFFFFFu && net.r3.u32 == 10035,
                   "a non-blocking receive did not report WSAEWOULDBLOCK")) return 1;
        PPC_STORE_U32(kSet, 1);
        PPC_STORE_U32(kSet + 4, socket);
        PPC_STORE_U32(kSet + 0x104, 1);
        PPC_STORE_U32(kSet + 0x108, socket);
        PPC_STORE_U32(kArgument, 0);
        PPC_STORE_U32(kArgument + 4, 0);
        net.r3.u64 = 1;
        net.r4.u64 = 0;
        net.r5.u64 = kSet;
        net.r6.u64 = kSet + 0x104;
        net.r7.u64 = 0;
        net.r8.u64 = kArgument;
        __imp__NetDll_select(net, base);
        if (!Check(net.r3.u32 == 1 && PPC_LOAD_U32(kSet) == 0 &&
                       PPC_LOAD_U32(kSet + 0x104) == 1,
                   "select reported a readable socket or no writable one")) return 1;
        net.r3.u64 = 1;
        net.r4.u64 = kXnAddr;
        __imp__NetDll_XNetGetTitleXnAddr(net, base);
        if (!Check(net.r3.u32 == 2 && PPC_LOAD_U32(kXnAddr) == 0 &&
                       PPC_LOAD_U8(kXnAddr + 10) == 0x02,
                   "the offline adapter reported an IP address")) return 1;
        net.r3.u64 = 1;
        net.r4.u64 = socket;
        __imp__NetDll_closesocket(net, base);
        const uint32_t closed = net.r3.u32;
        net.r3.u64 = 1;
        net.r4.u64 = socket;
        __imp__NetDll_closesocket(net, base);
        const uint32_t closedTwice = net.r3.u32;
        __imp__NetDll_WSAGetLastError(net, base);
        if (!Check(closed == 0 && closedTwice == 0xFFFFFFFFu && net.r3.u32 == 10038,
                   "closesocket accepted a closed socket")) return 1;
        net.r3.u64 = 0;
        net.r4.u64 = 7;
        net.r5.u64 = kXuid;
        __imp__XamUserGetXUID(net, base);
        if (!Check(net.r3.u32 == 0 && PPC_LOAD_U64(kXuid) == 0xE000000000000001ull,
                   "the local profile has no offline XUID")) return 1;
        net.r3.u64 = 0xFC;
        net.r4.u64 = 0x58004;
        net.r5.u64 = kData;
        net.r6.u64 = 0;
        __imp__XMsgInProcessCall(net, base);
        if (!Check(net.r3.u32 == 0x80004005u, "an Xbox LIVE message succeeded offline")) return 1;
    }

    return 0;
}
