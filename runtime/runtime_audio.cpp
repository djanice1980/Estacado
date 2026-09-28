#include "runtime_audio.h"

#include "runtime_guest_bulk_write.h"
#include "ppc_recomp_shared.h"
#include "runtime_function_trace.h"
#include "runtime_graphics.h"
#include "runtime_memory.h"
#include "runtime_threads.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kXErrorSuccess = 0x00000000;
constexpr uint32_t kXErrorInvalidArgument = 0x80070057;
constexpr uint32_t kXStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kXStatusNoMemory = 0xC0000017;
constexpr uint32_t kXStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kDriverTokenPrefix = 0x41550000;
constexpr size_t kMaximumClients = 8;
constexpr uint32_t kQueuedFrames = 8;
constexpr uint32_t kAudioWorkerStackSize = 128 * 1024;
// Capture exactly one second of the first audible render-client stream. The
// callback only copies into pre-reserved memory; file I/O happens on a detached
// host thread after the bounded capture is complete. This keeps the evidence
// useful for offline source-versus-host comparison without synchronously
// perturbing the 5.33 ms audio callback cadence.
constexpr size_t kDiagnosticAudioCaptureFrames =
    kRuntimeXAudioHostSampleRate / kRuntimeXAudioHostFrameCount;
// XMA context churn is normal during streaming. Its lifecycle semantics are
// regression-covered, and synchronous per-context output is too costly for a
// representative audio/pacing run.
constexpr bool kCompletedAudioLifecycleDiagnosticsEnabled = false;
std::atomic<bool> runtimeAudioDiagnosticsEnabled{};
std::atomic<uint32_t> runtimeAudioMasterVolumeQ16{
    kRuntimeAudioMasterVolumeOneQ16};
// Runtime-private guest virtual storage. GuestObjects is bounded below this
// page and PCR/TEB storage begins at 0x7D100000. The page is shared only with
// the immutable execution-info record at 0x7D0FFE00, leaving these eight words
// disjoint from title allocations, kernel objects, and thread state.
constexpr uint32_t kWrappedCallbackArgs = 0x7D0FF000;
constexpr uint32_t kPhysicalGuestBase = 0xA0000000;
constexpr uint32_t kXmaContextCount = 320;
constexpr uint32_t kXmaContextBytes = 64 * kXmaContextCount;
static_assert(kXmaContextBytes % GuestMemoryAccounting::kPageSize == 0);

