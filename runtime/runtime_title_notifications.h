#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

struct RuntimeTitleTerminateNotification {
    uint32_t routine{};
    uint32_t priority{};
};

class RuntimeTitleTerminateNotifications {
public:
    void Register(uint32_t routine, uint32_t priority);
    void Remove(uint32_t routine);
    std::vector<RuntimeTitleTerminateNotification> Snapshot() const;
    void ResetForTests();

private:
    mutable std::mutex mutex_{};
    std::vector<RuntimeTitleTerminateNotification> notifications_{};
};

RuntimeTitleTerminateNotifications& GetRuntimeTitleTerminateNotifications();
