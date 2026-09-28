#include "runtime_sync.h"

#include "runtime_guest_bulk_write.h"
#include "ppc_recomp_shared.h"
#include "runtime_threads.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t kLockCountOffset = 16;
constexpr uint32_t kRecursionOffset = 20;
constexpr uint32_t kOwnerOffset = 24;
}

struct GuestCriticalSections::CriticalSection {
    std::mutex mutex;
    std::condition_variable wake;
    uint32_t owner{};
    uint32_t recursion{};
    uint32_t waiters{};
};

std::shared_ptr<GuestCriticalSections::CriticalSection> GuestCriticalSections::FindOrCreate(uint32_t address) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& section = sections_[address];
    if (!section) section = std::make_shared<CriticalSection>();
    return section;
}

void GuestCriticalSections::Initialize(uint8_t* base, uint32_t address) {
    auto section = FindOrCreate(address);
    std::lock_guard<std::mutex> lock(section->mutex);
    section->owner = 0;
    section->recursion = 0;
    section->waiters = 0;
    RuntimeGeneratedMemset(base, base + address, 0, 28, __FILE__, __LINE__);
    PPC_STORE_U8(address, 1);  // EventSynchronizationObject.
    PPC_STORE_U32(address + kLockCountOffset, 0xFFFFFFFF);
}

void GuestCriticalSections::Enter(uint8_t* base, uint32_t address, uint32_t guestThreadObject) {
    if (!guestThreadObject) throw std::runtime_error("critical section entered without guest thread identity");
    auto section = FindOrCreate(address);
    std::unique_lock<std::mutex> lock(section->mutex);
    if (section->owner == guestThreadObject) {
        ++section->recursion;
    } else {
        while (section->owner) {
            ++section->waiters;
            section->wake.wait(lock, [&] { return section->owner == 0; });
            --section->waiters;
        }
        section->owner = guestThreadObject;
        section->recursion = 1;
    }
    PPC_STORE_U32(address + kLockCountOffset, section->recursion - 1);
    PPC_STORE_U32(address + kRecursionOffset, section->recursion);
    PPC_STORE_U32(address + kOwnerOffset, section->owner);
}

bool GuestCriticalSections::TryEnter(uint8_t* base, uint32_t address,
                                     uint32_t guestThreadObject) {
    if (!guestThreadObject)
        throw std::runtime_error("critical section try-entered without guest thread identity");
    auto section = FindOrCreate(address);
    std::lock_guard<std::mutex> lock(section->mutex);
    if (section->owner && section->owner != guestThreadObject) return false;
    if (section->owner == guestThreadObject) {
        ++section->recursion;
    } else {
        section->owner = guestThreadObject;
        section->recursion = 1;
    }
    PPC_STORE_U32(address + kLockCountOffset, section->recursion - 1);
    PPC_STORE_U32(address + kRecursionOffset, section->recursion);
    PPC_STORE_U32(address + kOwnerOffset, section->owner);
    return true;
}

void GuestCriticalSections::Leave(uint8_t* base, uint32_t address, uint32_t guestThreadObject) {
    auto section = FindOrCreate(address);
    std::unique_lock<std::mutex> lock(section->mutex);
    if (!section->recursion || section->owner != guestThreadObject) {
        throw std::runtime_error("critical section leave by non-owner");
    }
    --section->recursion;
    if (!section->recursion) {
        section->owner = 0;
        PPC_STORE_U32(address + kLockCountOffset, 0xFFFFFFFF);
        PPC_STORE_U32(address + kOwnerOffset, 0);
        section->wake.notify_one();
    } else {
        PPC_STORE_U32(address + kLockCountOffset, section->recursion - 1);
    }
    PPC_STORE_U32(address + kRecursionOffset, section->recursion);
}

void GuestCriticalSections::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    sections_.clear();
}

GuestCriticalSections& GetGuestCriticalSections() {
    static GuestCriticalSections sections;
    return sections;
}

namespace {
constexpr uint32_t kDispatcherHeaderSize = 0x10;
constexpr uint32_t kDispatcherSignalStateOffset = 0x04;
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusTimeout = 0x00000102;
}