float LoadBigEndianFloat(const uint8_t* bytes) {
    const uint32_t bits = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                          (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

int16_t FloatToPcm16(float value) {
    if (!std::isfinite(value)) value = 0.0f;
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(value * 32767.0f));
}

struct AudioBuffer {
    std::array<int16_t, kRuntimeXAudioStereoSamples> samples{};
    WAVEHDR header{};
    bool prepared{};
    bool queued{};
};

class RuntimeAudioSystem;

struct AudioClient {
    RuntimeAudioSystem* owner{};
    size_t index{};
    bool inUse{};
    uint32_t callback{};
    uint32_t callbackArg{};
    uint32_t wrappedCallbackArg{};
    uint32_t callbacksDue{};
    HWAVEOUT device{};
    uint32_t submissionsInFlight{};
    uint64_t deviceCompletionCount{};
    uint64_t deviceCompletionLastGapUs{};
    uint64_t deviceCompletionGapTotalUs{};
    uint64_t deviceCompletionMaximumGapUs{};
    std::chrono::steady_clock::time_point previousDeviceCompletion{};
    bool reportedFirstNonSilentPcm{};
    bool diagnosticCaptureActive{};
    bool diagnosticCaptureComplete{};
    uint64_t diagnosticCaptureFirstOrdinal{};
    std::vector<uint8_t> diagnosticGuestFrames;
    std::vector<int16_t> diagnosticHostSamples;
    std::array<AudioBuffer, kQueuedFrames> buffers{};
};

void WriteU16(std::ostream& stream, uint16_t value) {
    const std::array<uint8_t, 2> bytes = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void WriteU32(std::ostream& stream, uint32_t value) {
    const std::array<uint8_t, 4> bytes = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

uint32_t HashBytes(const uint8_t* bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

void WriteDiagnosticAudioCapture(std::vector<uint8_t> guestFrames,
                                 std::vector<int16_t> hostSamples,
                                 uint64_t firstOrdinal) {
    constexpr const char* kGuestPath =
        "rex_audio_first_non_silent_guest_be_f32.bin";
    constexpr const char* kHostPath =
        "rex_audio_first_non_silent_host_pcm16.wav";
    constexpr const char* kMetadataPath =
        "rex_audio_first_non_silent_capture.txt";

    bool guestWritten = false;
    {
        std::ofstream guestFile(kGuestPath, std::ios::binary | std::ios::trunc);
        if (guestFile) {
            guestFile.write(reinterpret_cast<const char*>(guestFrames.data()),
                            static_cast<std::streamsize>(guestFrames.size()));
            guestWritten = guestFile.good();
        }
    }

    bool hostWritten = false;
    {
        std::ofstream hostFile(kHostPath, std::ios::binary | std::ios::trunc);
        if (hostFile &&
            hostSamples.size() <= (UINT32_MAX - 36u) / sizeof(int16_t)) {
            const uint32_t dataBytes =
                static_cast<uint32_t>(hostSamples.size() * sizeof(int16_t));
            hostFile.write("RIFF", 4);
            WriteU32(hostFile, 36u + dataBytes);
            hostFile.write("WAVEfmt ", 8);
            WriteU32(hostFile, 16);
            WriteU16(hostFile, WAVE_FORMAT_PCM);
            WriteU16(hostFile,
                     static_cast<uint16_t>(kRuntimeXAudioHostChannelCount));
            WriteU32(hostFile,
                     static_cast<uint32_t>(kRuntimeXAudioHostSampleRate));
            WriteU32(hostFile,
                     static_cast<uint32_t>(kRuntimeXAudioHostSampleRate *
                                           kRuntimeXAudioHostBlockAlign));
            WriteU16(hostFile,
                     static_cast<uint16_t>(kRuntimeXAudioHostBlockAlign));
            WriteU16(hostFile,
                     static_cast<uint16_t>(kRuntimeXAudioHostBitsPerSample));
            hostFile.write("data", 4);
            WriteU32(hostFile, dataBytes);
            hostFile.write(reinterpret_cast<const char*>(hostSamples.data()),
                           dataBytes);
            hostWritten = hostFile.good();
        }
    }

    const uint32_t guestHash =
        HashBytes(guestFrames.data(), guestFrames.size());
    const uint32_t hostHash = HashBytes(
        reinterpret_cast<const uint8_t*>(hostSamples.data()),
        hostSamples.size() * sizeof(int16_t));
    {
        std::ofstream metadataFile(kMetadataPath, std::ios::trunc);
        if (metadataFile) {
            metadataFile
                << "first_submission_ordinal=" << firstOrdinal << '\n'
                << "frame_count=" << kDiagnosticAudioCaptureFrames << '\n'
                << "samples_per_channel=" << kRuntimeXAudioHostFrameCount
                << '\n'
                << "sample_rate=" << kRuntimeXAudioHostSampleRate << '\n'
                << "guest_channels=" << kRuntimeXAudioChannelCount << '\n'
                << "guest_format=planar_big_endian_float32\n"
                << "guest_bytes=" << guestFrames.size() << '\n'
                << "guest_fnv1a=0x" << std::hex << guestHash << std::dec
                << '\n'
                << "host_channels=" << kRuntimeXAudioHostChannelCount << '\n'
                << "host_format=interleaved_little_endian_pcm16\n"
                << "host_bytes=" << hostSamples.size() * sizeof(int16_t)
                << '\n'
                << "host_fnv1a=0x" << std::hex << hostHash << std::dec
                << '\n';
        }
    }

    std::cout << "XAUDIO_DIAGNOSTIC_CAPTURE_COMPLETE first_ordinal="
              << firstOrdinal << " frames=" << kDiagnosticAudioCaptureFrames
              << " guest_bytes=" << guestFrames.size()
              << " host_bytes=" << hostSamples.size() * sizeof(int16_t)
              << " guest_hash=0x" << std::hex << guestHash
              << " host_hash=0x" << hostHash << std::dec
              << " guest_written=" << guestWritten
              << " host_written=" << hostWritten << '\n';
}

class RuntimeAudioSystem {
public:
    uint32_t Register(uint8_t* base, uint32_t callbackPair, uint32_t driverOut) {
        if (!base || !callbackPair || (callbackPair & 3u) ||
            callbackPair > UINT32_MAX - 8u || !driverOut || (driverOut & 3u) ||
            driverOut > UINT32_MAX - 4u) {
            return kXErrorInvalidArgument;
        }
        const uint32_t callback = PPC_LOAD_U32(callbackPair);
        const uint32_t callbackArg = PPC_LOAD_U32(callbackPair + 4);
        if (!callback || !RuntimeGeneratedAddressInRange(callback)) {
            return kXErrorInvalidArgument;
        }

        size_t selected = kMaximumClients;
        bool startWorker = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shuttingDown_) return kXStatusUnsuccessful;
            for (size_t index = 0; index < clients_.size(); ++index) {
                if (!clients_[index].inUse && !clients_[index].device) {
                    selected = index;
                    break;
                }
            }
            if (selected == kMaximumClients) return kXStatusUnsuccessful;

            auto& client = clients_[selected];
            client.owner = this;
            client.index = selected;
            client.callback = callback;
            client.callbackArg = callbackArg;
            client.wrappedCallbackArg = kWrappedCallbackArgs + uint32_t(selected * 4);
            client.reportedFirstNonSilentPcm = false;
            client.diagnosticCaptureActive = false;
            client.diagnosticCaptureComplete = false;
            client.diagnosticCaptureFirstOrdinal = 0;
            client.diagnosticGuestFrames.clear();
            client.diagnosticHostSamples.clear();
            if (RuntimeAudioDiagnosticsEnabled()) {
                client.diagnosticGuestFrames.reserve(
                    kDiagnosticAudioCaptureFrames * kRuntimeXAudioFrameBytes);
                client.diagnosticHostSamples.reserve(
                    kDiagnosticAudioCaptureFrames * kRuntimeXAudioStereoSamples);
            }
            PPC_STORE_U32(client.wrappedCallbackArg, callbackArg);
            if (!OpenDevice(client)) {
                client = {};
                return kXStatusUnsuccessful;
            }
            client.callbacksDue = kQueuedFrames;
            client.inUse = true;
            PPC_STORE_U32(driverOut, RuntimeAudioDriverToken(selected));
            startWorker = !workerStarted_;
            workerStarted_ = true;
        }

        if (startWorker) {
            try {
                StartGuestHostThread(
                    base, kAudioWorkerStackSize, callback,
                    [this](PPCContext& context, uint8_t* workerBase) {
                        return WorkerMain(context, workerBase);
                    });
            } catch (...) {
                Unregister(RuntimeAudioDriverToken(selected));
                PPC_STORE_U32(driverOut, 0);
                std::lock_guard<std::mutex> lock(mutex_);
                workerStarted_ = false;
                return kXStatusUnsuccessful;
            }
        }
        wake_.notify_one();
        std::cout << "XAUDIO_CLIENT_REGISTER index=" << selected << " token=0x"
                  << std::hex << RuntimeAudioDriverToken(selected) << " callback=0x"
                  << callback << " callback_arg=0x" << callbackArg << " wrapped_arg=0x"
                  << (kWrappedCallbackArgs + uint32_t(selected * 4)) << std::dec
                  << " queued_frames=" << kQueuedFrames
                  << " host_frame_bytes=" << kRuntimeXAudioHostFrameBytes
                  << " host_frames=" << kRuntimeXAudioHostFrameCount
                  << " expected_frame_us=" << kRuntimeXAudioHostFrameDurationUs
                  << " backend=waveout_pcm16\n";
        return kXErrorSuccess;
    }

    uint32_t Unregister(uint32_t driverToken) {
        size_t index{};
        if (!RuntimeAudioDecodeDriverToken(driverToken, &index)) {
            return kXErrorInvalidArgument;
        }
        HWAVEOUT device{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto& client = clients_[index];
            if (!client.inUse || !client.device) return kXErrorInvalidArgument;
            client.inUse = false;
            client.callback = 0;
            client.callbacksDue = 0;
            submissionIdle_.wait(lock, [&client] {
                return client.submissionsInFlight == 0;
            });
            device = client.device;
        }
        CloseDevice(index, device);
        std::cout << "XAUDIO_CLIENT_UNREGISTER index=" << index << " token=0x"
                  << std::hex << driverToken << std::dec << '\n';
        return kXErrorSuccess;
    }

    uint32_t Submit(uint8_t* base, uint32_t driverToken, uint32_t samples) {
        return SubmitWithWriter(
            base, driverToken, samples,
            [](HWAVEOUT device, WAVEHDR* header, UINT headerBytes) {
                return waveOutWrite(device, header, headerBytes);
            });
    }

    bool RunSynchronousCompletionRegression() {
        std::array<uint8_t, kRuntimeXAudioFrameBytes + 64> guestFrame{};
        constexpr uint32_t kSamples = 32;
        auto& client = clients_[0];
        client.owner = this;
        client.index = 0;
        client.inUse = true;
        client.device = reinterpret_cast<HWAVEOUT>(uintptr_t{1});
        for (size_t index = 0; index < client.buffers.size(); ++index) {
            client.buffers[index].header.dwUser = static_cast<DWORD_PTR>(index);
        }

        bool writerObservedUnlockedMutex = false;
        bool synchronousCompletionReached = false;
        const uint32_t result = SubmitWithWriter(
            guestFrame.data(), RuntimeAudioDriverToken(0), kSamples,
            [&](HWAVEOUT, WAVEHDR* header, UINT) {
                if (!mutex_.try_lock()) return MMRESULT{MMSYSERR_ERROR};
                mutex_.unlock();
                writerObservedUnlockedMutex = true;
                FrameComplete(&client, header);
                synchronousCompletionReached = true;
                return MMRESULT{MMSYSERR_NOERROR};
            });

        bool anyQueued = false;
        for (const auto& buffer : client.buffers) anyQueued |= buffer.queued;
        client.device = nullptr;
        client.inUse = false;
        return result == kXErrorSuccess && writerObservedUnlockedMutex &&
               synchronousCompletionReached && !client.submissionsInFlight &&
               !anyQueued && client.deviceCompletionCount == 1;
    }

private:
    template <typename Writer>
    uint32_t SubmitWithWriter(uint8_t* base, uint32_t driverToken,
                              uint32_t samples, Writer&& writer) {
        size_t index{};
        if (!base || !samples || samples > UINT32_MAX - kRuntimeXAudioFrameBytes ||
            !RuntimeAudioDecodeDriverToken(driverToken, &index)) {
            return kXErrorInvalidArgument;
        }

        size_t bufferIndex = kQueuedFrames;
        MMRESULT writeResult = MMSYSERR_ERROR;
        const uint64_t candidateOrdinal = submittedFrames_ + 1;
        const bool recordDiagnostics = RuntimeAudioDiagnosticsEnabled();
        // A 256-frame period emits roughly once per 1.37 seconds at the
        // correct 48 kHz cadence and made the diagnostic stream itself a
        // measurable participant in the pacing path. Keep the startup sample
        // and one aggregate checkpoint about every 21.8 seconds instead.
        bool traceFrame = recordDiagnostics &&
            (candidateOrdinal <= 8 || (candidateOrdinal & 0xFFFu) == 0);
        bool firstNonSilentPcm{};
        uint32_t sourceNonzero{};
        uint32_t sourceNonfinite{};
        uint32_t sourceOverOne{};
        float sourcePeak{};
        uint32_t sourceHash = 2166136261u;
        uint32_t pcmZero{};
        uint32_t pcmClipped{};
        uint32_t pcmPeak{};
        uint64_t pcmSquareSum{};
        uint32_t pcmHash = 2166136261u;
        uint32_t queuedBefore{};
        uint32_t queuedAfter{};
        uint32_t callbacksDue{};
        uint64_t queueDrains{};
        uint64_t deviceCompletions{};
        uint64_t deviceGapUs{};
        uint64_t deviceAverageGapUs{};
        uint64_t deviceMaximumGapUs{};
        uint64_t writerDurationUs{};
        bool diagnosticCaptureBegan{};
        size_t diagnosticGuestSizeBefore{};
        size_t diagnosticHostSizeBefore{};
        std::vector<uint8_t> completedDiagnosticGuestFrames;
        std::vector<int16_t> completedDiagnosticHostSamples;
        uint64_t completedDiagnosticFirstOrdinal{};
        HWAVEOUT device{};
        WAVEHDR* header{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& client = clients_[index];
            if (!client.inUse || !client.device) return kXErrorInvalidArgument;
            for (size_t candidate = 0; candidate < client.buffers.size(); ++candidate) {
                if (client.buffers[candidate].queued) {
                    if (recordDiagnostics) ++queuedBefore;
                } else if (bufferIndex == kQueuedFrames) {
                    bufferIndex = candidate;
                }
            }
            if (bufferIndex == kQueuedFrames) return kXStatusUnsuccessful;
            auto& buffer = client.buffers[bufferIndex];
            buffer.queued = true;
            ++client.submissionsInFlight;
            device = client.device;
            header = &buffer.header;
            RuntimeAudioConvertFrameToStereoPcm16(base + samples, buffer.samples.data());
            const uint32_t masterVolume = RuntimeAudioMasterVolumeQ16();
            if (masterVolume != kRuntimeAudioMasterVolumeOneQ16) {
                RuntimeAudioScaleHostPcm16(buffer.samples.data(),
                                           buffer.samples.size(),
                                           masterVolume);
            }
            // A transition-only sample is needed to distinguish a correctly
            // mixed first audible frame from prolonged silence without
            // hashing every 5.33 ms frame. This scan touches the 1 KiB PCM
            // buffer that was just written and emits at most once per client.
            firstNonSilentPcm = recordDiagnostics &&
                !client.reportedFirstNonSilentPcm &&
                std::any_of(buffer.samples.begin(), buffer.samples.end(),
                            [](int16_t value) { return value != 0; });
            traceFrame |= firstNonSilentPcm;
            if (firstNonSilentPcm && !client.diagnosticCaptureComplete &&
                !client.diagnosticCaptureActive) {
                client.diagnosticCaptureActive = true;
                client.diagnosticCaptureFirstOrdinal = candidateOrdinal;
                diagnosticCaptureBegan = true;
            }
            if (client.diagnosticCaptureActive &&
                client.diagnosticHostSamples.size() <
                    kDiagnosticAudioCaptureFrames *
                        kRuntimeXAudioStereoSamples) {
                diagnosticGuestSizeBefore =
                    client.diagnosticGuestFrames.size();
                diagnosticHostSizeBefore =
                    client.diagnosticHostSamples.size();
                client.diagnosticGuestFrames.insert(
                    client.diagnosticGuestFrames.end(), base + samples,
                    base + samples + kRuntimeXAudioFrameBytes);
                client.diagnosticHostSamples.insert(
                    client.diagnosticHostSamples.end(), buffer.samples.begin(),
                    buffer.samples.end());
            }
            if (traceFrame) {
                for (size_t byte = 0; byte < kRuntimeXAudioFrameBytes; ++byte) {
                    sourceHash ^= base[samples + byte];
                    sourceHash *= 16777619u;
                }
                for (size_t sample = 0;
                     sample < kRuntimeXAudioChannelSamples * kRuntimeXAudioChannelCount;
                     ++sample) {
                    const float value = LoadBigEndianFloat(base + samples + sample * sizeof(float));
                    if (!std::isfinite(value)) {
                        ++sourceNonfinite;
                        continue;
                    }
                    if (value != 0.0f) ++sourceNonzero;
                    const float magnitude = std::abs(value);
                    if (magnitude > 1.0f) ++sourceOverOne;
                    sourcePeak = std::max(sourcePeak, magnitude);
                }
                for (const int16_t value : buffer.samples) {
                    const uint16_t bits = static_cast<uint16_t>(value);
                    pcmHash ^= static_cast<uint8_t>(bits);
                    pcmHash *= 16777619u;
                    pcmHash ^= static_cast<uint8_t>(bits >> 8);
                    pcmHash *= 16777619u;
                    const uint32_t magnitude = value == INT16_MIN
                        ? uint32_t(INT16_MAX) + 1u
                        : static_cast<uint32_t>(std::abs(int32_t(value)));
                    if (!value) ++pcmZero;
                    if (magnitude >= uint32_t(INT16_MAX)) ++pcmClipped;
                    pcmPeak = std::max(pcmPeak, magnitude);
                    pcmSquareSum += uint64_t(magnitude) * magnitude;
                }
            }
        }

        // waveOutWrite may synchronously wait for, or race with, a WOM_DONE
        // callback. FrameComplete takes mutex_, so holding it here creates a
        // process-wide audio deadlock: the submitter waits inside the driver
        // while the driver callback waits for this mutex. submissionsInFlight
        // pins the device until this call returns without blocking callbacks.
        const auto writerStart = recordDiagnostics
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        writeResult = writer(device, header, sizeof(*header));
        if (recordDiagnostics) {
            writerDurationUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - writerStart).count());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& client = clients_[index];
            auto& buffer = client.buffers[bufferIndex];
            if (client.submissionsInFlight) --client.submissionsInFlight;
            if (!client.submissionsInFlight) submissionIdle_.notify_all();
            if (writeResult != MMSYSERR_NOERROR) {
                if (client.diagnosticCaptureActive) {
                    client.diagnosticGuestFrames.resize(
                        diagnosticGuestSizeBefore);
                    client.diagnosticHostSamples.resize(
                        diagnosticHostSizeBefore);
                    if (diagnosticCaptureBegan) {
                        client.diagnosticCaptureActive = false;
                        client.diagnosticCaptureFirstOrdinal = 0;
                    }
                }
                if (buffer.queued) {
                    buffer.queued = false;
                    if (client.inUse && client.callbacksDue < kQueuedFrames) {
                        ++client.callbacksDue;
                        wake_.notify_one();
                    }
                }
            } else if (firstNonSilentPcm) {
                client.reportedFirstNonSilentPcm = true;
            }
            if (writeResult == MMSYSERR_NOERROR &&
                client.diagnosticCaptureActive &&
                client.diagnosticHostSamples.size() ==
                    kDiagnosticAudioCaptureFrames *
                        kRuntimeXAudioStereoSamples) {
                client.diagnosticCaptureActive = false;
                client.diagnosticCaptureComplete = true;
                completedDiagnosticFirstOrdinal =
                    client.diagnosticCaptureFirstOrdinal;
                completedDiagnosticGuestFrames =
                    std::move(client.diagnosticGuestFrames);
                completedDiagnosticHostSamples =
                    std::move(client.diagnosticHostSamples);
            }
            if (recordDiagnostics) {
                for (const auto& candidate : client.buffers) {
                    if (candidate.queued) ++queuedAfter;
                }
                callbacksDue = client.callbacksDue;
                queueDrains = queueDrainCount_;
                deviceCompletions = client.deviceCompletionCount;
                deviceGapUs = client.deviceCompletionLastGapUs;
                deviceAverageGapUs = client.deviceCompletionCount > 1
                    ? client.deviceCompletionGapTotalUs / (client.deviceCompletionCount - 1)
                    : 0;
                deviceMaximumGapUs = client.deviceCompletionMaximumGapUs;
            }
        }
        if (writeResult != MMSYSERR_NOERROR) {
            std::cerr << "XAUDIO_SUBMIT_FAILED index=" << index << " mmresult="
                      << writeResult << '\n';
            return kXStatusUnsuccessful;
        }
        if (diagnosticCaptureBegan) {
            std::cout << "XAUDIO_DIAGNOSTIC_CAPTURE_BEGIN first_ordinal="
                      << candidateOrdinal
                      << " frames=" << kDiagnosticAudioCaptureFrames
                      << " duration_ms="
                      << (kDiagnosticAudioCaptureFrames *
                          kRuntimeXAudioHostFrameDurationUs / 1000)
                      << '\n';
        }
        if (!completedDiagnosticHostSamples.empty()) {
            std::thread(WriteDiagnosticAudioCapture,
                        std::move(completedDiagnosticGuestFrames),
                        std::move(completedDiagnosticHostSamples),
                        completedDiagnosticFirstOrdinal)
                .detach();
        }
        submittedFrames_ = candidateOrdinal;
        if (traceFrame) {
            const uint32_t pcmRms = static_cast<uint32_t>(std::sqrt(
                static_cast<double>(pcmSquareSum) / kRuntimeXAudioStereoSamples));
            std::cout << "XAUDIO_FRAME_SUBMIT index=" << index << " samples=0x"
                      << std::hex << samples << std::dec << " ordinal=" << candidateOrdinal
                      << " queue_before=" << queuedBefore << " queue_after=" << queuedAfter
                      << " callbacks_due=" << callbacksDue << " queue_drains=" << queueDrains
                      << " device_completions=" << deviceCompletions
                      << " device_gap_us=" << deviceGapUs
                      << " device_average_gap_us=" << deviceAverageGapUs
                      << " device_maximum_gap_us=" << deviceMaximumGapUs
                      << " writer_duration_us=" << writerDurationUs
                      << " host_frame_bytes=" << kRuntimeXAudioHostFrameBytes
                      << " host_frames=" << kRuntimeXAudioHostFrameCount
                      << " expected_frame_us=" << kRuntimeXAudioHostFrameDurationUs
                      << " first_non_silent=" << firstNonSilentPcm
                      << " source_nonzero=" << sourceNonzero
                      << " source_nonfinite=" << sourceNonfinite
                      << " source_over1=" << sourceOverOne << " source_peak=" << sourcePeak
                      << " source_hash=0x" << std::hex << sourceHash << std::dec
                      << " pcm_zero=" << pcmZero << " pcm_clipped=" << pcmClipped
                      << " pcm_peak=" << pcmPeak << " pcm_rms=" << pcmRms
                      << " pcm_hash=0x" << std::hex << pcmHash << std::dec << '\n';
        }
        return kXErrorSuccess;
    }

public:
    void Shutdown() noexcept {
        std::array<HWAVEOUT, kMaximumClients> devices{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (shuttingDown_) return;
            shuttingDown_ = true;
            for (size_t index = 0; index < clients_.size(); ++index) {
                clients_[index].inUse = false;
                clients_[index].callback = 0;
                clients_[index].callbacksDue = 0;
            }
            submissionIdle_.wait(lock, [this] {
                return std::all_of(clients_.begin(), clients_.end(),
                                   [](const AudioClient& client) {
                                       return client.submissionsInFlight == 0;
                                   });
            });
            for (size_t index = 0; index < clients_.size(); ++index) {
                devices[index] = clients_[index].device;
            }
        }
        wake_.notify_all();
        for (size_t index = 0; index < devices.size(); ++index) {
            if (devices[index]) CloseDevice(index, devices[index]);
        }
    }

    void FrameComplete(AudioClient* client, WAVEHDR* header) noexcept {
        if (!client || !header) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (client->index >= clients_.size() || &clients_[client->index] != client) return;
        const size_t bufferIndex = static_cast<size_t>(header->dwUser);
        if (bufferIndex >= client->buffers.size()) return;
        auto& buffer = client->buffers[bufferIndex];
        if (!buffer.queued) return;
        buffer.queued = false;
        if (RuntimeAudioDiagnosticsEnabled()) {
            const auto completionTime = std::chrono::steady_clock::now();
            if (client->deviceCompletionCount) {
                client->deviceCompletionLastGapUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        completionTime - client->previousDeviceCompletion).count());
                client->deviceCompletionGapTotalUs += client->deviceCompletionLastGapUs;
                client->deviceCompletionMaximumGapUs =
                    std::max(client->deviceCompletionMaximumGapUs,
                             client->deviceCompletionLastGapUs);
            }
            client->previousDeviceCompletion = completionTime;
        }
        // This counter also verifies completion ownership in the synchronous
        // callback regression. Incrementing it is semantic bookkeeping; only
        // the clock reads and gap aggregation are diagnostic.
        ++client->deviceCompletionCount;
        bool anyQueued = false;
        for (const auto& candidate : client->buffers) {
            anyQueued |= candidate.queued;
        }
        if (!anyQueued) ++queueDrainCount_;
        if (client->inUse && client->callbacksDue < kQueuedFrames) {
            ++client->callbacksDue;
            wake_.notify_one();
        }
    }

    static void CALLBACK WaveCallback(HWAVEOUT, UINT message, DWORD_PTR instance,
                                      DWORD_PTR parameter1, DWORD_PTR) {
        if (message != WOM_DONE || !instance || !parameter1) return;
        auto* client = reinterpret_cast<AudioClient*>(instance);
        if (client->owner) {
            client->owner->FrameComplete(client, reinterpret_cast<WAVEHDR*>(parameter1));
        }
    }

    bool OpenDevice(AudioClient& client) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(kRuntimeXAudioHostChannelCount);
        format.nSamplesPerSec = static_cast<DWORD>(kRuntimeXAudioHostSampleRate);
        format.wBitsPerSample = static_cast<WORD>(kRuntimeXAudioHostBitsPerSample);
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const MMRESULT openResult = waveOutOpen(
            &client.device, WAVE_MAPPER, &format,
            reinterpret_cast<DWORD_PTR>(&RuntimeAudioSystem::WaveCallback),
            reinterpret_cast<DWORD_PTR>(&client), CALLBACK_FUNCTION);
        if (openResult != MMSYSERR_NOERROR) {
            std::cerr << "XAUDIO_DEVICE_OPEN_FAILED index=" << client.index
                      << " mmresult=" << openResult << '\n';
            client.device = nullptr;
            return false;
        }

        for (size_t index = 0; index < client.buffers.size(); ++index) {
            auto& buffer = client.buffers[index];
            buffer.header = {};
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength =
                static_cast<DWORD>(kRuntimeXAudioHostFrameBytes);
            buffer.header.dwUser = static_cast<DWORD_PTR>(index);
            const MMRESULT prepareResult =
                waveOutPrepareHeader(client.device, &buffer.header, sizeof(buffer.header));
            if (prepareResult != MMSYSERR_NOERROR) {
                std::cerr << "XAUDIO_DEVICE_PREPARE_FAILED index=" << client.index
                          << " buffer=" << index << " mmresult=" << prepareResult << '\n';
                for (size_t prepared = 0; prepared < index; ++prepared) {
                    waveOutUnprepareHeader(client.device, &client.buffers[prepared].header,
                                           sizeof(WAVEHDR));
                    client.buffers[prepared].prepared = false;
                }
                waveOutClose(client.device);
                client.device = nullptr;
                return false;
            }
            buffer.prepared = true;
            buffer.queued = false;
        }
        return true;
    }

    void CloseDevice(size_t index, HWAVEOUT expectedDevice) noexcept {
        if (index >= clients_.size() || !expectedDevice) return;
        waveOutReset(expectedDevice);
        auto& client = clients_[index];
        for (auto& buffer : client.buffers) {
            if (buffer.prepared) {
                waveOutUnprepareHeader(expectedDevice, &buffer.header, sizeof(buffer.header));
                buffer.prepared = false;
            }
            buffer.queued = false;
        }
        waveOutClose(expectedDevice);
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.device == expectedDevice) {
            client.device = nullptr;
            client.callbackArg = 0;
            client.wrappedCallbackArg = 0;
        }
    }

    uint32_t WorkerMain(PPCContext& context, uint8_t* base) {
        const bool recordDiagnostics = RuntimeAudioDiagnosticsEnabled();
        uint64_t callbackOrdinal = 0;
        uint64_t callbackGapTotalUs = 0;
        uint64_t callbackGapMaximumUs = 0;
        uint64_t callbackDurationTotalUs = 0;
        uint64_t callbackDurationMaximumUs = 0;
        std::chrono::steady_clock::time_point previousCallbackStart{};
        while (true) {
            uint32_t callback{};
            uint32_t wrappedArg{};
            size_t index{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    if (shuttingDown_ || GuestRuntimeStopRequested()) return true;
                    for (const auto& client : clients_) {
                        if (client.inUse && client.callback && client.callbacksDue) return true;
                    }
                    return false;
                });
                if (shuttingDown_ || GuestRuntimeStopRequested()) return 0;
                bool found = false;
                for (size_t candidate = 0; candidate < clients_.size(); ++candidate) {
                    auto& client = clients_[candidate];
                    if (!client.inUse || !client.callback || !client.callbacksDue) continue;
                    --client.callbacksDue;
                    callback = client.callback;
                    wrappedArg = client.wrappedCallbackArg;
                    index = candidate;
                    found = true;
                    break;
                }
                if (!found) continue;
            }

            if (!RuntimeGeneratedAddressInRange(callback)) {
                throw std::runtime_error("XAudio callback is outside generated code");
            }
            PPCFunc* routine = PPC_LOOKUP_FUNC(base, callback);
            if (!routine) throw std::runtime_error("XAudio callback has no generated function");
            ++callbackOrdinal;
            const auto callbackStart = recordDiagnostics
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const uint64_t callbackGapUs = recordDiagnostics &&
                    previousCallbackStart.time_since_epoch().count()
                ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                      callbackStart - previousCallbackStart).count())
                : 0;
            if (recordDiagnostics) previousCallbackStart = callbackStart;
            if (recordDiagnostics && callbackOrdinal > 1) {
                callbackGapTotalUs += callbackGapUs;
                callbackGapMaximumUs = std::max(callbackGapMaximumUs, callbackGapUs);
            }
            if (recordDiagnostics &&
                (callbackOrdinal <= 16 || (callbackOrdinal & 0xFFFu) == 0)) {
                std::cout << "XAUDIO_CALLBACK_DISPATCH index=" << index << " callback=0x"
                          << std::hex << callback << " arg=0x" << wrappedArg << std::dec
                          << " ordinal=" << callbackOrdinal
                          << " host_tid=" << GetCurrentThreadId() << '\n';
            }
            const uint32_t savedStack = context.r1.u32;
            const uint64_t savedLr = context.lr;
            context.r1.u64 -= 64 + 112;
            context.lr = 0xBCBCBCBC;
            context.r3.u64 = wrappedArg;
            try {
                routine(context, base);
            } catch (...) {
                context.r1.u64 = savedStack;
                context.lr = savedLr;
                throw;
            }
            context.r1.u64 = savedStack;
            context.lr = savedLr;
            const uint64_t callbackDurationUs = recordDiagnostics
                ? static_cast<uint64_t>(
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - callbackStart).count())
                : 0;
            if (recordDiagnostics) {
                callbackDurationTotalUs += callbackDurationUs;
                callbackDurationMaximumUs =
                    std::max(callbackDurationMaximumUs, callbackDurationUs);
            }
            if (recordDiagnostics &&
                (callbackOrdinal <= 16 || (callbackOrdinal & 0xFFFu) == 0)) {
                std::cout << "XAUDIO_CALLBACK_TIMING index=" << index
                          << " ordinal=" << callbackOrdinal
                          << " host_tid=" << GetCurrentThreadId()
                          << " gap_us=" << callbackGapUs
                          << " average_gap_us="
                          << (callbackOrdinal > 1
                                  ? callbackGapTotalUs / (callbackOrdinal - 1)
                                  : 0)
                          << " maximum_gap_us=" << callbackGapMaximumUs
                          << " duration_us=" << callbackDurationUs
                          << " average_us=" << callbackDurationTotalUs / callbackOrdinal
                          << " maximum_us=" << callbackDurationMaximumUs << '\n';
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable submissionIdle_;
    std::array<AudioClient, kMaximumClients> clients_{};
    bool workerStarted_{};
    bool shuttingDown_{};
    uint64_t submittedFrames_{};
    uint64_t queueDrainCount_{};
};

