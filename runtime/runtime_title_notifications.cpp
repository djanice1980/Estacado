#include "runtime_title_notifications.h"

#include <algorithm>

void RuntimeTitleTerminateNotifications::Register(uint32_t routine, uint32_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    notifications_.push_back({routine, priority});
}

void RuntimeTitleTerminateNotifications::Remove(uint32_t routine) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(notifications_.begin(), notifications_.end(),
        [routine](const RuntimeTitleTerminateNotification& notification) {
            return notification.routine == routine;
        });
    if (it != notifications_.end()) notifications_.erase(it);
}

std::vector<RuntimeTitleTerminateNotification> RuntimeTitleTerminateNotifications::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return notifications_;
}

void RuntimeTitleTerminateNotifications::ResetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    notifications_.clear();
}

RuntimeTitleTerminateNotifications& GetRuntimeTitleTerminateNotifications() {
    static RuntimeTitleTerminateNotifications notifications;
    return notifications;
}