struct GuestDispatcherEvents::Event {
    std::mutex mutex;
    std::condition_variable wake;
    uint32_t type{};
    bool signalled{};
};

std::shared_ptr<GuestDispatcherEvents::Event> GuestDispatcherEvents::FindOrCreate(
    uint8_t* base, uint32_t address) {
    if (!address || address > UINT32_MAX - kDispatcherHeaderSize)
        throw std::runtime_error("kernel dispatcher event has an invalid guest address");
    std::lock_guard<std::mutex> lock(mutex_);
    auto& event = events_[address];
    if (!event) {
        const uint32_t type = PPC_LOAD_U8(address);
        // Xbox dispatcher object types 0 and 1 are notification (manual-reset)
        // and synchronization (auto-reset) events respectively.
        if (type > 1) throw std::runtime_error("Ke event pointer is not an X_KEVENT");
        event = std::make_shared<Event>();
        event->type = type;
        event->signalled = PPC_LOAD_U32(address + kDispatcherSignalStateOffset) != 0;
    }
    return event;
}

void GuestDispatcherEvents::Initialize(uint8_t* base, uint32_t address,
                                       uint32_t type, bool initialState) {
    if (!address || address > UINT32_MAX - kDispatcherHeaderSize || type > 1)
        throw std::runtime_error("KeInitializeEvent reached an invalid event contract");
    std::shared_ptr<Event> event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event = std::make_shared<Event>();
        event->type = type;
        event->signalled = initialState;
        events_[address] = event;
    }
    RuntimeGeneratedMemset(base, base + address, 0, kDispatcherHeaderSize, __FILE__, __LINE__);
    PPC_STORE_U8(address, static_cast<uint8_t>(type));
    PPC_STORE_U32(address + kDispatcherSignalStateOffset, initialState ? 1u : 0u);
    GetGuestDispatcherWaitCoordinator().Notify();
}

int32_t GuestDispatcherEvents::Set(uint8_t* base, uint32_t address) {
    auto event = FindOrCreate(base, address);
    std::lock_guard<std::mutex> lock(event->mutex);
    const int32_t previous = event->signalled ? 1 : 0;
    event->signalled = true;
    PPC_STORE_U32(address + kDispatcherSignalStateOffset, 1);
    if (event->type == 0) event->wake.notify_all();
    else event->wake.notify_one();
    GetGuestDispatcherWaitCoordinator().Notify();
    return previous;
}

int32_t GuestDispatcherEvents::Reset(uint8_t* base, uint32_t address) {
    auto event = FindOrCreate(base, address);
    std::lock_guard<std::mutex> lock(event->mutex);
    const int32_t previous = event->signalled ? 1 : 0;
    event->signalled = false;
    PPC_STORE_U32(address + kDispatcherSignalStateOffset, 0);
    return previous;
}

uint32_t GuestDispatcherEvents::Wait(uint8_t* base, uint32_t address,
                                     bool hasTimeout, int64_t timeout) {
    auto event = FindOrCreate(base, address);
    std::unique_lock<std::mutex> lock(event->mutex);
    const auto consumeSignal = [&] {
        if (!event->signalled) return false;
        if (event->type == 1) {
            event->signalled = false;
            PPC_STORE_U32(address + kDispatcherSignalStateOffset, 0);
        }
        return true;
    };
    if (consumeSignal()) return kStatusSuccess;
    const auto ready = [&] { return GuestRuntimeStopRequested() || event->signalled; };
    if (!hasTimeout) {
        event->wake.wait(lock, ready);
    } else {
        using namespace std::chrono;
        int64_t ticks = timeout < 0 ? (timeout == INT64_MIN ? INT64_MAX : -timeout) : 0;
        if (timeout > 0) {
            constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
            const int64_t now =
                duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 +
                kWindowsToUnixEpochTicks;
            ticks = timeout > now ? timeout - now : 0;
        }
        const auto maximumTicks = duration_cast<nanoseconds>(hours(24)).count() / 100;
        const auto boundedTicks = std::min<int64_t>(ticks, maximumTicks);
        if (!event->wake.wait_for(lock, nanoseconds(boundedTicks * 100), ready))
            return kStatusTimeout;
    }
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
    return consumeSignal() ? kStatusSuccess : kStatusTimeout;
}

