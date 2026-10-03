// Single-player controller routing (0.9.1, issue #8): every physical
// controller drives guest user 0, the one with the signed-in profile; the
// active pad is the newest press (lowest slot on a tie), else the lowest
// connected slot; idle connected devices never take over.
#include "runtime_input_slot_routing.h"

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
    using Router = RuntimeInputSlotRouter;
    const auto poll = [](Router& router, const bool (&connected)[4], const uint16_t (&buttons)[4]) {
        for (uint32_t slot = 0; slot < 4; ++slot) router.Observe(slot, connected[slot], buttons[slot]);
        return router.Finish();
    };
    {
        Router router;
        check(poll(router, {false, false, false, false}, {0, 0, 0, 0}) == Router::kNone,
              "no controller: nothing drives player 1");
        check(poll(router, {false, true, false, false}, {0, 0, 0, 0}) == 1,
              "a pad only on slot 1 drives player 1");
        check(poll(router, {false, false, false, true}, {0, 0, 0, 0x10}) == 3,
              "a pad only on slot 3 drives player 1");
    }
    {
        // A stuck or idle device on slot 0 and a real pad on slot 2.
        Router router;
        check(poll(router, {true, false, true, false}, {0, 0, 0, 0}) == 0,
              "before any press the lowest connected slot is used");
        check(poll(router, {true, false, true, false}, {0, 0, 0x1000, 0}) == 2,
              "a press on slot 2 takes over");
        check(poll(router, {true, false, true, false}, {0, 0, 0x1000, 0}) == 2,
              "holding the button keeps it");
        check(poll(router, {true, false, true, false}, {0, 0, 0, 0}) == 2,
              "releasing keeps the active pad (the idle slot 0 cannot take over)");
    }
    {
        // Two pads: the newest press wins; a tie goes to the lowest slot.
        Router router;
        check(poll(router, {true, true, false, false}, {0x10, 0x10, 0, 0}) == 0,
              "a press on both in one poll goes to the lowest slot");
        check(poll(router, {true, true, false, false}, {0x10, 0x30, 0, 0}) == 1,
              "a new button on slot 1 is the newest press");
        check(poll(router, {true, true, false, false}, {0x30, 0x30, 0, 0}) == 0,
              "a new button on slot 0 takes it back");
    }
    {
        // Unplug and replug.
        Router router;
        poll(router, {false, true, true, false}, {0, 0, 0x10, 0});
        check(router.Active() == 2, "slot 2 active");
        check(poll(router, {false, true, false, false}, {0, 0, 0, 0}) == 1,
              "unplugging the active pad falls back to the lowest connected slot");
        check(poll(router, {false, true, true, false}, {0, 0, 0x10, 0}) == 2,
              "a replugged pad holding a button counts as a new press");
        check(poll(router, {false, false, false, false}, {0, 0, 0, 0}) == Router::kNone,
              "all unplugged: nothing");
    }
    {
        // Source policy: routed users 1-3 report no controller, player 1 polls
        // every slot and publishes through the merged packet tracker.
        std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + "/runtime/runtime_input.cpp");
        const std::string source((std::istreambuf_iterator<char>(file)), {});
        check(source.find("if (routed && actualUserIndex != 0) return kXErrorDeviceNotConnected;") !=
                  std::string::npos,
              "routed users 1-3 have no controller");
        check(source.find("inputSlotRouter.Observe(slot, results[slot] == ERROR_SUCCESS,") !=
                      std::string::npos &&
                  source.find("const uint32_t active = inputSlotRouter.Finish();") != std::string::npos,
              "player 1 routes every slot");
        check(source.find("const bool mergedDevice = routed || RuntimeGraphicsKeyboardMouseEnabled(actualUserIndex);") !=
                  std::string::npos,
              "the routed player publishes one packet sequence");
        check(source.find("std::strcmp(mode, \"console\") == 0") != std::string::npos,
              "DARKNESS_INPUT_ROUTING=console keeps the 1:1 mapping");
    }
    if (!passed) return 1;
    std::cout << "runtime input slot routing: PASS\n";
    return 0;
}
