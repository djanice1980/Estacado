#include "runtime_objects.h"
#include "runtime_guest_write_completion.h"
#include "runtime_threads.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <stdexcept>

namespace {
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
constexpr uint32_t kStatusTimeout = 0x00000102;
constexpr uint32_t kXErrorInvalidHandle = 0x00000006;
constexpr uint32_t kXErrorNoMoreFiles = 0x00000012;
constexpr uint32_t kXErrorInvalidParameter = 0x00000057;
constexpr uint32_t kXErrorInsufficientBuffer = 0x0000007A;

// NT time: a negative value is relative (100 ns units), any other value is an
// absolute system time (100 ns units since 1601).
std::chrono::steady_clock::time_point SteadyTimeFromNt(int64_t ntTime) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    int64_t ticks = 0;
    if (ntTime < 0) {
        ticks = ntTime == INT64_MIN ? INT64_MAX : -ntTime;
    } else {
        constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
        const int64_t systemNow =
            duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 +
            kWindowsToUnixEpochTicks;
        ticks = ntTime > systemNow ? ntTime - systemNow : 0;
    }
    // Clamp far-future times (a day is far beyond any title timer).
    constexpr int64_t kMaxTicks = 24LL * 3600 * 10'000'000LL;
    return now + duration_cast<steady_clock::duration>(
                     nanoseconds(std::min(ticks, kMaxTicks) * 100));
}
}

struct GuestObjects::Object {
    GuestObjectInfo info{};
    bool manualReset{};
    bool signalled{};
    uint32_t semaphoreCount{};
    uint32_t semaphoreLimit{};
    GuestThreadInfo thread{};
    uint64_t notificationMask{};
    uint32_t notificationMaxVersion{};
    std::vector<std::pair<uint32_t, uint32_t>> notifications;
    uint32_t enumeratorItemsPerCall{};
    uint32_t enumeratorItemSize{};
    uint32_t enumeratorNextItem{};
    uint32_t enumeratorItemCount{};
    std::vector<uint8_t> enumeratorItems;
    GuestEnumeratorWriter enumeratorWriter;
    bool timerArmed{};
    std::chrono::steady_clock::time_point timerDue{};
    std::chrono::milliseconds timerPeriod{};
    std::mutex waitMutex;
    std::condition_variable waitWake;

    // Timer expiry at `now` (waitMutex held): a due timer becomes signalled;
    // a periodic one moves to its next expiry (missed expiries coalesce, as
    // the NT signal state is a flag), a one-shot one disarms.
    void AdvanceTimer(std::chrono::steady_clock::time_point now) {
        if (!timerArmed || now < timerDue) return;
        signalled = true;
        if (timerPeriod.count() > 0) {
            const auto late = now - timerDue;
            timerDue += timerPeriod * (late / timerPeriod + 1);
        } else {
            timerArmed = false;
        }
    }
};

GuestObjectCreation GuestObjects::CreateObject(std::shared_ptr<Object> object) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!nextHandle_ || byHandle_.count(nextHandle_)) ++nextHandle_;
    const uint32_t handle = nextHandle_++;
    const uint32_t guestObject = nextGuestObject_;
    // The title's verified GetExitCodeThread wrapper reads a referenced
    // KTHREAD-like object through +0x04 and +0x140. Reserve the complete
    // observed span so a later kernel object can never overlap those fields.
    const uint32_t objectBytes = object->info.type == GuestObjectType::Thread ? 0x200 : 0x20;
    // 0x7D0FF000-0x7D0FFFFF is reserved for runtime-owned callback wrappers;
    // PCR/TEB storage starts at 0x7D100000. Fail explicitly instead of ever
    // allowing a high object count to alias either private runtime region.
    if (guestObject >= 0x7D0FF000u || objectBytes > 0x7D0FF000u - guestObject) {
        throw std::runtime_error("guest kernel object private range exhausted");
    }
    nextGuestObject_ += objectBytes;
    object->info.handle = handle;
    object->info.guestObject = guestObject;
    object->info.handleCount = 1;
    object->info.closed = false;
    byHandle_.emplace(handle, object);
    byGuestObject_.emplace(guestObject, object);
    return {handle, guestObject};
}