RuntimeAudioSystem& GetRuntimeAudioSystem() {
    static RuntimeAudioSystem system;
    return system;
}

class RuntimeXmaSystem {
public:
    bool InitializeHardware(uint8_t* base) {
        if (!base) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return true;
        // The title reads XMA ContextArrayAddress (0x7FEA1800) during its
        // audio-library initialization, before its first XMACreateContext.
        // Therefore the hardware context array is a VdInitializeEngines-time
        // kernel resource, not a lazy XMACreateContext allocation.
        if (!RuntimeGraphicsIsActive()) return false;
        constexpr uint32_t pageCount =
            kXmaContextBytes / GuestMemoryAccounting::kPageSize;
        uint32_t firstPage{};
        if (!GetGuestMemoryAccounting().ReservePhysicalPages(
                pageCount, 1, 0,
                GuestMemoryAccounting::kPhysicalPages - 1,
                0x4, &firstPage)) {
            return false;
        }
        contextArrayFirstPage_ = firstPage;
        contextArray_ =
            kPhysicalGuestBase + firstPage * GuestMemoryAccounting::kPageSize;
        RuntimeGeneratedMemset(base, base + contextArray_, 0, kXmaContextBytes, __FILE__, __LINE__);
        try {
            if (!RuntimeGraphicsInitializeXma(contextArray_)) {
                GetGuestMemoryAccounting().ReleasePhysicalPages(firstPage);
                contextArray_ = 0;
                contextArrayFirstPage_ = 0;
                return false;
            }
        } catch (...) {
            GetGuestMemoryAccounting().ReleasePhysicalPages(firstPage);
            contextArray_ = 0;
            contextArrayFirstPage_ = 0;
            throw;
        }
        initialized_ = true;
        std::cout << "XMA_CONTEXT_ARRAY_READY guest=0x" << std::hex
                  << contextArray_ << " physical=0x"
                  << contextArrayFirstPage_ * GuestMemoryAccounting::kPageSize
                  << " bytes=0x" << kXmaContextBytes << std::dec << '\n';
        return true;
    }

