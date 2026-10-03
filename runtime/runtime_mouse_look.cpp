// Native mouse look (V310+). After the client's per-frame look update
// (CWClient_Mod slot 0x1C4, sub_823F8AF0, which shapes the right stick into a
// look velocity), the physical mouse counts accumulated since the previous
// frame are sent through the title's own look(dx, dy) command (slot 0x420,
// sub_823FCF68 - what the title binds to "mousemove"): 1:1 position control
// without the stick's dead zone, acceleration ramp or turn cap. The title
// still applies its own view-side handling (zoom, pitch clamp, yaw wrap), and
// the stick path is untouched. See runtime_mouse_look_policy.h for units,
// remainder carry, command size and the command rate (one per frame while
// the client's command ring has room, at most 240 per second).
//
// The title routes controller input through its per-frame input processor
// (sub_820E29D8, main game object): to the console while the bind manager is
// in console mode, to the active window (menus) first - a modal window
// consumes what it does not handle - and only then to the binds that call
// look(). The direct call mirrors those gates, so menus and the console keep
// the mouse out of the camera; cutscene and death states are rejected by the
// title's own look handling.
//
// Enabled with keyboard/mouse input and input.mouse_look = "native" (ReXGlue
// then stops translating mouse motion to the right stick). The environment
// variable DARKNESS_NATIVE_MOUSE_LOOK=0 disables the hook for comparisons.
#include "runtime_mouse_look.h"
#include "runtime_button_prompts.h"

#include "ppc_recomp_shared.h"
#include "runtime_gpu_calibration.h"
#include "runtime_graphics.h"
#include "runtime_mouse_look_policy.h"
#include "runtime_fatal.h"
#include "runtime_movement_packet.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// sub_823FCF68 (look), sub_823F8AF0 and sub_820E29D8 are declared by
// ppc_recomp_shared.h; the overrides below replace their generated weak
// aliases.
extern "C" PPC_FUNC(__imp__sub_823F8AF0);
extern "C" PPC_FUNC(__imp__sub_820E29D8);

std::atomic<bool> runtimeNativeMouseLook{true};

