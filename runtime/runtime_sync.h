#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

class GuestCriticalSections {
public:
    void Initialize(uint8_t* base, uint32_t address);
    void Enter(uint8_t* base, uint32_t address, uint32_t guestThreadObject);
    bool TryEnter(uint8_t* base, uint32_t address, uint32_t guestThreadObject);
    void Leave(uint8_t* base, uint32_t address, uint32_t guestThreadObject);
    void ResetForTests();

private:
    struct CriticalSection;
    std::shared_ptr<CriticalSection> FindOrCreate(uint32_t address);
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<CriticalSection>> sections_;
};

GuestCriticalSections& GetGuestCriticalSections();

// Kernel dispatcher events embedded directly in title-owned guest objects.
// These are addressed by X_KEVENT pointers, not by the handle table used by
// NtCreateEvent/NtWaitForSingleObjectEx.
class GuestDispatcherEvents {
public:
    void Initialize(uint8_t* base, uint32_t address, uint32_t type, bool initialState);
    int32_t Set(uint8_t* base, uint32_t address);
    int32_t Reset(uint8_t* base, uint32_t address);
    uint32_t Wait(uint8_t* base, uint32_t address, bool hasTimeout, int64_t timeout);
    bool TryWait(uint8_t* base, uint32_t address);
    void WakeAllWaiters();
    void ResetForTests();

private:
    struct Event;
    std::shared_ptr<Event> FindOrCreate(uint8_t* base, uint32_t address);
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Event>> events_;
};

GuestDispatcherEvents& GetGuestDispatcherEvents();

// Kernel dispatcher semaphores embedded directly in title-owned guest memory.
// The guest-visible count remains in X_KSEMAPHORE::Header.SignalState while
// host condition variables provide only the blocking/wakeup mechanism.
class GuestDispatcherSemaphores {
public:
    void Initialize(uint8_t* base, uint32_t address, uint32_t count, uint32_t limit);
    int32_t Release(uint8_t* base, uint32_t address, uint32_t adjustment);
    uint32_t Wait(uint8_t* base, uint32_t address, bool hasTimeout, int64_t timeout);
    bool TryWait(uint8_t* base, uint32_t address);
    void WakeAllWaiters();
    void ResetForTests();

private:
    struct Semaphore;
    std::shared_ptr<Semaphore> FindOrCreate(uint8_t* base, uint32_t address);
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Semaphore>> semaphores_;
};

GuestDispatcherSemaphores& GetGuestDispatcherSemaphores();

// A generation-based wake coordinator lets KeWaitForMultipleObjects block on
// heterogeneous embedded dispatcher objects without polling or losing a
// signal between its nonblocking scans and host wait.
class GuestDispatcherWaitCoordinator {
public:
    uint64_t Generation() const;
    void Notify();
    bool WaitForChange(uint64_t generation, bool hasDeadline,
                       std::chrono::steady_clock::time_point deadline);
    void WakeAll();

private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    uint64_t generation_{};
};

GuestDispatcherWaitCoordinator& GetGuestDispatcherWaitCoordinator();
