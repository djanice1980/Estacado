#include "runtime_video_mode.h"

#include <atomic>

namespace darkness::guest_video_mode {
namespace {
std::atomic<uint32_t> g_width{kDisplayWidth};
std::atomic<uint32_t> g_height{kDisplayHeight};
}  // namespace

uint32_t DisplayWidth() { return g_width.load(std::memory_order_relaxed); }
uint32_t DisplayHeight() { return g_height.load(std::memory_order_relaxed); }

void ConfigureDisplaySize(uint32_t width, uint32_t height) {
    g_width.store(width, std::memory_order_relaxed);
    g_height.store(height, std::memory_order_relaxed);
}
}  // namespace darkness::guest_video_mode