GuestObjectCreation GuestObjects::CreateEvent(bool manualReset, bool initialState) {
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Event;
    object->manualReset = manualReset;
    object->signalled = initialState;
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateSemaphore(uint32_t initialCount, uint32_t limit) {
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Semaphore;
    object->semaphoreCount = initialCount;
    object->semaphoreLimit = limit;
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateThread(uint32_t threadId, uint32_t stackTop, uint32_t stackSize) {
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Thread;
    object->thread = {threadId, stackTop, stackSize, 0, 1, false, 0, GuestThreadState::Running};
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateNotifyListener(uint64_t mask, uint32_t maxVersion) {
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::NotifyListener;
    object->manualReset = true;
    object->notificationMask = mask;
    object->notificationMaxVersion = maxVersion;
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateEnumerator(uint32_t itemsPerEnumerate,
                                                    uint32_t itemSize,
                                                    std::vector<uint8_t> items) {
    if (!itemsPerEnumerate || !itemSize || items.size() % itemSize) {
        throw std::invalid_argument("invalid guest enumerator shape");
    }
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Enumerator;
    object->enumeratorItemsPerCall = itemsPerEnumerate;
    object->enumeratorItemSize = itemSize;
    object->enumeratorItemCount = static_cast<uint32_t>(items.size() / itemSize);
    object->enumeratorItems = std::move(items);
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateEnumerator(uint32_t itemsPerEnumerate,
                                                    uint32_t itemSize,
                                                    uint32_t itemCount,
                                                    GuestEnumeratorWriter writer) {
    if (!itemsPerEnumerate || !itemSize || !writer) {
        throw std::invalid_argument("invalid custom guest enumerator shape");
    }
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Enumerator;
    object->enumeratorItemsPerCall = itemsPerEnumerate;
    object->enumeratorItemSize = itemSize;
    object->enumeratorItemCount = itemCount;
    object->enumeratorWriter = std::move(writer);
    return CreateObject(std::move(object));
}

GuestObjectCreation GuestObjects::CreateTimer(bool manualReset) {
    auto object = std::make_shared<Object>();
    object->info.type = GuestObjectType::Timer;
    object->manualReset = manualReset;
    return CreateObject(std::move(object));
}

uint32_t GuestObjects::SetTimer(uint32_t handle, int64_t dueTime, uint32_t periodMs,
                                bool* previousState) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return kStatusInvalidHandle;
    const auto& object = it->second;
    if (object->info.type != GuestObjectType::Timer) return kStatusObjectTypeMismatch;
    std::lock_guard<std::mutex> waitLock(object->waitMutex);
    object->AdvanceTimer(std::chrono::steady_clock::now());
    if (previousState) *previousState = object->signalled;
    object->signalled = false;
    object->timerDue = SteadyTimeFromNt(dueTime);
    object->timerPeriod = std::chrono::milliseconds(periodMs);
    object->timerArmed = true;
    object->waitWake.notify_all();
    return kStatusSuccess;
}

uint32_t GuestObjects::CancelTimer(uint32_t handle, bool* currentState) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return kStatusInvalidHandle;
    const auto& object = it->second;
    if (object->info.type != GuestObjectType::Timer) return kStatusObjectTypeMismatch;
    std::lock_guard<std::mutex> waitLock(object->waitMutex);
    object->AdvanceTimer(std::chrono::steady_clock::now());
    if (currentState) *currentState = object->signalled;
    object->timerArmed = false;
    object->waitWake.notify_all();
    return kStatusSuccess;
}

uint32_t GuestObjects::DuplicateHandle(uint32_t handle, uint32_t currentThreadObject,
                                       bool closeSource, uint32_t* newHandle) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<Object> object;
    const bool currentThread = handle == 0xFFFFFFFEu;
    if (currentThread) {
        const auto current = byGuestObject_.find(currentThreadObject);
        if (current == byGuestObject_.end() || current->second->info.type != GuestObjectType::Thread) {
            return kStatusInvalidHandle;
        }
        object = current->second;
    } else {
        const auto it = byHandle_.find(handle);
        if (it == byHandle_.end()) return kStatusInvalidHandle;
        object = it->second;
    }
    while (!nextHandle_ || byHandle_.count(nextHandle_)) ++nextHandle_;
    const uint32_t duplicate = nextHandle_++;
    byHandle_.emplace(duplicate, object);
    ++object->info.handleCount;
    object->info.closed = false;
    if (closeSource && !currentThread) {
        byHandle_.erase(handle);
        --object->info.handleCount;
    }
    if (newHandle) *newHandle = duplicate;
    return kStatusSuccess;
}

uint32_t GuestObjects::ReferenceByHandle(uint32_t handle, uint32_t expectedTypeToken,
                                         uint32_t currentThreadObject, uint32_t* guestObject) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<Object> object;
    if (handle == 0xFFFFFFFEu) {
        const auto current = byGuestObject_.find(currentThreadObject);
        if (current == byGuestObject_.end() || current->second->info.type != GuestObjectType::Thread) {
            return kStatusInvalidHandle;
        }
        object = current->second;
    } else {
        const auto it = byHandle_.find(handle);
        if (it == byHandle_.end()) return kStatusInvalidHandle;
        object = it->second;
    }
    if (expectedTypeToken &&
        !(object->info.type == GuestObjectType::Thread && expectedTypeToken == kThreadObjectTypeToken)) {
        return kStatusObjectTypeMismatch;
    }
    ++object->info.referenceCount;
    if (guestObject) *guestObject = object->info.guestObject;
    return kStatusSuccess;
}

uint32_t GuestObjects::ReferenceThreadById(uint32_t threadId, uint32_t* guestObject) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, object] : byGuestObject_) {
        if (object->info.type == GuestObjectType::Thread && object->thread.threadId == threadId) {
            ++object->info.referenceCount;
            if (guestObject) *guestObject = object->info.guestObject;
            return kStatusSuccess;
        }
    }
    return 0xC0000225; // STATUS_NOT_FOUND, verified by the object lookup contract.
}

