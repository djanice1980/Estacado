#pragma once

#include <cstddef>
#include <cstdint>

// Xbox 360 XAudio render clients submit one 256-sample, six-channel frame at
// 48 kHz. Samples are planar big-endian IEEE floats in FL, FR, FC, LFE, BL, BR
// order. The host adapter downmixes that verified contract to interleaved
// stereo PCM16 for the Windows wave output device.
constexpr size_t kRuntimeXAudioChannelSamples = 256;
constexpr size_t kRuntimeXAudioChannelCount = 6;
constexpr size_t kRuntimeXAudioFrameBytes =
    kRuntimeXAudioChannelSamples * kRuntimeXAudioChannelCount * sizeof(float);
constexpr size_t kRuntimeXAudioStereoSamples = kRuntimeXAudioChannelSamples * 2;
constexpr size_t kRuntimeXAudioHostSampleRate = 48000;
constexpr size_t kRuntimeXAudioHostChannelCount = 2;
constexpr size_t kRuntimeXAudioHostBitsPerSample = 16;
constexpr size_t kRuntimeXAudioHostBlockAlign =
    kRuntimeXAudioHostChannelCount * kRuntimeXAudioHostBitsPerSample / 8;
constexpr size_t kRuntimeXAudioHostFrameBytes =
    kRuntimeXAudioStereoSamples * sizeof(int16_t);
constexpr size_t kRuntimeXAudioHostFrameCount =
    kRuntimeXAudioHostFrameBytes / kRuntimeXAudioHostBlockAlign;
constexpr uint64_t kRuntimeXAudioHostFrameDurationUs =
    kRuntimeXAudioHostFrameCount * 1000000ull / kRuntimeXAudioHostSampleRate;
constexpr uint32_t kRuntimeAudioMasterVolumeOneQ16 = 65536u;
static_assert(kRuntimeXAudioHostFrameCount == kRuntimeXAudioChannelSamples);
static_assert(kRuntimeXAudioHostFrameBytes == 1024);
static_assert(kRuntimeXAudioHostFrameDurationUs == 5333);

void ConfigureRuntimeAudioDiagnostics(bool enabled) noexcept;
bool RuntimeAudioDiagnosticsEnabled() noexcept;
uint32_t RuntimeAudioDriverToken(size_t index) noexcept;
bool RuntimeAudioDecodeDriverToken(uint32_t token, size_t* index) noexcept;
uint32_t RuntimeAudioGetVoiceCategoryVolumeChangeMask(
    uint32_t driverToken, uint32_t* mask) noexcept;
void RuntimeAudioConvertFrameToStereoPcm16(const uint8_t* guestFrame,
                                           int16_t* stereoSamples);
// The optional PC master-volume control is applied only to the final,
// runtime-owned host PCM buffer. Q16 one is an exact fast-path identity; guest
// source data, title mixing, and callback/device timing remain unchanged.
int16_t ScaleRuntimeAudioHostSample(int16_t sample,
                                    uint32_t scaleQ16) noexcept;
void RuntimeAudioScaleHostPcm16(int16_t* samples, size_t sampleCount,
                                uint32_t scaleQ16) noexcept;
void ConfigureRuntimeAudioMasterVolume(double volume);
uint32_t RuntimeAudioMasterVolumeQ16() noexcept;
// Focused host-backend regression: proves a synchronous WOM_DONE-style
// completion can re-enter the runtime while a frame is being submitted.
// Production waveOut drivers are permitted to deliver callbacks from another
// thread before waveOutWrite returns, so the client mutex must not span that
// call.
bool RuntimeAudioRunSynchronousCompletionRegression();

uint32_t RuntimeAudioRegisterRenderDriverClient(uint8_t* base,
                                                uint32_t callbackPair,
                                                uint32_t driverOut);
uint32_t RuntimeAudioUnregisterRenderDriverClient(uint32_t driverToken);
uint32_t RuntimeAudioSubmitRenderDriverFrame(uint8_t* base,
                                             uint32_t driverToken,
                                             uint32_t samples);
bool RuntimeAudioInitializeXmaHardware(uint8_t* base);
uint32_t RuntimeAudioCreateXmaContext(uint8_t* base, uint32_t contextOut);
uint32_t RuntimeAudioReleaseXmaContext(uint32_t contextAddress);
void ShutdownRuntimeAudio() noexcept;
