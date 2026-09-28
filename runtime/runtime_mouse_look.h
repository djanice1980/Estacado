#pragma once

#include <atomic>

// Native mouse look (runtime_mouse_look.cpp): physical mouse counts go through
// the title's own look command after each client frame. Default on; the
// environment variable DARKNESS_NATIVE_MOUSE_LOOK=0 disables the hook.
extern std::atomic<bool> runtimeNativeMouseLook;

void InitializeRuntimeNativeMouseLook();