bool GuestDispatcherEvents::TryWait(uint8_t* base, uint32_t address) {
    auto event = FindOrCreate(base, address);
    std::lock_guard<std::mutex> lock(event->mutex);
    if (!event->signalled) return false;
    if (event->type == 1) {
        event->signalled = false;
        PPC_STORE_U32(address + kDispatcherSignalStateOffset, 0);
        GetGuestDispatcherWaitCoordinator().Notify();
    }
    return true;
}

void GuestDispatcherEvents::WakeAllWaiters() {
    std::vector<std::shared_ptr<Event>> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.reserve(events_.size());
        for (const auto& [_, event] : events_) events.push_back(event);
    }
    for (const auto& event : events) event->wake.notify_all();
}

void GuestDispatcherEvents::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
}

GuestDispatcherEvents& GetGuestDispatcherEvents() {
    static GuestDispatcherEvents events;
    return events;
}

namespace {
constexpr uint32_t kSemaphoreDispatcherType = 5;
constexpr uint32_t kGuestSemaphoreSize = 0x14;
constexpr uint32_t kGuestSemaphoreLimitOffset = 0x10;
}

struct GuestDispatcherSemaphores::Semaphore {
    std::mutex mutex;
    std::condition_variable wake;
    int32_t count{};
    int32_t limit{};
};

std::shared_ptr<GuestDispatcherSemaphores::Semaphore>
GuestDispatcherSemaphores::FindOrCreate(uint8_t* base, uint32_t address) {
    if (!address || address > UINT32_MAX - kGuestSemaphoreSize)
        throw std::runtime_error("kernel dispatcher semaphore has an invalid guest address");
    std::lock_guard<std::mutex> lock(mutex_);
    auto& semaphore = semaphores_[address];
    if (!semaphore) {
        if (PPC_LOAD_U8(address) != kSemaphoreDispatcherType)
            throw std::runtime_error("Ke semaphore pointer is not an X_KSEMAPHORE");
        const uint32_t count = PPC_LOAD_U32(address + kDispatcherSignalStateOffset);
        const uint32_t limit = PPC_LOAD_U32(address + kGuestSemaphoreLimitOffset);
        if (!limit || limit > INT32_MAX || count > limit)
            throw std::runtime_error("X_KSEMAPHORE has an invalid count or limit");
        semaphore = std::make_shared<Semaphore>();
        semaphore->count = static_cast<int32_t>(count);
        semaphore->limit = static_cast<int32_t>(limit);
    }
    return semaphore;
}

void GuestDispatcherSemaphores::Initialize(uint8_t* base, uint32_t address,
                                           uint32_t count, uint32_t limit) {
    if (!address || address > UINT32_MAX - kGuestSemaphoreSize || !limit ||
        limit > INT32_MAX || count > limit) {
        throw std::runtime_error("KeInitializeSemaphore reached an invalid semaphore contract");
    }
    auto semaphore = std::make_shared<Semaphore>();
    semaphore->count = static_cast<int32_t>(count);
    semaphore->limit = static_cast<int32_t>(limit);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        semaphores_[address] = semaphore;
    }
    RuntimeGeneratedMemset(base, base + address, 0, kGuestSemaphoreSize, __FILE__, __LINE__);
    PPC_STORE_U8(address, kSemaphoreDispatcherType);
    PPC_STORE_U32(address + kDispatcherSignalStateOffset, count);
    PPC_STORE_U32(address + kGuestSemaphoreLimitOffset, limit);
    GetGuestDispatcherWaitCoordinator().Notify();
}

