#include "runtime_threads.h"

#include "runtime_guest_bulk_write.h"
#include "ppc_recomp_shared.h"
#include "runtime_objects.h"
#include "runtime_function_trace.h"
#include "runtime_sync.h"
#include "runtime_fatal.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
std::atomic<uint32_t> next_thread_id{2};
std::atomic<uint32_t> next_stack_top{0x7EF00000};
std::atomic<uint64_t> queued_apc_trace_count{};
std::atomic<uint64_t> delivered_apc_trace_count{};
std::atomic<bool> runtime_stop_requested{};
std::mutex runtime_blocker_mutex;
std::string runtime_blocker;
std::mutex threads_mutex;
std::vector<std::thread> threads;
struct GuestThreadStartGate {
    std::mutex mutex;
    std::condition_variable wake;
    uint32_t suspendCount{};
};
std::mutex thread_start_gates_mutex;
std::unordered_map<uint32_t, std::shared_ptr<GuestThreadStartGate>> thread_start_gates;
thread_local uint32_t current_guest_object{};
thread_local uint32_t current_guest_thread_id{};
thread_local uint32_t current_guest_handle{};
thread_local uint32_t current_guest_pcr{};
// KeEnter/LeaveCriticalRegion maintain a signed, per-KTHREAD normal-kernel-APC
// disable count. User APCs from alertable waits and hardware GPU interrupts
// are separate mechanisms and must not consult this count.
thread_local int32_t current_apc_disable_count{};

struct GuestApc {
    uint32_t routine;
    uint32_t context;
    uint32_t argument1;
    uint32_t argument2;
};
thread_local std::vector<GuestApc> pending_apcs;

// Keep kernel thread environments in the private virtual range. 0x7F000000
// aliases physical 0xA0000000 in the runtime memory map and is overwritten by
// the title's first physical allocation.
constexpr uint32_t kPcrBase = 0x7D100000;
constexpr uint32_t kThreadEnvironmentStride = 0x2000;
constexpr uint32_t kPcrBytes = 0x2D8;
constexpr uint32_t kTebOffset = 0x400;
constexpr uint32_t kTlsOffset = 0x800;
// Verified directly from the title's wrapper at 0x828A7F50: byte +0x04 is
// zero while active and nonzero after termination; +0x140 is the exit code.
constexpr uint32_t kThreadStateOffset = 0x04;
constexpr uint32_t kThreadExitCodeOffset = 0x140;

void PublishGuestThreadTermination(uint8_t* base, uint32_t guestObject, uint32_t exitCode) {
    if (!guestObject) throw std::runtime_error("guest thread termination has no object");
    // Publish the exit code first. The guest observes the state byte and then
    // reads the exit code, so the nonzero state word is the release marker.
    PPC_STORE_U32(guestObject + kThreadExitCodeOffset, exitCode);
    PPC_STORE_U32(guestObject + kThreadStateOffset, 1);
    if (!GetGuestObjects().TerminateThread(guestObject, exitCode)) {
        throw std::runtime_error("guest thread termination object is invalid");
    }
}

uint32_t InitializeGuestThreadEnvironment(uint8_t* base, uint32_t threadId,
                                           uint32_t guestObject, uint32_t stackTop,
                                           uint32_t stackSize) {
    const uint32_t pcr = kPcrBase + (threadId - 1) * kThreadEnvironmentStride;
    const uint32_t teb = pcr + kTebOffset;
    RuntimeGeneratedMemset(base, base + pcr, 0, kThreadEnvironmentStride, __FILE__, __LINE__);
    // Verified KPCR/TEB fields used by the title and documented in Xenia's
    // threading implementation. Keep this intentionally narrow: no KTHREAD
    // layout is invented.
    PPC_STORE_U32(pcr + 0x000, pcr + kTlsOffset);
    PPC_STORE_U32(pcr + 0x100, teb);
    PPC_STORE_U32(pcr + 0x10C, 0);
    PPC_STORE_U32(pcr + 0x150, 0);
    PPC_STORE_U32(teb + 0x14C, threadId);
    PPC_STORE_U32(pcr + 0x104, guestObject);
    PPC_STORE_U32(pcr + 0x110, stackTop);
    PPC_STORE_U32(pcr + 0x114, stackTop - stackSize);
    return pcr;
}