    uint32_t Create(uint8_t* base, uint32_t contextOut) {
        if (!base || !contextOut || (contextOut & 3u)) return kXStatusInvalidParameter;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return kXStatusUnsuccessful;
        const uint32_t context = RuntimeGraphicsAllocateXmaContext();
        if (!context) return kXStatusNoMemory;
        liveContexts_.insert(context);
        PPC_STORE_U32(contextOut, context);
        if constexpr (kCompletedAudioLifecycleDiagnosticsEnabled) {
            std::cout << "XMA_CONTEXT_CREATE context=0x" << std::hex << context
                      << " array=0x" << contextArray_ << std::dec
                      << " live=" << liveContexts_.size() << '\n';
        }
        return kXErrorSuccess;
    }

    uint32_t Release(uint32_t context) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = liveContexts_.find(context);
        if (it == liveContexts_.end() ||
            !RuntimeGraphicsReleaseXmaContext(context)) {
            return kXStatusInvalidParameter;
        }
        liveContexts_.erase(it);
        if constexpr (kCompletedAudioLifecycleDiagnosticsEnabled) {
            std::cout << "XMA_CONTEXT_RELEASE context=0x" << std::hex << context
                      << std::dec << " live=" << liveContexts_.size() << '\n';
        }
        return kXErrorSuccess;
    }

    void Shutdown() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;
        RuntimeGraphicsShutdownXma();
        liveContexts_.clear();
        GetGuestMemoryAccounting().ReleasePhysicalPages(contextArrayFirstPage_);
        contextArray_ = 0;
        contextArrayFirstPage_ = 0;
        initialized_ = false;
    }

