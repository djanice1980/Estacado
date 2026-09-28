#include "runtime_owned_packet_read.h"
#include <iostream>
#include <memory>
using namespace runtime_owned_camera_source;
int main() {
    bool ok = true;
    RuntimeSourceMemoryCoordinator coordinator;
    ok &= coordinator.ConfigureAtStartup(true);
    auto packets = std::make_unique<PacketConstantStore>();
    ConstantPublication constant; constant.id = 9; constant.source.token = {71, 2};
    for (uint32_t i = 0; i < 32; ++i) constant.source.packed[i] = 0x3F000000 + i;
    for (uint32_t i = 0; i < 16; ++i) constant.source.camera_current[i] = 0x3F800000 + i;
    uint32_t memory[33]{0x001F4030};
    for (uint32_t i = 0; i < 32; ++i) memory[i + 1] = constant.source.packed[i];
    uint32_t reads = 0, payload[1024]{};
    auto read = [&](uint32_t address) { ++reads; return memory[(address - 0x3000) / 4]; };
    rex::graphics::pc_owned_camera_packet::Source source;
    // Exercise the production CP gate: wrapped, out-of-range, unrelated and
    // default-off packets must not invoke a host callback at all.
    using namespace rex::graphics::pc_owned_camera_packet;
    uint32_t calls = 0;
    Callbacks callback{&calls, [](void* context, uint32_t, uint32_t, uint32_t,
                                uint32_t*, rex::graphics::pc_owned_camera_packet::Source*) noexcept {
        ++*static_cast<uint32_t*>(context); return true;
    }};
    ok &= CopyContiguous(callback, 0x3000, memory[0], 32, 4, 132, payload, &source);
    ok &= calls == 1;
    ok &= !CopyContiguous(callback, 0x3000, memory[0], 32, 0, 132, payload, &source);
    ok &= !CopyContiguous(callback, 0x3000, memory[0], 32, 8, 132, payload, &source);
    ok &= !CopyContiguous(callback, 0x3000, memory[0], 32, 136, 132, payload, &source);
    ok &= !CopyContiguous(callback, 0x3000, 0x001F4000, 32, 4, 132, payload, &source);
    ok &= !CopyContiguous(callback, 0x1FFFFF80, memory[0], 32, 4, 132, payload, &source);
    ok &= !CopyContiguous({}, 0x3000, memory[0], 32, 4, 132, payload, &source);
    ok &= calls == 1;
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        constant.write = snapshot.Watch({0x2000, 128});
        ok &= packets->Publish(snapshot, constant, 0, 0x3000, memory, 33, 1, 32) != 0;
        for (int replay = 0; replay < 2; ++replay) {
            ok &= RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3000, memory[0], 32, payload, &source, read);
            ok &= source.publication == 71 && source.item == 2 && source.Covers(0x4030) && source.Covers(0x404F);
            ok &= !source.Covers(0x402F) && !source.Covers(0x4050);
            for (uint32_t i = 0; i < 32; ++i) ok &= payload[i] == constant.source.packed[i];
            for (uint32_t i = 0; i < 16; ++i) ok &= source.camera_current[i] == constant.source.camera_current[i];
        }
        const auto before = reads;
        ok &= !RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3000, memory[0], 31, payload, &source, read);
        ok &= !RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3004, memory[0], 32, payload, &source, read);
        ok &= !RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3000, memory[0] | 0x8000, 32, payload, &source, read);
        ok &= reads == before; // Unknown shapes must not touch unqualified memory.
        memory[0] ^= 1;
        ok &= !RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3000, 0x001F4030, 32, payload, &source, read);
        memory[0] ^= 1;
    }
    // Owned output survives subsequent writes; a new lookup must fail closed.
    { RuntimeSourceMemoryCoordinator::Write write(coordinator, {0x3000, 132}); memory[1] = 0; }
    ok &= payload[0] == constant.source.packed[0];
    {
        RuntimeSourceMemoryCoordinator::Snapshot snapshot(coordinator);
        const auto before = reads;
        ok &= !RuntimeReadOwnedCameraPacket(*packets, snapshot, 0x3000, memory[0], 32, payload, &source, read);
        ok &= reads == before && source.publication == 71;
    }
    std::cout << (ok ? "owned packet read PASS\n" : "owned packet read FAIL\n");
    return ok ? 0 : 1;
}