void GuestObjects::CollectClosedObject(const std::shared_ptr<Object>& object) {
    if (!object->info.closed || object->info.referenceCount || object->info.handleCount) return;
    byGuestObject_.erase(object->info.guestObject);
}

bool GuestObjects::DereferenceGuestObject(uint32_t guestObject) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || !it->second->info.referenceCount) return false;
    const auto& object = it->second;
    --object->info.referenceCount;
    CollectClosedObject(object);
    return true;
}

bool GuestObjects::Close(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return false;
    const auto object = it->second;
    byHandle_.erase(it);
    --object->info.handleCount;
    object->info.closed = object->info.handleCount == 0;
    CollectClosedObject(object);
    return true;
}

bool GuestObjects::SetEvent(uint32_t handle, uint32_t* previousState) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end() || it->second->info.type != GuestObjectType::Event) return false;
    const auto& object = it->second;
    std::lock_guard<std::mutex> waitLock(object->waitMutex);
    if (previousState) *previousState = object->signalled ? 1 : 0;
    object->signalled = true;
    if (object->manualReset) object->waitWake.notify_all();
    else object->waitWake.notify_one();
    return true;
}

bool GuestObjects::ClearEvent(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end() || it->second->info.type != GuestObjectType::Event) return false;
    std::lock_guard<std::mutex> waitLock(it->second->waitMutex);
    it->second->signalled = false;
    return true;
}