uint32_t AllocateStackTop(uint32_t requestedSize) {
    const uint32_t size = (requestedSize + 0xFFF) & ~uint32_t(0xFFF);
    const uint32_t effectiveSize = size < 0x4000 ? 0x4000 : size;
    return next_stack_top.fetch_sub(effectiveSize);
}

void PublishGuestWorkerBlocker(uint32_t threadId, uint32_t guestObject,
                               const std::exception& error) {
    GetGuestObjects().SetThreadState(guestObject, GuestThreadState::Blocked);
    {
        std::lock_guard<std::mutex> blockerLock(runtime_blocker_mutex);
        if (runtime_blocker.empty()) {
            std::ostringstream message;
            message << "guest worker blocker thread=" << threadId << ' ' << error.what();
            runtime_blocker = message.str();
        }
    }
    std::cerr << "GUEST_THREAD_BLOCKER id=" << threadId << ' ' << error.what() << '\n';
    RuntimeFatalRecordDetail("guest worker thread=" + std::to_string(threadId) + ' ' +
                             error.what());
    RuntimeFatalWriteDump("guest-worker-blocker");
    // A worker is part of the same guest process. Once it reaches a real
    // blocker, stop all guest execution so a later failure cannot obscure it.
    RequestGuestRuntimeStop();
}
}

uint32_t InitializeMainGuestThread(uint8_t* base) {
    if (current_guest_object) return current_guest_pcr;
    const auto main = GetGuestObjects().CreateThread(1, 0x70000000, 0x00200000);
    current_guest_object = main.guestObject;
    current_guest_thread_id = 1;
    current_guest_handle = main.handle;
    const uint32_t pcr = InitializeGuestThreadEnvironment(base, 1, main.guestObject, 0x70000000, 0x00200000);
    current_guest_pcr = pcr;
    RuntimeTraceThreadStarted(1, main.guestObject, 0, pcr, pcr + kTebOffset, pcr + kTlsOffset);
    return pcr;
}

void ResetGuestRuntimeStop() {
    {
        std::lock_guard<std::mutex> lock(runtime_blocker_mutex);
        runtime_blocker.clear();
    }
    runtime_stop_requested.store(false, std::memory_order_release);
    RuntimeSetEntrySlowBit(kRuntimeEntrySlowStop, false);
}

void RequestGuestRuntimeStop() {
    runtime_stop_requested.store(true, std::memory_order_release);
    // Route every generated function entry through the full hook so running
    // guest threads observe the stop at their next call boundary.
    RuntimeSetEntrySlowBit(kRuntimeEntrySlowStop, true);
    std::vector<std::shared_ptr<GuestThreadStartGate>> startGates;
    {
        std::lock_guard<std::mutex> lock(thread_start_gates_mutex);
        for (const auto& [guestObject, gate] : thread_start_gates) {
            (void)guestObject;
            startGates.push_back(gate);
        }
    }
    for (const auto& gate : startGates) gate->wake.notify_all();
    GetGuestObjects().WakeAllWaiters();
    GetGuestDispatcherEvents().WakeAllWaiters();
    GetGuestDispatcherSemaphores().WakeAllWaiters();
    GetGuestDispatcherWaitCoordinator().WakeAll();
}

bool GuestRuntimeStopRequested() noexcept {
    return runtime_stop_requested.load(std::memory_order_acquire);
}

std::string GuestRuntimeBlockerMessage() {
    std::lock_guard<std::mutex> lock(runtime_blocker_mutex);
    return runtime_blocker;
}

