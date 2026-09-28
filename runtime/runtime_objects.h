#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

enum class GuestObjectType : uint32_t { Event, Semaphore, Thread, NotifyListener, Enumerator, Timer };
enum class GuestThreadState : uint32_t { Running, Suspended, Blocked, Waiting, Terminated };

struct GuestObjectInfo {
    GuestObjectType type;
    uint32_t handle;
    uint32_t guestObject;
    uint32_t referenceCount;
    uint32_t handleCount;
    bool closed;
};

struct GuestThreadInfo {
    uint32_t threadId;
    uint32_t stackTop;
    uint32_t stackSize;
    int32_t priority;
    uint32_t affinity;
    bool fpuExceptionsEnabled;
    uint32_t exitCode;
    GuestThreadState state;
};

struct GuestObjectCreation {
    uint32_t handle;
    uint32_t guestObject;
};

using GuestEnumeratorWriter = std::function<uint32_t(
    uint32_t firstItem, uint32_t itemCount, uint32_t destinationGuestAddress,
    uint8_t* destination, uint32_t destinationBytes, uint32_t* itemsWritten)>;

class GuestObjects {
public:
    static constexpr uint32_t kThreadObjectTypeToken = 0x1B000000;

    GuestObjectCreation CreateEvent(bool manualReset, bool initialState);
    GuestObjectCreation CreateSemaphore(uint32_t initialCount, uint32_t limit);
    GuestObjectCreation CreateThread(uint32_t threadId, uint32_t stackTop, uint32_t stackSize);
    GuestObjectCreation CreateNotifyListener(uint64_t mask, uint32_t maxVersion);
    GuestObjectCreation CreateEnumerator(uint32_t itemsPerEnumerate, uint32_t itemSize,
                                         std::vector<uint8_t> items = {});
    GuestObjectCreation CreateEnumerator(uint32_t itemsPerEnumerate, uint32_t itemSize,
                                         uint32_t itemCount,
                                         GuestEnumeratorWriter writer);
    // NT timers: a notification timer (manual reset) stays signalled once due;
    // a synchronization timer releases one waiter per expiry. dueTime is the
    // NT LARGE_INTEGER (negative: relative, 100 ns units; otherwise absolute
    // system time); periodMs 0 is a one-shot timer. Setting a timer clears
    // its signal state.
    GuestObjectCreation CreateTimer(bool manualReset);
    uint32_t SetTimer(uint32_t handle, int64_t dueTime, uint32_t periodMs, bool* previousState);
    uint32_t CancelTimer(uint32_t handle, bool* currentState);
    // NtDuplicateObject: a second handle to the same object (-2 is the
    // current thread), optionally closing the source handle.
    uint32_t DuplicateHandle(uint32_t handle, uint32_t currentThreadObject, bool closeSource,
                             uint32_t* newHandle);

    // A nonzero expected type is checked against the observed The Darkness
    // thread type token. Other type tokens are deliberately rejected until
    // they are reached and evidenced.
    // -2 is the documented current-thread pseudo-handle in the title's
    // observed thread path. Resolve it through the caller's guest identity;
    // never synthesize a new handle or object for that case.
    uint32_t ReferenceByHandle(uint32_t handle, uint32_t expectedTypeToken,
                               uint32_t currentThreadObject, uint32_t* guestObject);
    uint32_t WaitForSingleObject(uint32_t handle, bool hasTimeout, int64_t timeout);
    uint32_t ReleaseSemaphore(uint32_t handle, uint32_t releaseCount, uint32_t* previousCount);
    bool DequeueNotification(uint32_t handle, uint32_t matchId,
                             uint32_t* notificationId, uint32_t* parameter);
    uint32_t BroadcastNotification(uint32_t notificationId, uint32_t parameter);
    uint32_t Enumerate(uint32_t handle, uint32_t destinationGuestAddress,
                       uint8_t* destination, uint32_t destinationBytes,
                       uint32_t* itemsWritten);
    uint32_t ReferenceThreadById(uint32_t threadId, uint32_t* guestObject);
    uint32_t OpenObjectByGuestPointer(uint32_t guestObject, uint32_t* handle);
    bool DereferenceGuestObject(uint32_t guestObject);
    bool Close(uint32_t handle);

    bool SetEvent(uint32_t handle, uint32_t* previousState);
    bool ClearEvent(uint32_t handle);
    bool SetThreadAffinity(uint32_t guestObject, uint32_t affinity, uint32_t* previousAffinity);
    bool SetThreadPriority(uint32_t guestObject, int32_t priority, int32_t* previousPriority);
    bool SetThreadFpuExceptionsEnabled(uint32_t guestObject, bool enabled);
    bool SetThreadState(uint32_t guestObject, GuestThreadState state);
    bool TerminateThread(uint32_t guestObject, uint32_t exitCode);
    bool GetThreadInfo(uint32_t guestObject, GuestThreadInfo* info) const;
    bool GetObjectInfo(uint32_t handle, GuestObjectInfo* info) const;

    // Runtime teardown only: wake every kernel-object waiter so each host
    // worker can observe the global stop request at its guest boundary.
    void WakeAllWaiters();

    void ResetForTests(uint32_t firstGuestObject = 0x7D000000);

private:
    struct Object;
    GuestObjectCreation CreateObject(std::shared_ptr<Object> object);
    void CollectClosedObject(const std::shared_ptr<Object>& object);

    mutable std::mutex mutex_;
    uint32_t nextHandle_ = 1;
    uint32_t nextGuestObject_ = 0x7D000000;
    std::unordered_map<uint32_t, std::shared_ptr<Object>> byHandle_;
    std::unordered_map<uint32_t, std::shared_ptr<Object>> byGuestObject_;
};

GuestObjects& GetGuestObjects();