uint32_t GuestObjects::WaitForSingleObject(uint32_t handle, bool hasTimeout, int64_t timeout) {
    std::shared_ptr<Object> object;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = byHandle_.find(handle);
        if (it == byHandle_.end()) return kStatusInvalidHandle;
        object = it->second;
    }
    std::unique_lock<std::mutex> waitLock(object->waitMutex);
    if (object->info.type == GuestObjectType::Timer) {
        // Expiry is a point in time, not a notification: sleep until the
        // earlier of the timer's expiry and the caller's timeout.
        using clock = std::chrono::steady_clock;
        const auto deadline = hasTimeout ? SteadyTimeFromNt(timeout) : clock::time_point::max();
        for (;;) {
            if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
            const auto now = clock::now();
            object->AdvanceTimer(now);
            if (object->signalled) {
                if (!object->manualReset) object->signalled = false;
                return kStatusSuccess;
            }
            if (now >= deadline) return kStatusTimeout;
            const auto wake = object->timerArmed ? std::min(deadline, object->timerDue) : deadline;
            if (wake == clock::time_point::max()) object->waitWake.wait(waitLock);
            else object->waitWake.wait_until(waitLock, wake);
        }
    }
    const auto consumeSignal = [&] {
        if (object->info.type == GuestObjectType::Event ||
            object->info.type == GuestObjectType::NotifyListener) {
            if (!object->signalled) return false;
            if (!object->manualReset) object->signalled = false;
            return true;
        }
        if (object->info.type == GuestObjectType::Semaphore) {
            if (!object->semaphoreCount) return false;
            --object->semaphoreCount;
            return true;
        }
        return object->info.type == GuestObjectType::Thread &&
            object->thread.state == GuestThreadState::Terminated;
    };
    if (consumeSignal()) return kStatusSuccess;
    const auto ready = [&] {
        if (GuestRuntimeStopRequested()) return true;
        if (object->info.type == GuestObjectType::Event ||
            object->info.type == GuestObjectType::NotifyListener) return object->signalled;
        if (object->info.type == GuestObjectType::Semaphore) return object->semaphoreCount != 0;
        return object->info.type == GuestObjectType::Thread && object->thread.state == GuestThreadState::Terminated;
    };
    if (!hasTimeout) {
        object->waitWake.wait(waitLock, ready);
    } else {
        using namespace std::chrono;
        int64_t ticks = timeout < 0 ? (timeout == INT64_MIN ? INT64_MAX : -timeout) : 0;
        if (timeout > 0) {
            constexpr int64_t kWindowsToUnixEpochTicks = 11644473600LL * 10'000'000LL;
            const int64_t now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100
                + kWindowsToUnixEpochTicks;
            ticks = timeout > now ? timeout - now : 0;
        }
        if (!object->waitWake.wait_for(waitLock, milliseconds(ticks / 10'000), ready)) return kStatusTimeout;
    }
    if (GuestRuntimeStopRequested()) throw GuestRuntimeStop{};
    return consumeSignal() ? kStatusSuccess : kStatusTimeout;
}

bool GuestObjects::DequeueNotification(uint32_t handle, uint32_t matchId,
                                       uint32_t* notificationId, uint32_t* parameter) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end() || it->second->info.type != GuestObjectType::NotifyListener)
        return false;
    const auto& object = it->second;
    std::lock_guard<std::mutex> waitLock(object->waitMutex);
    auto selected = object->notifications.end();
    if (!matchId && !object->notifications.empty()) {
        selected = object->notifications.begin();
    } else if (matchId) {
        for (auto candidate = object->notifications.begin();
             candidate != object->notifications.end(); ++candidate) {
            if (candidate->first == matchId) {
                selected = candidate;
                break;
            }
        }
    }
    if (selected == object->notifications.end()) return false;
    if (notificationId) *notificationId = selected->first;
    if (parameter) *parameter = selected->second;
    object->notifications.erase(selected);
    object->signalled = !object->notifications.empty();
    return true;
}

uint32_t GuestObjects::BroadcastNotification(uint32_t notificationId,
                                             uint32_t parameter) {
    // XNotificationKey packs local id [15:0], version [24:16], and listener
    // mask index [30:25]. Match the pinned XNotifyListener filter exactly.
    const uint32_t version = (notificationId >> 16) & 0x1FFu;
    const uint32_t maskIndex = (notificationId >> 25) & 0x3Fu;
    const uint64_t maskBit = uint64_t{1} << maskIndex;
    uint32_t delivered{};
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, object] : byGuestObject_) {
        if (object->info.type != GuestObjectType::NotifyListener ||
            !(object->notificationMask & maskBit) ||
            version > object->notificationMaxVersion) {
            continue;
        }
        std::lock_guard<std::mutex> waitLock(object->waitMutex);
        object->notifications.emplace_back(notificationId, parameter);
        object->signalled = true;
        object->waitWake.notify_all();
        ++delivered;
    }
    return delivered;
}

uint32_t GuestObjects::Enumerate(uint32_t handle, uint32_t destinationGuestAddress,
                                 uint8_t* destination, uint32_t destinationBytes,
                                 uint32_t* itemsWritten) {
    if (itemsWritten) *itemsWritten = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end() || it->second->info.type != GuestObjectType::Enumerator) {
        return kXErrorInvalidHandle;
    }
    if (!destination) return kXErrorInvalidParameter;
    const auto& object = it->second;
    const uint32_t itemCount = object->enumeratorItemCount;
    if (object->enumeratorNextItem >= itemCount) return kXErrorNoMoreFiles;
    const uint32_t remaining = itemCount - object->enumeratorNextItem;
    const uint32_t writeCount = std::min(remaining, object->enumeratorItemsPerCall);
    const uint64_t writeBytes64 = uint64_t(writeCount) * object->enumeratorItemSize;
    if (writeBytes64 > destinationBytes) return kXErrorInsufficientBuffer;
    const uint32_t writeBytes = static_cast<uint32_t>(writeBytes64);
    if (object->enumeratorWriter) {
        uint32_t customWritten{};
        const uint32_t result = object->enumeratorWriter(
            object->enumeratorNextItem, writeCount, destinationGuestAddress,
            destination, destinationBytes, &customWritten);
        if (result) return result;
        if (customWritten > writeCount) return kXErrorInvalidParameter;
        object->enumeratorNextItem += customWritten;
        if (itemsWritten) *itemsWritten = customWritten;
        return 0;
    }
    const size_t sourceOffset = size_t(object->enumeratorNextItem) * object->enumeratorItemSize;
    {
        const RuntimeGuestWriteCompletion completion(destinationGuestAddress, writeBytes);
        std::memcpy(destination, object->enumeratorItems.data() + sourceOffset, writeBytes);
    }
    object->enumeratorNextItem += writeCount;
    if (itemsWritten) *itemsWritten = writeCount;
    return 0;
}