uint32_t CurrentGuestThreadObject() { return current_guest_object; }
uint32_t CurrentGuestThreadId() { return current_guest_thread_id; }
uint32_t CurrentGuestThreadHandle() { return current_guest_handle; }
bool GuestProcessorNumberFromAffinity(uint32_t affinity,
                                      uint8_t* processorNumber) noexcept {
    if (!processorNumber || !affinity || (affinity & ~0x3Fu) ||
        (affinity & (affinity - 1u))) {
        return false;
    }
    uint8_t processor = 0;
    while ((affinity >> processor) != 1u) ++processor;
    *processorNumber = processor;
    return true;
}

bool GuestCreationAffinity(uint32_t creationFlags, uint32_t* affinity) noexcept {
    if (!affinity) return false;
    const uint32_t requested = creationFlags >> 24;
    if (requested) {
        uint8_t processor{};
        if (!GuestProcessorNumberFromAffinity(requested, &processor)) return false;
    }
    *affinity = requested;
    return true;
}

void InitializeGuestFloatingPointHostState(PPCContext& context) noexcept {
    context.fpscr.loadFromHost();
#if defined(__x86_64__) || defined(_M_X64)
    // MXCSR bits 7..12 mask the six native SIMD floating-point exceptions.
    // Pinned ReXGlue initializes every PPC context the same way before guest
    // execution. Without this, a zero-initialized context's first flush-mode
    // transition writes MXCSR=0 and an ordinary guest fsqrts can raise a host
    // STATUS_FLOAT_INEXACT_RESULT instead of completing architecturally.
    constexpr uint32_t kHostExceptionMask = 0x1F80u;
    context.fpscr.csr |= kHostExceptionMask;
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM FPCR uses enable bits rather than mask bits, so clearing these bits
    // is the equivalent native-exception policy used by pinned ReXGlue.
    constexpr uint32_t kHostExceptionEnableMask = 0x9F00u;
    context.fpscr.csr &= ~kHostExceptionEnableMask;
#endif
    context.fpscr.setcsr(context.fpscr.csr);
}

bool SetGuestThreadAffinity(uint8_t* base, uint32_t guestObject,
                            uint32_t affinity, uint32_t* previousAffinity) {
    uint8_t processor{};
    if (!base || !GuestProcessorNumberFromAffinity(affinity, &processor) ||
        !GetGuestObjects().SetThreadAffinity(guestObject, affinity,
                                             previousAffinity)) {
        return false;
    }
    GuestThreadInfo info{};
    if (!GetGuestObjects().GetThreadInfo(guestObject, &info) || !info.threadId)
        return false;
    const uint32_t pcr = kPcrBase + (info.threadId - 1u) * kThreadEnvironmentStride;
    // Pinned ReXGlue/Xenia keeps both KTHREAD::current_cpu (+0xBF) and
    // KPCR::PrcbData.current_cpu (+0x10C) synchronized. The title's audio
    // barrier reads the latter directly through r13+268.
    PPC_STORE_U8(guestObject + 0xBFu, processor);
    PPC_STORE_U8(pcr + 0x10Cu, processor);
    return true;
}
int32_t EnterGuestCriticalRegion() noexcept { return --current_apc_disable_count; }
int32_t LeaveGuestCriticalRegion() noexcept { return ++current_apc_disable_count; }
int32_t CurrentGuestApcDisableCount() noexcept { return current_apc_disable_count; }

void SetCurrentGuestThreadForTests(uint32_t guestObject, uint32_t threadId, uint32_t handle) {
    current_guest_object = guestObject;
    current_guest_thread_id = threadId;
    current_guest_handle = handle;
}

GuestThreadDispatchPlan ResolveGuestThreadDispatch(uint32_t xapiStartup,
                                                   uint32_t startAddress,
                                                   uint32_t startContext) noexcept {
    if (xapiStartup) return {xapiStartup, startAddress, startContext, false};
    return {startAddress, startContext, 0, true};
}

