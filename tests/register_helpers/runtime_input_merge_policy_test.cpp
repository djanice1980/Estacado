#include "runtime_input.h"

#include <iostream>
#include <thread>
#include <vector>

namespace {
bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}
}

int main() {
    RuntimeInputState controller{};
    controller.packetNumber = 100;
    controller.buttons = 0x1000;
    controller.leftTrigger = 32;
    controller.thumbLX = -12000;
    controller.thumbRX = 4000;

    RuntimeInputState keyboardMouse{};
    keyboardMouse.packetNumber = 7;
    keyboardMouse.buttons = 0x0010;
    keyboardMouse.rightTrigger = 255;
    keyboardMouse.thumbLX = 32767;
    keyboardMouse.thumbRX = -9000;

    const RuntimeInputState merged =
        MergeRuntimeInputStates(controller, keyboardMouse);
    bool passed = true;
    // Same slot/lock type as production; synthetic concurrent polls, not host
    // input. The fixture changes payload inside the polling critical section.
    RuntimeMergedInputSlot slot;
    std::vector<uint32_t> publications;
    uint16_t fixtureButtons{};
    std::vector<std::thread> pollers;
    for (int worker = 0; worker < 4; ++worker) {
        pollers.emplace_back([&] {
            for (int poll = 0; poll < 1000; ++poll) {
                std::lock_guard lock(slot.mutex);
                RuntimeInputState state{};
                state.buttons = ++fixtureButtons;
                publications.push_back(slot.packets.Publish(state).packetNumber);
            }
        });
    }
    for (auto& poller : pollers) poller.join();
    passed &= Check(publications.size() == 4000, "every concurrent poll publishes");
    for (size_t i = 0; i < publications.size(); ++i)
        passed &= Check(publications[i] == i + 1, "publication follows serialized polling");
    RuntimeMergedInputSlot independentSlot;
    passed &= Check(independentSlot.packets.Publish({}).packetNumber == 1,
                    "slots have independent lifetimes");
    passed &= Check(merged.packetNumber == 0,
                    "merge leaves packet ownership to publication tracker");
    passed &= Check(merged.buttons == 0x1010,
                    "digital buttons must be combined");
    passed &= Check(merged.leftTrigger == 32 && merged.rightTrigger == 255,
                    "stronger trigger values must win");
    passed &= Check(merged.thumbLX == 32767 && merged.thumbRX == -9000,
                    "larger-magnitude axes must win without quantization");

    RuntimeInputState keyboardOnly{};
    keyboardOnly.packetNumber = 3;
    keyboardOnly.buttons = 0x2000;
    keyboardOnly.thumbLY = -32768;
    const RuntimeInputState noController =
        MergeRuntimeInputStates({}, keyboardOnly);
    passed &= Check(noController.packetNumber == 0 &&
                        noController.buttons == 0x2000 &&
                        noController.thumbLY == -32768,
                    "keyboard/mouse must remain connected without XInput");

    const RuntimeInputButtonMap identity;
    RuntimeMergedInputPacketTracker packets;
    auto first = packets.Publish(merged);
    passed &= Check(first.packetNumber == 1 &&
                    packets.Publish(merged).packetNumber == 1,
                    "unchanged combined state retains packet");
    auto resetHostCounter = merged;
    resetHostCounter.packetNumber = UINT32_MAX;
    passed &= Check(packets.Publish(resetHostCounter).packetNumber == 1,
                    "host packet reset alone is not a guest payload change");
    auto releasedController = MergeRuntimeInputStates({}, keyboardMouse);
    passed &= Check(packets.Publish(releasedController).packetNumber == 2,
                    "controller unplug releases buttons with fresh merged packet");
    passed &= Check(packets.Publish(merged).packetNumber == 3,
                    "controller reconnect with changed payload advances packet");
    packets.Disconnect();
    passed &= Check(packets.Publish(merged).packetNumber == 4,
                    "complete disconnection invalidates last publication");
    auto analog = merged;
    ++analog.thumbLY;
    passed &= Check(packets.Publish(analog).packetNumber == 5,
                    "analog-only change advances packet");
    RuntimeMergedInputPacketTracker wrapping(UINT32_MAX);
    passed &= Check(wrapping.Publish(merged).packetNumber == 0 &&
                    wrapping.Publish(merged).packetNumber == 0,
                    "packet wrap is defined and stable");
    // Sum collision: old 1+7 == new 0+8 despite different physical states.
    auto collisionController = controller;
    collisionController.packetNumber = 1;
    RuntimeMergedInputPacketTracker collision;
    const auto before = collision.Publish(MergeRuntimeInputStates(collisionController, keyboardMouse));
    auto nextKeyboard = keyboardMouse;
    nextKeyboard.packetNumber = 8;
    const auto after = collision.Publish(MergeRuntimeInputStates({}, nextKeyboard));
    passed &= Check(before.packetNumber != after.packetNumber && before.buttons != after.buttons,
                    "resetting source counters cannot conceal a button release");
    passed &= Check(EncodeRuntimeInputButtonMap(identity) ==
                            kRuntimeInputIdentityButtonMap &&
                        RemapRuntimeInputButtons(0xB413,
                            kRuntimeInputIdentityButtonMap) == 0xB413,
                    "the default controller map must be exact identity");

    RuntimeInputButtonMap swapped;
    swapped.sourceForGuest[static_cast<size_t>(RuntimeInputButton::A)] =
        static_cast<uint8_t>(RuntimeInputButton::B);
    swapped.sourceForGuest[static_cast<size_t>(RuntimeInputButton::B)] =
        static_cast<uint8_t>(RuntimeInputButton::A);
    passed &= Check(RemapRuntimeInputButtons(
                        0x1000, EncodeRuntimeInputButtonMap(swapped)) == 0x2000 &&
                        RemapRuntimeInputButtons(
                            0x2000, EncodeRuntimeInputButtonMap(swapped)) == 0x1000,
                    "A/B remapping must translate physical to guest actions");

    RuntimeInputButtonMap disabled;
    const RuntimeInputState mappedController = RemapRuntimeControllerState(
        controller, EncodeRuntimeInputButtonMap(swapped));
    passed &= Check(mappedController.buttons == 0x2000 &&
                        mappedController.packetNumber == controller.packetNumber &&
                        mappedController.leftTrigger == controller.leftTrigger &&
                        mappedController.thumbLX == controller.thumbLX &&
                        mappedController.thumbRX == controller.thumbRX,
                    "state remapping changes only physical digital buttons");
    RuntimeInputState keyboardA{};
    keyboardA.buttons = 0x1000;
    const auto mixedMapped = MergeRuntimeInputStates(mappedController, keyboardA);
    passed &= Check(mixedMapped.buttons == 0x3000,
                    "keyboard actions must not be remapped with physical buttons");
    const auto unchanged = RemapRuntimeControllerState(controller,
                                                       kRuntimeInputIdentityButtonMap);
    passed &= Check(unchanged.buttons == controller.buttons &&
                        unchanged.packetNumber == controller.packetNumber &&
                        unchanged.thumbLX == controller.thumbLX,
                    "Original controller state remains unchanged");
    disabled.sourceForGuest[static_cast<size_t>(RuntimeInputButton::Y)] =
        static_cast<uint8_t>(RuntimeInputButton::None);
    disabled.sourceForGuest[static_cast<size_t>(RuntimeInputButton::X)] =
        static_cast<uint8_t>(RuntimeInputButton::A);
    passed &= Check(RemapRuntimeInputButtons(
                        0x9000, EncodeRuntimeInputButtonMap(disabled)) == 0x5000,
                    "none and duplicate mappings must be deterministic");
    passed &= Check((RemapRuntimeInputButtons(
                        0x0800, EncodeRuntimeInputButtonMap(swapped)) & 0x0800) != 0,
                    "unknown/reserved physical bits must be preserved");

    if (passed) std::cout << "Runtime input merge-policy tests passed\n";
    return passed ? 0 : 1;
}