namespace {

// Game-thread state (the input processor and the client update both run on
// the title's game thread).
runtime_mouse_look::State g_state;
std::atomic<uint32_t> g_mainGame{};
std::atomic<uint64_t> g_commands{};
std::atomic<uint64_t> g_frames{};
std::atomic<uint64_t> g_ringDeferrals{};
std::atomic<bool> g_ringLogged{};
LARGE_INTEGER g_frequency{};

constexpr uint32_t kSensXOffset = 0x2284;
constexpr uint32_t kSensYOffset = 0x2288;

// Client command ring (AddCommand sub_824A1720 -> sub_82433970; free space as
// sub_824338F0): 36-byte commands, full ring = every queued command dropped.
constexpr uint32_t kClientCommandRing = 0xB08;  // -> ring
constexpr uint32_t kRingDescriptor = 12;        // -> array descriptor, +4 capacity
constexpr uint32_t kRingHead = 16;              // write index
constexpr uint32_t kRingTail = 20;              // read index

// Title input routing (sub_820E29D8 and sub_820F9218).
constexpr uint32_t kMainGameSystem = 0x18;       // -> system
constexpr uint32_t kSystemBindManager = 0x38;    // -> bind manager
constexpr uint32_t kBindManagerMode = 0x5C;      // 1: keys go to the console
constexpr uint32_t kMainGameFlags = 0xAA0;
constexpr uint32_t kDebugOverlayFlag = 0x400000; // keys go to the debug overlay
constexpr uint32_t kMainGameContext = 0xAE4;     // -> game context
constexpr uint32_t kContextWindow = 0xE58;       // active window (menus)
constexpr uint32_t kWindowModal = 0x10;          // consumes unhandled input

uint32_t ReadGuestU32(uint8_t* base, uint32_t address) {
    uint32_t raw;
    std::memcpy(&raw, base + address, sizeof(raw));
    return _byteswap_ulong(raw);
}

float ReadGuestFloat(uint8_t* base, uint32_t address) {
    const uint32_t raw = ReadGuestU32(base, address);
    float value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

bool IsGuestPointer(uint32_t address) { return address >= 0x10000u; }

struct CommandRing {
    bool known = false;
    int32_t capacity = 0;
    int32_t free = 0;
};

CommandRing ReadCommandRing(uint8_t* base, uint32_t client) {
    CommandRing ring;
    const uint32_t queue = ReadGuestU32(base, client + kClientCommandRing);
    if (!IsGuestPointer(queue)) {
        return ring;
    }
    const uint32_t descriptor = ReadGuestU32(base, queue + kRingDescriptor);
    if (!IsGuestPointer(descriptor)) {
        return ring;
    }
    const int32_t capacity = int32_t(ReadGuestU32(base, descriptor + 4));
    const int32_t head = int32_t(ReadGuestU32(base, queue + kRingHead));
    const int32_t tail = int32_t(ReadGuestU32(base, queue + kRingTail));
    if (capacity <= 0 || head < 0 || tail < 0 || head >= capacity || tail >= capacity) {
        return ring;
    }
    ring.known = true;
    ring.capacity = capacity;
    ring.free = head == tail ? capacity - 1 : (2 * capacity - head + tail - 1) % capacity;
    return ring;
}

struct Gate {
    bool open = false;
    uint32_t context = 0;
    uint32_t window = 0;
    uint32_t windowVtable = 0;
    uint32_t modal = 0;
    uint32_t bindMode = 0;
    bool overlay = false;

    bool operator==(const Gate& other) const {
        return open == other.open && context == other.context && window == other.window &&
               windowVtable == other.windowVtable && modal == other.modal &&
               bindMode == other.bindMode && overlay == other.overlay;
    }
};

// Where the title would deliver a controller look event right now: open
// only when neither the console, the debug overlay nor a modal window would
// take it.
Gate ReadGate(uint8_t* base) {
    Gate gate;
    const uint32_t mainGame = g_mainGame.load(std::memory_order_relaxed);
    if (!IsGuestPointer(mainGame)) {
        return gate;
    }
    const uint32_t system = ReadGuestU32(base, mainGame + kMainGameSystem);
    const uint32_t bindManager =
        IsGuestPointer(system) ? ReadGuestU32(base, system + kSystemBindManager) : 0;
    gate.bindMode =
        IsGuestPointer(bindManager) ? ReadGuestU32(base, bindManager + kBindManagerMode) : 0;
    gate.overlay = (ReadGuestU32(base, mainGame + kMainGameFlags) & kDebugOverlayFlag) != 0;
    gate.context = ReadGuestU32(base, mainGame + kMainGameContext);
    if (!IsGuestPointer(gate.context)) {
        return gate;
    }
    gate.window = ReadGuestU32(base, gate.context + kContextWindow);
    if (IsGuestPointer(gate.window)) {
        gate.windowVtable = ReadGuestU32(base, gate.window);
        gate.modal = ReadGuestU32(base, gate.window + kWindowModal);
    }
    gate.open = gate.bindMode != 1 && !gate.overlay && !(gate.window && gate.modal);
    return gate;
}

void LogGateChange(const Gate& gate) {
    static Gate last;
    static bool haveLast = false;
    if (haveLast && gate == last) {
        return;
    }
    last = gate;
    haveLast = true;
    std::fprintf(stderr,
                 "RUNTIME_NATIVE_MOUSE_LOOK_GATE open=%u context=0x%08X window=0x%08X "
                 "window_vtable=0x%08X modal=%u bind_mode=%u overlay=%u\n",
                 gate.open ? 1u : 0u, gate.context, gate.window, gate.windowVtable,
                 gate.modal, gate.bindMode, gate.overlay ? 1u : 0u);
    std::fflush(stderr);
}

// Drops pending look units (bounded record when whole units are dropped).
void DropPending(const char* reason) {
    static uint32_t records = 0;
    if ((std::fabs(g_state.pendingYawUnits) >= 1.0 ||
         std::fabs(g_state.pendingPitchUnits) >= 1.0) && records < 64) {
        ++records;
        std::fprintf(stderr,
                     "RUNTIME_NATIVE_MOUSE_LOOK_DROP reason=%s yaw_units=%.1f pitch_units=%.1f\n",
                     reason, g_state.pendingYawUnits, g_state.pendingPitchUnits);
        std::fflush(stderr);
    }
    runtime_mouse_look::Reset(g_state);
}

void AfterClientFrame(PPCContext& ctx, uint8_t* base, uint32_t client) {
    if (!runtimeNativeMouseLook.load(std::memory_order_relaxed) || !client) {
        return;
    }
    RuntimeGraphicsMouseLook mouse;
    if (!RuntimeGraphicsConsumeMouseLook(RuntimeGraphicsKeyboardMouseUserIndex(), mouse)) {
        // Released capture, lost focus or stick mode: nothing may carry over.
        DropPending("inactive");
        return;
    }
    if (mouse.dx || mouse.dy) {
        // Mouse movement counts as keyboard/mouse input for button prompts.
        RuntimeNoteInputActivity(false, true);
    }
    const Gate gate = ReadGate(base);
    LogGateChange(gate);
    if (!gate.open) {
        // Motion made while a menu or the console has the input is dropped,
        // as it would be for the stick.
        DropPending("gate");
        return;
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    runtime_mouse_look::Accumulate(g_state, mouse.dx, mouse.dy, mouse.sensitivity,
                                   mouse.invertY);
    const CommandRing ring = ReadCommandRing(base, client);
    if (!g_ringLogged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "RUNTIME_NATIVE_MOUSE_LOOK_RING known=%u capacity=%d free=%d "
                     "min_free=%d\n",
                     ring.known ? 1u : 0u, ring.capacity, ring.free,
                     (std::max)(runtime_mouse_look::kMinFreeCommandSlots, ring.capacity / 2));
        std::fflush(stderr);
    }
    if (!ring.known || !runtime_mouse_look::CommandRingHasRoom(ring.capacity, ring.free)) {
        // The title's own commands keep their room; the motion waits.
        g_ringDeferrals.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const runtime_mouse_look::Command command = runtime_mouse_look::Next(
        g_state, ReadGuestFloat(base, client + kSensXOffset),
        ReadGuestFloat(base, client + kSensYOffset), now.QuadPart, g_frequency.QuadPart);
    if (!command.send) {
        return;
    }
    // A call at the client's own call site: preserve what the caller of the
    // frame update may read back (volatile return registers).
    const PPCRegister savedR3 = ctx.r3;
    const PPCRegister savedF1 = ctx.f1;
    const PPCRegister savedF2 = ctx.f2;
    ctx.r3.u64 = client;
    ctx.f1.f64 = double(command.argX);
    ctx.f2.f64 = double(command.argY);
    sub_823FCF68(ctx, base);
    ctx.r3 = savedR3;
    ctx.f1 = savedF1;
    ctx.f2 = savedF2;
    const uint64_t commands = g_commands.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((commands & 0xFFF) == 1) {
        std::fprintf(stderr,
                     "RUNTIME_NATIVE_MOUSE_LOOK commands=%llu frames=%llu ring_deferrals=%llu "
                     "last_yaw_units=%d last_pitch_units=%d\n",
                     static_cast<unsigned long long>(commands),
                     static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(
                         g_ringDeferrals.load(std::memory_order_relaxed)),
                     command.yawUnits, command.pitchUnits);
        std::fflush(stderr);
    }
}

}  // namespace

void InitializeRuntimeNativeMouseLook() {
    QueryPerformanceFrequency(&g_frequency);
    if (const char* value = std::getenv("DARKNESS_NATIVE_MOUSE_LOOK")) {
        runtimeNativeMouseLook.store(value[0] != '0', std::memory_order_relaxed);
    }
    std::fprintf(stderr, "RUNTIME_NATIVE_MOUSE_LOOK_POLICY enabled=%u units_per_count=%.2f "
                         "min_interval_ms=%.2f ring_room=half_min_%d\n",
                 runtimeNativeMouseLook.load(std::memory_order_relaxed) ? 1u : 0u,
                 runtime_mouse_look::kUnitsPerCount,
                 runtime_mouse_look::kMinCommandSeconds * 1000.0,
                 runtime_mouse_look::kMinFreeCommandSlots);
    std::fflush(stderr);
}

namespace {

// Menus for display.menu_frame_rate (V330): the title shows a menu when no
// game client updates (title screen, main menu, loading) or a modal window
// has the input (pause and in-game menus). Reads guest state only.
std::atomic<uint64_t> g_clientFrames{};

void UpdateMenuState(uint8_t* base) {
    static uint64_t lastClientFrames = 0;
    static uint32_t framesWithoutClient = 0;
    static int lastMenu = -1;
    static Gate lastGate;
    const uint64_t clientFrames = g_clientFrames.load(std::memory_order_relaxed);
    if (clientFrames != lastClientFrames) {
        lastClientFrames = clientFrames;
        framesWithoutClient = 0;
    } else if (framesWithoutClient < 1000) {
        ++framesWithoutClient;
    }
    // A few input frames without a client update: none is running.
    const bool clientRunning = framesWithoutClient < 4;
    const Gate gate = ReadGate(base);
    const bool modal = gate.window && gate.modal;
    const int menu = (!clientRunning || modal) ? 1 : 0;
    // The front end's screens (no client, a menu window) are told apart by
    // the class (vtable) of the window's active screen object; measured
    // automatic settings time the main menu's 3D backdrop (V338).
    const uint32_t screenClass = modal ? ReadGuestU32(base, gate.modal) : 0;
    RuntimeGpuCalibrationOnGuestFrame(!clientRunning && modal, screenClass);
    if (menu == lastMenu && gate.window == lastGate.window && gate.modal == lastGate.modal) {
        return;
    }
    const bool changed = menu != lastMenu;
    lastMenu = menu;
    lastGate = gate;
    std::fprintf(stderr,
                 "RUNTIME_MENU_STATE menu=%d client=%u window=0x%08X window_vtable=0x%08X "
                 "modal=%u screen_class=0x%08X context=0x%08X\n",
                 menu, clientRunning ? 1u : 0u, gate.window, gate.windowVtable, gate.modal,
                 screenClass, gate.context);
    std::fflush(stderr);
    if (changed) RuntimeGraphicsSetMenuState(menu != 0 ? 1u : 0u);
}

}  // namespace

// The main game's per-frame input processor: records the main game object
// for the gates above, then runs unchanged.
PPC_FUNC(sub_820E29D8) {
    g_mainGame.store(ctx.r3.u32, std::memory_order_relaxed);
    __imp__sub_820E29D8(ctx, base);
    UpdateMenuState(base);
}

PPC_FUNC(sub_823F8AF0) {
    const uint32_t client = ctx.r3.u32;
    if (RuntimeStickCommandTraceActive()) {
        std::printf("RUNTIME_STICK_FRAME frame=%llu client_frame=%llu lr=0x%08X\n",
                    static_cast<unsigned long long>(RuntimeSwapCount()),
                    static_cast<unsigned long long>(g_clientFrames.load(std::memory_order_relaxed)),
                    uint32_t(ctx.lr));
    }
    __imp__sub_823F8AF0(ctx, base);
    g_clientFrames.fetch_add(1, std::memory_order_relaxed);
    AfterClientFrame(ctx, base, client);
}