GuestThreadStartResult StartGuestThread(PPCContext& parentContext, uint8_t* base,
                                        uint32_t stackSize, uint32_t xapiStartup,
                                        uint32_t startAddress, uint32_t startContext,
                                        uint32_t creationFlags) {
    const uint32_t threadId = next_thread_id.fetch_add(1);
    const uint32_t stackTop = AllocateStackTop(stackSize);
    const auto object = GetGuestObjects().CreateThread(threadId, stackTop, stackSize);
    uint32_t creationAffinity{};
    if (!GuestCreationAffinity(creationFlags, &creationAffinity)) {
        throw std::runtime_error("guest thread creation has an invalid processor affinity");
    }
    // The top byte of ExCreateThread's creation flags is a logical-processor
    // mask. The Darkness supplies 0x10 for its audio worker, and the pinned
    // XThread implementation applies this before publishing KTHREAD/KPCR.
    // A zero mask leaves the object manager's existing default unchanged.
    if (creationAffinity &&
        !GetGuestObjects().SetThreadAffinity(object.guestObject, creationAffinity, nullptr)) {
        throw std::runtime_error("guest thread creation affinity could not be retained");
    }
    const auto startGate = std::make_shared<GuestThreadStartGate>();
    startGate->suspendCount = (creationFlags & 1u) ? 1u : 0u;
    {
        std::lock_guard<std::mutex> gateLock(thread_start_gates_mutex);
        thread_start_gates[object.guestObject] = startGate;
    }
    if (startGate->suspendCount) {
        GetGuestObjects().SetThreadState(object.guestObject, GuestThreadState::Suspended);
    }
    std::lock_guard<std::mutex> lock(threads_mutex);
    threads.emplace_back([=] {
        current_guest_object = object.guestObject;
        current_guest_thread_id = threadId;
        current_guest_handle = object.handle;
        try {
            PPCContext context{};
            InitializeGuestFloatingPointHostState(context);
            context.r1.u64 = stackTop;
            const uint32_t pcr = InitializeGuestThreadEnvironment(
                base, threadId, object.guestObject, stackTop, stackSize);
            current_guest_pcr = pcr;
            context.r13.u64 = pcr;
            RuntimeTraceThreadStarted(threadId, object.guestObject, startAddress, pcr,
                                      pcr + kTebOffset, pcr + kTlsOffset);
            {
                std::unique_lock<std::mutex> startLock(startGate->mutex);
                startGate->wake.wait(startLock, [&] {
                    return startGate->suspendCount == 0 || GuestRuntimeStopRequested();
                });
            }
            if (GuestRuntimeStopRequested()) throw GuestRuntimeStop();
            GuestThreadInfo threadInfo{};
            uint8_t processor{};
            if (!GetGuestObjects().GetThreadInfo(object.guestObject, &threadInfo) ||
                !GuestProcessorNumberFromAffinity(threadInfo.affinity, &processor)) {
                throw std::runtime_error("guest thread has an invalid processor affinity");
            }
            // KeSetAffinityThread may run while this suspended worker is still
            // publishing its PCR. Reapply the authoritative object affinity
            // after the start gate so creation ordering cannot clear it.
            PPC_STORE_U8(object.guestObject + 0xBFu, processor);
            PPC_STORE_U8(pcr + 0x10Cu, processor);
            GetGuestObjects().SetThreadState(object.guestObject, GuestThreadState::Running);
            std::cout << "GUEST_THREAD_START id=" << threadId << " entry=0x" << std::hex
                      << startAddress << " startup=0x" << xapiStartup << std::dec << '\n';
            const GuestThreadDispatchPlan dispatch =
                ResolveGuestThreadDispatch(xapiStartup, startAddress, startContext);
            context.r3.u64 = dispatch.r3;
            context.r4.u64 = dispatch.r4;
            if (!RuntimeGeneratedAddressInRange(dispatch.address)) {
                std::ostringstream message;
                message << "guest thread startup outside generated code address=0x" << std::hex
                        << dispatch.address;
                throw std::runtime_error(message.str());
            }
            PPCFunc* startup = PPC_LOOKUP_FUNC(base, dispatch.address);
            if (!startup) throw std::runtime_error("guest thread startup has no generated function");
            startup(context, base);
            // A raw thread's return value is its exit code. XAPI owns termination when a
            // trampoline is present, so a trampoline that returns implicitly exits with zero.
            PublishGuestThreadTermination(base, object.guestObject,
                                          dispatch.rawThread ? context.r3.u32 : 0u);
            RuntimeTraceThreadTerminated(threadId);
        } catch (const GuestThreadExit&) {
            RuntimeTraceThreadTerminated(threadId);
            std::cout << "GUEST_THREAD_EXIT id=" << threadId << '\n';
        } catch (const GuestRuntimeStop&) {
            RuntimeTraceThreadTerminated(threadId);
            std::cout << "GUEST_THREAD_STOP id=" << threadId << '\n';
        } catch (const std::exception& error) {
            PublishGuestWorkerBlocker(threadId, object.guestObject, error);
        }
    });
    return {object.handle, threadId, object.guestObject};
}