uint32_t GuestObjects::ReleaseSemaphore(uint32_t handle, uint32_t releaseCount, uint32_t* previousCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end() || it->second->info.type != GuestObjectType::Semaphore) {
        return kStatusInvalidHandle;
    }
    const auto& object = it->second;
    std::lock_guard<std::mutex> waitLock(object->waitMutex);
    if (!releaseCount || releaseCount > object->semaphoreLimit - object->semaphoreCount) {
        return kStatusInvalidParameter;
    }
    if (previousCount) *previousCount = object->semaphoreCount;
    object->semaphoreCount += releaseCount;
    object->waitWake.notify_all();
    return kStatusSuccess;
}

uint32_t GuestObjects::OpenObjectByGuestPointer(uint32_t guestObject, uint32_t* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    // Xenia's exported ObOpenObjectByPointer contract rejects a guest pointer
    // that is not a live kernel object with STATUS_UNSUCCESSFUL.  It is not a
    // handle lookup, so STATUS_INVALID_HANDLE would misidentify this failure.
    if (it == byGuestObject_.end()) return kStatusUnsuccessful;
    const auto& object = it->second;
    while (!nextHandle_ || byHandle_.count(nextHandle_)) ++nextHandle_;
    const uint32_t newHandle = nextHandle_++;
    byHandle_.emplace(newHandle, object);
    ++object->info.handleCount;
    object->info.closed = false;
    if (handle) *handle = newHandle;
    return kStatusSuccess;
}

bool GuestObjects::SetThreadAffinity(uint32_t guestObject, uint32_t affinity, uint32_t* previousAffinity) {
    if (!affinity) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    if (previousAffinity) *previousAffinity = it->second->thread.affinity;
    it->second->thread.affinity = affinity;
    return true;
}

bool GuestObjects::SetThreadPriority(uint32_t guestObject, int32_t priority, int32_t* previousPriority) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    if (previousPriority) *previousPriority = it->second->thread.priority;
    it->second->thread.priority = priority;
    return true;
}

bool GuestObjects::SetThreadFpuExceptionsEnabled(uint32_t guestObject, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    it->second->thread.fpuExceptionsEnabled = enabled;
    return true;
}

bool GuestObjects::SetThreadState(uint32_t guestObject, GuestThreadState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    std::lock_guard<std::mutex> waitLock(it->second->waitMutex);
    it->second->thread.state = state;
    if (state == GuestThreadState::Terminated) it->second->waitWake.notify_all();
    return true;
}

bool GuestObjects::TerminateThread(uint32_t guestObject, uint32_t exitCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    std::lock_guard<std::mutex> waitLock(it->second->waitMutex);
    it->second->thread.exitCode = exitCode;
    it->second->thread.state = GuestThreadState::Terminated;
    it->second->waitWake.notify_all();
    return true;
}

bool GuestObjects::GetThreadInfo(uint32_t guestObject, GuestThreadInfo* info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byGuestObject_.find(guestObject);
    if (it == byGuestObject_.end() || it->second->info.type != GuestObjectType::Thread) return false;
    if (info) *info = it->second->thread;
    return true;
}

bool GuestObjects::GetObjectInfo(uint32_t handle, GuestObjectInfo* info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return false;
    if (info) {
        *info = it->second->info;
        // The object keeps its original creation handle as identity metadata;
        // this query is handle-centric, so report the specific live entry that
        // was resolved.
        info->handle = handle;
        info->closed = false;
    }
    return true;
}

void GuestObjects::WakeAllWaiters() {
    std::vector<std::shared_ptr<Object>> objects;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        objects.reserve(byGuestObject_.size());
        for (const auto& [_, object] : byGuestObject_) objects.push_back(object);
    }
    for (const auto& object : objects) object->waitWake.notify_all();
}

void GuestObjects::ResetForTests(uint32_t firstGuestObject) {
    std::lock_guard<std::mutex> lock(mutex_);
    nextHandle_ = 1;
    nextGuestObject_ = firstGuestObject;
    byHandle_.clear();
    byGuestObject_.clear();
}

GuestObjects& GetGuestObjects() {
    static GuestObjects objects;
    return objects;
}