int32_t GuestDispatcherSemaphores::Release(uint8_t* base, uint32_t address,
                                           uint32_t adjustment) {
    auto semaphore = FindOrCreate(base, address);
    std::lock_guard<std::mutex> lock(semaphore->mutex);
    if (!adjustment || adjustment > static_cast<uint32_t>(semaphore->limit - semaphore->count))
        throw std::runtime_error("KeReleaseSemaphore would exceed the semaphore limit");
    const int32_t previous = semaphore->count;
    semaphore->count += static_cast<int32_t>(adjustment);
    PPC_STORE_U32(address + kDispatcherSignalStateOffset,
                  static_cast<uint32_t>(semaphore->count));
    for (uint32_t index = 0; index < adjustment; ++index) semaphore->wake.notify_one();
    GetGuestDispatcherWaitCoordinator().Notify();
    return previous;
}

uint32_t GuestDispatcherSemaphores::Wait(uint8_t* base, uint32_t address,
                                         bool hasTimeout, int64_t timeout) {
    auto semaphore = FindOrCreate(base, address);
    std::unique_lock<std::mutex> lock(semaphore->mutex);
    const auto consume = [&] {
        if (semaphore->count <= 0) return false;
        --semaphore->count;
        PPC_STORE_U32(address + kDispatcherSignalStateOffset,
                      static_cast<uint32_t>(semaphore->count));
        return true;
    };
    if (consume()) return kStatusSuccess;
    const auto ready = [&] { return GuestRuntimeStopRequested() || semaphore->count > 0; };
    if (!hasTimeout) {
        semaphore->wake.wait(lock, ready);
    } else {
        using namespace std::chrono;
        int64_t ticks = timeout < 0 ? (timeout == INT64_MIN ? INT64_MAX : -timeout) : 0;
        if (timeout > 0) {
            constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
            const int64_t now =
                duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 +
                kWindowsToUnixEpochTicks;
            ticks = timeout > now ? timeout - now : 0;
        }
        const auto maximumTicks = duration_cast<nanoseconds>(hours(24)).count() / 100;
        const auto boundedTicks = std::min<int64_t>(ticks, maximumTicks);
        if (!semaphore->wake.wait_for(lock, nanoseconds(boundedTicks * 100), ready))
            return kStatusTimeout;
    }
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
    return consume() ? kStatusSuccess : kStatusTimeout;
}

bool GuestDispatcherSemaphores::TryWait(uint8_t* base, uint32_t address) {
    auto semaphore = FindOrCreate(base, address);
    std::lock_guard<std::mutex> lock(semaphore->mutex);
    if (semaphore->count <= 0) return false;
    --semaphore->count;
    PPC_STORE_U32(address + kDispatcherSignalStateOffset,
                  static_cast<uint32_t>(semaphore->count));
    GetGuestDispatcherWaitCoordinator().Notify();
    return true;
}

void GuestDispatcherSemaphores::WakeAllWaiters() {
    std::vector<std::shared_ptr<Semaphore>> semaphores;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        semaphores.reserve(semaphores_.size());
        for (const auto& [_, semaphore] : semaphores_) semaphores.push_back(semaphore);
    }
    for (const auto& semaphore : semaphores) semaphore->wake.notify_all();
}

void GuestDispatcherSemaphores::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    semaphores_.clear();
}

GuestDispatcherSemaphores& GetGuestDispatcherSemaphores() {
    static GuestDispatcherSemaphores semaphores;
    return semaphores;
}

uint64_t GuestDispatcherWaitCoordinator::Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

void GuestDispatcherWaitCoordinator::Notify() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
    }
    wake_.notify_all();
}

bool GuestDispatcherWaitCoordinator::WaitForChange(
    uint64_t generation, bool hasDeadline,
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto changed = [&] {
        return generation_ != generation || GuestRuntimeStopRequested();
    };
    if (changed()) return true;
    if (!hasDeadline) {
        wake_.wait(lock, changed);
        return true;
    }
    return wake_.wait_until(lock, deadline, changed);
}

void GuestDispatcherWaitCoordinator::WakeAll() { wake_.notify_all(); }

GuestDispatcherWaitCoordinator& GetGuestDispatcherWaitCoordinator() {
    static GuestDispatcherWaitCoordinator coordinator;
    return coordinator;
}
