#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

struct PPCContext;

class GuestThreadExit final : public std::exception {
public:
    const char* what() const noexcept override { return "guest thread exited"; }
};

class GuestRuntimeStop final : public std::exception {
public:
    const char* what() const noexcept override { return "guest runtime stop requested"; }
};

struct GuestThreadStartResult {
    uint32_t handle;
    uint32_t threadId;
    uint32_t guestObject;
};

struct GuestThreadDispatchPlan {
    uint32_t address;
    uint32_t r3;
    uint32_t r4;
    bool rawThread;
};

GuestThreadDispatchPlan ResolveGuestThreadDispatch(uint32_t xapiStartup,
                                                   uint32_t startAddress,
                                                   uint32_t startContext) noexcept;

uint32_t InitializeMainGuestThread(uint8_t* base);
uint32_t CurrentGuestThreadObject();
uint32_t CurrentGuestThreadId();
uint32_t CurrentGuestThreadHandle();
bool GuestProcessorNumberFromAffinity(uint32_t affinity,
                                      uint8_t* processorNumber) noexcept;
bool GuestCreationAffinity(uint32_t creationFlags, uint32_t* affinity) noexcept;
// Captures the host floating-point control state for a newly created guest
// context while keeping native exceptions masked. Guest exception policy is
// represented separately by the emulated FPSCR/kernel APIs.
void InitializeGuestFloatingPointHostState(PPCContext& context) noexcept;
bool SetGuestThreadAffinity(uint8_t* base, uint32_t guestObject,
                            uint32_t affinity, uint32_t* previousAffinity);
int32_t EnterGuestCriticalRegion() noexcept;
int32_t LeaveGuestCriticalRegion() noexcept;
int32_t CurrentGuestApcDisableCount() noexcept;
void SetCurrentGuestThreadForTests(uint32_t guestObject, uint32_t threadId,
                                   uint32_t handle = 0);
GuestThreadStartResult StartGuestThread(PPCContext& parentContext, uint8_t* base,
                                        uint32_t stackSize, uint32_t xapiStartup,
                                        uint32_t startAddress, uint32_t startContext,
                                        uint32_t creationFlags);
// Creates a kernel-visible guest thread environment for a runtime-owned worker
// that must execute guest callbacks (audio, notifications, and similar host
// services). The worker receives a valid PPC stack, PCR, TEB, and TLS state;
// it must observe GuestRuntimeStopRequested during blocking waits.
GuestThreadStartResult StartGuestHostThread(
    uint8_t* base, uint32_t stackSize, uint32_t reportedEntry,
    std::function<uint32_t(PPCContext&, uint8_t*)> worker);
bool ResumeGuestThread(uint32_t guestObject, uint32_t* previousSuspendCount);
bool SuspendGuestThread(uint32_t guestObject, uint32_t* previousSuspendCount);
void MarkCurrentGuestThreadTerminated(uint8_t* base, uint32_t exitCode);
void ResetGuestRuntimeStop();
void RequestGuestRuntimeStop();
bool GuestRuntimeStopRequested() noexcept;
std::string GuestRuntimeBlockerMessage();
void QueueGuestApc(uint32_t routine, uint32_t context, uint32_t argument1, uint32_t argument2);
bool HasPendingGuestApcs();
bool DeliverGuestApcs(PPCContext& context, uint8_t* base);
void JoinGuestThreads();