GuestThreadStartResult StartGuestHostThread(
    uint8_t* base, uint32_t stackSize, uint32_t reportedEntry,
    std::function<uint32_t(PPCContext&, uint8_t*)> worker) {
    if (!base || !worker) throw std::runtime_error("guest host worker has an invalid contract");
    const uint32_t threadId = next_thread_id.fetch_add(1);
    const uint32_t stackTop = AllocateStackTop(stackSize);
    const auto object = GetGuestObjects().CreateThread(threadId, stackTop, stackSize);
    std::lock_guard<std::mutex> lock(threads_mutex);
    threads.emplace_back([=, worker = std::move(worker)]() mutable {
        current_guest_object = object.guestObject;
        current_guest_thread_id = threadId;
        current_guest_handle = object.handle;
        try {
            PPCContext context{};
            InitializeGuestFloatingPointHostState(context);
            context.r1.u64 = stackTop;
            const uint32_t pcr = InitializeGuestThreadEnvironment(
                base, threadId, object.guestObject, stackTop, stackSize);
            current_guest_pcr = pcr;
            context.r13.u64 = pcr;
            RuntimeTraceThreadStarted(threadId, object.guestObject, reportedEntry, pcr,
                                      pcr + kTebOffset, pcr + kTlsOffset);
            std::cout << "GUEST_HOST_THREAD_START id=" << threadId << " entry=0x"
                      << std::hex << reportedEntry << std::dec << '\n';
            const uint32_t exitCode = worker(context, base);
            PublishGuestThreadTermination(base, object.guestObject, exitCode);
            RuntimeTraceThreadTerminated(threadId);
        } catch (const GuestThreadExit&) {
            RuntimeTraceThreadTerminated(threadId);
            std::cout << "GUEST_THREAD_EXIT id=" << threadId << '\n';
        } catch (const GuestRuntimeStop&) {
            RuntimeTraceThreadTerminated(threadId);
            std::cout << "GUEST_THREAD_STOP id=" << threadId << '\n';
        } catch (const std::exception& error) {
            PublishGuestWorkerBlocker(threadId, object.guestObject, error);
        }
    });
    return {object.handle, threadId, object.guestObject};
}

bool ResumeGuestThread(uint32_t guestObject, uint32_t* previousSuspendCount) {
    std::shared_ptr<GuestThreadStartGate> gate;
    {
        std::lock_guard<std::mutex> lock(thread_start_gates_mutex);
        const auto it = thread_start_gates.find(guestObject);
        if (it == thread_start_gates.end()) return false;
        gate = it->second;
    }
    uint32_t previous{};
    bool becameRunnable = false;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        previous = gate->suspendCount;
        if (gate->suspendCount) {
            --gate->suspendCount;
            becameRunnable = gate->suspendCount == 0;
        }
    }
    if (previousSuspendCount) *previousSuspendCount = previous;
    if (becameRunnable) {
        GetGuestObjects().SetThreadState(guestObject, GuestThreadState::Running);
        gate->wake.notify_all();
    }
    return true;
}