private:
    std::mutex mutex_;
    std::unordered_set<uint32_t> liveContexts_;
    uint32_t contextArray_{};
    uint32_t contextArrayFirstPage_{};
    bool initialized_{};
};

RuntimeXmaSystem& GetRuntimeXmaSystem() {
    static RuntimeXmaSystem system;
    return system;
}
}

void ConfigureRuntimeAudioDiagnostics(bool enabled) noexcept {
    runtimeAudioDiagnosticsEnabled.store(enabled, std::memory_order_relaxed);
}

bool RuntimeAudioDiagnosticsEnabled() noexcept {
    return runtimeAudioDiagnosticsEnabled.load(std::memory_order_relaxed);
}

uint32_t RuntimeAudioDriverToken(size_t index) noexcept {
    return index < kMaximumClients ? kDriverTokenPrefix | static_cast<uint32_t>(index) : 0;
}

bool RuntimeAudioDecodeDriverToken(uint32_t token, size_t* index) noexcept {
    if ((token & 0xFFFF0000u) != kDriverTokenPrefix) return false;
    const size_t decoded = token & 0xFFFFu;
    if (decoded >= kMaximumClients) return false;
    if (index) *index = decoded;
    return true;
}

uint32_t RuntimeAudioGetVoiceCategoryVolumeChangeMask(
    uint32_t driverToken, uint32_t* mask) noexcept {
    if (!mask || !RuntimeAudioDecodeDriverToken(driverToken, nullptr)) {
        return kXErrorInvalidArgument;
    }
    // This API is a non-blocking state query. With no host-side category
    // mutation source, the verified result is an empty change mask. Do not add
    // a host scheduler sleep here: it is not guest-visible API semantics and
    // this query is issued once per 256-sample render callback.
    *mask = 0;
    return kXErrorSuccess;
}

