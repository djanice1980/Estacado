#pragma once

#include <string>

// Fatal stops of the title (V380). The runtime fails closed on a guest call it
// cannot serve (an unresolved import, an unverified contract) and on the
// title's own fatal paths; those stops end the process through main's catch.
// Players start the game from the launcher without a console, so before V380
// such a stop left nothing: the V379 play-test "crash" had no Windows error,
// no crash log and no dump. Every fatal stop now appends a RUNTIME_FATAL_EXIT
// line to <game>\logs\runtime_crash.log, writes a thread snapshot and a
// minidump (taken where the stop happened when possible) and tells the player
// in a message box. Automation sets DARKNESS_NO_FATAL_DIALOG to skip the box.

// Where reports go and the system dbghelp's MiniDumpWriteDump (main's
// PrepareCrashEvidence resolves both before any guest code runs).
void RuntimeFatalConfigure(const wchar_t* crashDirectory, void* miniDumpWriteDump) noexcept;
// That folder ("<executable directory>\logs\"; empty before the configuration).
const wchar_t* RuntimeFatalDirectory() noexcept;
// Extra context for the report (import name, registers, the title's message).
void RuntimeFatalRecordDetail(const std::string& detail) noexcept;
// A minidump of the process now, once per process (later calls do nothing).
void RuntimeFatalWriteDump(const char* tag) noexcept;
// Report line + thread snapshot + dump (if none yet). Once per process.
void RuntimeFatalReport(const std::string& reason) noexcept;
// The player-facing message (skipped when DARKNESS_NO_FATAL_DIALOG is set).
void RuntimeFatalShowDialog(const std::string& reason) noexcept;

// V383 freeze report. Every title state presents frames (gameplay, menus,
// loading screens), so a long gap without a swap after the first one is a
// freeze. The watch records it in logs\ (a RUNTIME_STALL line in
// runtime_crash.log, a thread snapshot, one TheDarkness_stall_*.dmp) and stops
// nothing, so a freeze the player ends by closing the window still leaves
// evidence; with a close request pending it records after 2 s (the close
// watchdog ends a frozen title 10 s after the request).
inline constexpr long long kRuntimeStallReportMs = 20000;
inline constexpr long long kRuntimeStallAtCloseMs = 2000;
// Every VdSwap.
void RuntimeFatalNoteSwap() noexcept;
// VdSwap calls so far (developer traces correlate with frames).
uint64_t RuntimeSwapCount() noexcept;
// Once, before the title starts; closeRequested polls the player's close.
void RuntimeFatalStartStallWatch(bool (*closeRequested)() noexcept) noexcept;
// Before a stop or shutdown (a stopping title presents no more frames).
void RuntimeFatalStopStallWatch() noexcept;