bool SuspendGuestThread(uint32_t guestObject, uint32_t* previousSuspendCount) {
    std::shared_ptr<GuestThreadStartGate> gate;
    {
        std::lock_guard<std::mutex> gateLock(thread_start_gates_mutex);
        const auto it = thread_start_gates.find(guestObject);
        if (it == thread_start_gates.end()) return false;
        gate = it->second;
    }

    const bool suspendingSelf = guestObject == current_guest_object;
    if (!suspendingSelf) {
        // The generated dispatcher has no asynchronous host-thread preemption
        // point. Increasing an already-suspended thread's count is exact, but
        // claiming that an independently running thread stopped would be
        // false. Reject that not-yet-reached case until cooperative boundary
        // checks are required by title evidence.
        GuestThreadInfo threadInfo{};
        if (!GetGuestObjects().GetThreadInfo(guestObject, &threadInfo) ||
            threadInfo.state != GuestThreadState::Suspended) {
            return false;
        }
    }

    uint32_t previous{};
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        previous = gate->suspendCount;
        ++gate->suspendCount;
    }
    if (previousSuspendCount) *previousSuspendCount = previous;
    GetGuestObjects().SetThreadState(guestObject, GuestThreadState::Suspended);

    if (!suspendingSelf) return true;

    // NtSuspendThread is used by each WMV worker on its own handle. Park the
    // host worker on the same count/gate used by NtResumeThread so only a real
    // guest resume makes it runnable again.
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->wake.wait(lock, [&] {
            return gate->suspendCount == 0 || GuestRuntimeStopRequested();
        });
    }
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop();
    GetGuestObjects().SetThreadState(guestObject, GuestThreadState::Running);
    return true;
}

void MarkCurrentGuestThreadTerminated(uint8_t* base, uint32_t exitCode) {
    PublishGuestThreadTermination(base, current_guest_object, exitCode);
}

void QueueGuestApc(uint32_t routine, uint32_t context, uint32_t argument1, uint32_t argument2) {
    if (!routine) return;
    pending_apcs.push_back({routine, context, argument1, argument2});
    const uint64_t ordinal = queued_apc_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal <= 64 || (ordinal & 0xFFu) == 0) {
        std::cout << "APC_QUEUED thread=" << CurrentGuestThreadId() << " routine=0x"
                  << std::hex << routine << " context=0x" << context << " arg1=0x"
                  << argument1 << " arg2=0x" << argument2 << std::dec
                  << " ordinal=" << ordinal << '\n';
    }
}

bool HasPendingGuestApcs() { return !pending_apcs.empty(); }

bool DeliverGuestApcs(PPCContext& context, uint8_t* base) {
    bool delivered = false;
    while (!pending_apcs.empty()) {
        const GuestApc apc = pending_apcs.front();
        pending_apcs.erase(pending_apcs.begin());
        if (!RuntimeGeneratedAddressInRange(apc.routine))
            throw std::runtime_error("queued guest APC target is outside generated code");
        PPCFunc* routine = PPC_LOOKUP_FUNC(base, apc.routine);
        if (!routine) throw std::runtime_error("queued guest APC has no callable routine");
        // Xenia queues normal APCs with (normal_context, system_arg1,
        // system_arg2). Dispatch at a guest scheduling boundary, not from the
        // NtReadFile import itself, so the import's result registers survive.
        // An APC interrupts and resumes the current guest context; its normal
        // routine receives volatile argument registers, but those register
        // changes must not leak into the interrupted title call.
        const PPCContext interruptedContext = context;
        context.r3.u64 = apc.context;
        context.r4.u64 = apc.argument1;
        context.r5.u64 = apc.argument2;
        const uint64_t ordinal =
            delivered_apc_trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal <= 64 || (ordinal & 0xFFu) == 0) {
            std::cout << "APC_DELIVER thread=" << CurrentGuestThreadId() << " routine=0x"
                      << std::hex << apc.routine << std::dec << " ordinal=" << ordinal << '\n';
        }
        routine(context, base);
        context = interruptedContext;
        delivered = true;
    }
    return delivered;
}

void JoinGuestThreads() {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(threads_mutex);
        to_join.swap(threads);
    }
    for (auto& thread : to_join) {
        if (thread.joinable()) thread.join();
    }
}