void RuntimeAudioConvertFrameToStereoPcm16(const uint8_t* guestFrame,
                                           int16_t* stereoSamples) {
    if (!guestFrame || !stereoSamples) {
        throw std::runtime_error("XAudio frame conversion has a null buffer");
    }
    constexpr float kCenterScale = 0.5f;
    constexpr float kDownmixScale = 1.0f / 2.5f;
    for (size_t sample = 0; sample < kRuntimeXAudioChannelSamples; ++sample) {
        const auto channel = [&](size_t index) {
            return LoadBigEndianFloat(
                guestFrame + (index * kRuntimeXAudioChannelSamples + sample) * sizeof(float));
        };
        const float center = channel(2) * kCenterScale;
        const float left = (channel(0) + channel(4) + center) * kDownmixScale;
        const float right = (channel(1) + channel(5) + center) * kDownmixScale;
        stereoSamples[sample * 2] = FloatToPcm16(left);
        stereoSamples[sample * 2 + 1] = FloatToPcm16(right);
    }
}

int16_t ScaleRuntimeAudioHostSample(int16_t sample,
                                    uint32_t scaleQ16) noexcept {
    if (scaleQ16 >= kRuntimeAudioMasterVolumeOneQ16) return sample;
    int64_t scaled = int64_t(sample) * scaleQ16;
    scaled += scaled >= 0 ? 32768 : -32768;
    return static_cast<int16_t>(scaled / int64_t{65536});
}

