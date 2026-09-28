// V380: fatal stops never end the game quietly, and the guest printf family
// formats like the Microsoft CRT (they were unresolved-import traps).
#include "../../runtime/runtime_guest_format.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

uint64_t DoubleBits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string Format(const char* format, std::vector<uint64_t> slots) {
    size_t index = 0;
    RuntimeGuestFormatArgs args;
    args.next = [&]() { return index < slots.size() ? slots[index++] : 0ull; };
    args.read_string = [](uint32_t address) {
        return address == 0x1000 ? std::string("street") : std::string("?");
    };
    args.read_wide = [](uint32_t address) {
        return address == 0x2000 ? std::u16string(u"Jackie") : std::u16string();
    };
    return RuntimeFormatGuestString(format, args);
}
}  // namespace

int main() {
    bool ok = true;
    // Integers are the low word of an 8-byte slot unless 64-bit is asked for.
    ok &= Check(Format("%d %i %u", {0xFFFFFFFFFFFFFFFFull, 42, 0x1FFFFFFFFull}) ==
                    "-1 42 4294967295",
                "32-bit integers take the low word of their slot");
    ok &= Check(Format("%I64d %lld", {0xFFFFFFFFFFFFFFFFull, 5}) == "-1 5",
                "64-bit integers take the whole slot");
    ok &= Check(Format("%08X|%-4x|%#o", {0xBEEF, 0x1F, 8}) == "0000BEEF|1f  |010",
                "flags and widths");
    ok &= Check(Format("%*d|%.*s", {5, 7, 3, 0x1000}) == "    7|str", "star width and precision");
    ok &= Check(Format("%s %S %ls %s", {0x1000, 0x2000, 0x2000, 0}) ==
                    "street Jackie Jackie (null)",
                "narrow, wide and null strings");
    ok &= Check(Format("%.2f %g", {DoubleBits(3.14159), DoubleBits(0.5)}) == "3.14 0.5",
                "doubles are the raw slot bits");
    ok &= Check(Format("%c%c 100%% %p", {'o', 'k', 0x82001000}) == "ok 100% 82001000",
                "characters, percent and pointers");
    ok &= Check(Format("%hd %hhu", {0x12345FFFF, 0x1FF}) == "-1 255", "short and char lengths");
    ok &= Check(Format("%q", {}) == "%q", "unknown conversions are copied");
    ok &= Check(RuntimeUtf16ToUtf8(u"é中") == "\xc3\xa9\xe4\xb8\xad",
                "UTF-16 to UTF-8");

    // Every fatal stop leaves a report and tells the player; automation opts
    // out of the message box; the V380 imports are no longer traps.
    const std::string root = DARKNESS_SOURCE_ROOT;
    const std::string main_source = Read(root + "/runtime/main.cpp");
    const std::string threads = Read(root + "/runtime/runtime_threads.cpp");
    const std::string imports = Read(root + "/runtime/verified_imports.cpp");
    const std::string traps = Read(root + "/runtime/generate_import_traps.ps1");
    const std::string probe = Read(root + "/scripts/start-isolated-gameplay-probe.ps1");
    // Report and message first: a teardown that hangs or faults cannot hide them.
    // A game start only: the launcher's windowless settings actions return the
    // error as text (a dialog there waits unseen and blocks the launcher).
    ok &= Check(main_source.find("        if (fatal && gameStart) {\n"
                                 "            RuntimeFatalReport(blocker);\n"
                                 "            RuntimeFatalShowDialog(blocker);\n        }") !=
                        std::string::npos &&
                    main_source.find("gameStart = launchOptions.action == "
                                     "RuntimeLaunchAction::Run;") != std::string::npos &&
                    main_source.find("fatal = !workerBlocker.empty();") != std::string::npos &&
                    main_source.find("return fatal ? 1 : 0;") != std::string::npos,
                "the main catch reports fatal stops (not a player's own close) before teardown");
    // A closed window stops the title (V380: before, it ran on without one).
    const std::string plugin = Read(root + "/external/ReXGlue/src/graphics/plugin_main.cpp");
    const std::string graphics_h =
        Read(root + "/external/ReXGlue/include/rex/graphics/graphics_system.h");
    ok &= Check(plugin.find("window->AddListener(&close_request_listener);") !=
                        std::string::npos &&
                    plugin.find("REX_WINDOW_CLOSE_WATCHDOG hard_exit=1") != std::string::npos &&
                    imports.find("if (RuntimeGraphicsCloseRequested()) {") != std::string::npos,
                "closing the window stops the title, with a watchdog");
    ok &= Check(plugin.find("embedded.graphics->ReleaseAppContext();") != std::string::npos &&
                    graphics_h.find("void ReleaseAppContext() { app_context_ = nullptr; }") !=
                        std::string::npos,
                "the presentation thread's app context is released before it is destroyed");
    ok &= Check(main_source.find("RuntimeFatalWriteDump(\"unresolved-import\");") !=
                        std::string::npos &&
                    main_source.find("\"unresolved guest import \") + importName") !=
                        std::string::npos,
                "an unresolved import dumps where it happened and names itself");
    ok &= Check(threads.find("RuntimeFatalWriteDump(\"guest-worker-blocker\");") !=
                    std::string::npos,
                "a worker's blocker dumps where it happened");
    ok &= Check(imports.find("RuntimeFatalReport(\"the game reported a disc or file read error") !=
                    std::string::npos,
                "the title's dirty-disc exit is reported");
    for (const char* symbol : {"'__imp__DbgPrint'", "'__imp___vsnprintf'", "'__imp__sprintf'",
                               "'__imp__XGetAVPack'", "'__imp__XamInputGetKeystrokeEx'",
                               "'__imp__XamShowMessageBoxUIEx'", "'__imp__XamLoaderTerminateTitle'",
                               "'__imp__KeBugCheck'", "'__imp__RtlRaiseException'"}) {
        ok &= Check(traps.find(symbol) != std::string::npos, symbol);
    }
    ok &= Check(imports.find("RuntimeIsOfflineXgiMessage(message)") != std::string::npos,
                "LIVE session/statistics messages answer like an offline console");
    ok &= Check(probe.find("'DARKNESS_NO_FATAL_DIALOG','1'") != std::string::npos,
                "automation runs never wait on the fatal message box");
    // V383: a freeze leaves a report too (the watch starts before the title,
    // every swap feeds it, a stop ends it, and it records without stopping).
    const std::string fatal_source = Read(root + "/runtime/runtime_fatal.cpp");
    ok &= Check(main_source.find("RuntimeFatalStartStallWatch(&RuntimeGraphicsCloseRequested);\n"
                                 "            _xstart(context, base);\n"
                                 "            RuntimeFatalStopStallWatch();") != std::string::npos &&
                    main_source.find("        RuntimeFatalStopStallWatch();\n"
                                     "        std::string blocker = error.what();") !=
                        std::string::npos,
                "the freeze watch runs exactly while the title runs");
    ok &= Check(imports.find("    RuntimeBeginPostSwapTrace(ctx);\n    RuntimeFatalNoteSwap();") !=
                    std::string::npos,
                "every frame feeds the freeze watch");
    ok &= Check(fatal_source.find("RUNTIME_STALL utc=") != std::string::npos &&
                    fatal_source.find("TheDarkness_stall_") != std::string::npos &&
                    fatal_source.find("RuntimeWriteThreadSnapshot(\"stall\");") != std::string::npos,
                "a freeze writes a report line, a thread snapshot and a dump");
    if (!ok) return 1;
    std::cout << "Fatal exits are reported; guest printf formatting PASS\n";
    return 0;
}
