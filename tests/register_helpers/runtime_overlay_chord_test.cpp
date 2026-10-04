// Controller access to the settings overlay (#7): Back + Start opens it, and
// the title never sees either button of the chord, also when one lands a
// little before the other; lone Back/Start presses and taps still reach the
// title, a window later.
#include "runtime_overlay_chord.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            passed = false;
        }
    };
    using Filter = RuntimeOverlayChordFilter;
    constexpr uint16_t kA = 0x1000;
    constexpr uint16_t kBack = Filter::kBack;
    constexpr uint16_t kStart = Filter::kStart;
    {
        // A lone Start held: hidden for the window, then shown until released.
        Filter filter;
        check(filter.Filter(kStart, 0) == 0, "a fresh Start waits for a possible chord");
        check(filter.Filter(kStart, 99) == 0, "still inside the window");
        check(filter.Filter(kStart, 100) == kStart, "after the window Start reaches the title");
        check(filter.Filter(kStart, 400) == kStart, "held Start stays pressed");
        check(filter.Filter(0, 410) == 0, "release reaches the title at once");
    }
    {
        // A quick tap inside the window is still delivered, a little late.
        Filter filter;
        check(filter.Filter(kBack, 0) == 0, "tap pressed: waiting");
        check(filter.Filter(0, 30) == kBack, "tap released inside the window: shown now");
        check(filter.Filter(0, 89) == kBack, "the tap stays visible for its hold time");
        check(filter.Filter(0, 90) == 0, "then it is released");
    }
    {
        // The chord with Back first: the title sees neither button.
        Filter filter;
        check(filter.Filter(kBack, 0) == 0, "Back first: hidden");
        check(filter.Filter(kBack | kStart, 40) == 0, "Start joins: chord, both hidden");
        check(filter.InChord() && filter.Chords() == 1, "one chord counted");
        check(filter.Filter(kBack | kStart, 500) == 0, "a held chord stays hidden");
        check(filter.Filter(kBack, 600) == 0, "releasing one keeps the other hidden");
        check(filter.Filter(0, 610) == 0 && !filter.InChord(), "both released: chord over");
        check(filter.Filter(kStart, 700) == 0 && filter.Filter(kStart, 800) == kStart,
              "after the chord a lone Start works normally again");
    }
    {
        // Both at the same poll.
        Filter filter;
        check(filter.Filter(kBack | kStart | kA, 0) == kA, "simultaneous chord: only A passes");
        check(filter.Filter(kA, 50) == kA, "chord released, A still passes");
    }
    {
        // Start already delivered (the pause menu is open), then Back: the
        // overlay opens on top, and the title sees Start released.
        Filter filter;
        filter.Filter(kStart, 0);
        check(filter.Filter(kStart, 150) == kStart, "Start delivered");
        check(filter.Filter(kStart | kBack, 300) == 0, "late Back makes a chord: both hidden");
        check(filter.Filter(0, 320) == 0, "released");
    }
    {
        // Other buttons are never delayed.
        Filter filter;
        check(filter.Filter(kA, 0) == kA, "A passes at once");
        check(filter.Filter(0x000F, 1) == 0x000F, "d-pad passes at once");
    }
    {
        // A second press during a shown tap first shows the release.
        Filter filter;
        filter.Filter(kStart, 0);
        check(filter.Filter(0, 20) == kStart, "tap shown");
        check(filter.Filter(kStart, 40) == 0, "new press: the title sees the release first");
        check(filter.Filter(kStart, 140) == kStart, "then the new press after its window");
    }
    {
        // The runtime applies the filter to the physical controller only (the
        // keyboard has the overlay key), before the button remap.
        std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_input.cpp");
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        const size_t filter = text.find("overlayChordFilters[");
        const size_t remap = text.find("RemapRuntimeControllerState(guest", filter);
        const size_t merge = text.find("MergeRuntimeInputStates(guest", filter);
        check(filter != std::string::npos, "runtime_input.cpp filters the controller chord");
        check(remap != std::string::npos && filter < remap, "the filter runs before the remap");
        check(merge != std::string::npos && filter < merge,
              "the filter runs before the keyboard merge");
    }
    if (!passed) return 1;
    std::cout << "runtime_overlay_chord: all checks passed\n";
    return 0;
}