void RuntimeAudioScaleHostPcm16(int16_t* samples, size_t sampleCount,
                                uint32_t scaleQ16) noexcept {
    if (!samples || scaleQ16 >= kRuntimeAudioMasterVolumeOneQ16) return;
    for (size_t index = 0; index < sampleCount; ++index) {
        samples[index] = ScaleRuntimeAudioHostSample(samples[index], scaleQ16);
    }
}

void ConfigureRuntimeAudioMasterVolume(double volume) {
    if (!std::isfinite(volume) || volume < 0.0 || volume > 1.0) {
        throw std::runtime_error("audio.master_volume must be within 0..1");
    }
    runtimeAudioMasterVolumeQ16.store(
        static_cast<uint32_t>(std::llround(
            volume * kRuntimeAudioMasterVolumeOneQ16)),
        std::memory_order_release);
}

uint32_t RuntimeAudioMasterVolumeQ16() noexcept {
    return runtimeAudioMasterVolumeQ16.load(std::memory_order_acquire);
}

bool RuntimeAudioRunSynchronousCompletionRegression() {
    RuntimeAudioSystem system;
    return system.RunSynchronousCompletionRegression();
}

uint32_t RuntimeAudioRegisterRenderDriverClient(uint8_t* base,
                                                uint32_t callbackPair,
                                                uint32_t driverOut) {
    return GetRuntimeAudioSystem().Register(base, callbackPair, driverOut);
}

uint32_t RuntimeAudioUnregisterRenderDriverClient(uint32_t driverToken) {
    return GetRuntimeAudioSystem().Unregister(driverToken);
}

uint32_t RuntimeAudioSubmitRenderDriverFrame(uint8_t* base,
                                             uint32_t driverToken,
                                             uint32_t samples) {
    return GetRuntimeAudioSystem().Submit(base, driverToken, samples);
}

bool RuntimeAudioInitializeXmaHardware(uint8_t* base) {
    return GetRuntimeXmaSystem().InitializeHardware(base);
}

uint32_t RuntimeAudioCreateXmaContext(uint8_t* base, uint32_t contextOut) {
    return GetRuntimeXmaSystem().Create(base, contextOut);
}

uint32_t RuntimeAudioReleaseXmaContext(uint32_t contextAddress) {
    return GetRuntimeXmaSystem().Release(contextAddress);
}

void ShutdownRuntimeAudio() noexcept {
    std::cerr << "RUNTIME_AUDIO_SHUTDOWN_BEGIN\n";
    GetRuntimeAudioSystem().Shutdown();
    std::cerr << "RUNTIME_AUDIO_DEVICE_SHUTDOWN_COMPLETE\n";
    GetRuntimeXmaSystem().Shutdown();
    std::cerr << "RUNTIME_AUDIO_SHUTDOWN_COMPLETE\n";
}
