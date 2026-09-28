#include "runtime_owned_camera_packets.h"
#include <iostream>
#include <memory>
#include <vector>
using namespace runtime_owned_camera_source;
using Coordinator = RuntimeSourceMemoryCoordinator;
int main() {
    bool ok = true;
    Coordinator owner; ok &= owner.ConfigureAtStartup(true);
    auto store = std::make_unique<PacketConstantStore>();
    ConstantPublication constant; constant.id = 9; constant.physical = 0x2000;
    constant.source.token = {71, 0};
    for (uint32_t i = 0; i < 32; ++i) constant.source.packed[i] = i + 0x3F800000;
    constant.source.packed[0] = 0x80000000; // Preserve signed zero exactly.
    std::vector<uint32_t> packet(33); packet[0] = 0x001F4030;
    for (uint32_t i = 0; i < 32; ++i) packet[i + 1] = constant.source.packed[i];
    PacketConstantPublication copy;
    uint64_t first{};
    {
        Coordinator::Snapshot snapshot(owner);
        constant.write = snapshot.Watch({0x2000, 128});
        unsigned visits = 0;
        ok &= VisitNativeConstantPackets(packet.data(), 33, [&](auto offset, auto reg, auto words) {
            ++visits; ok &= offset == 0 && reg == 0x4030 && words == 32;
        }) && visits == 1;
        auto bad = packet; bad.push_back(0xC0000000);
        visits = 0;
        ok &= !VisitNativeConstantPackets(bad.data(), uint32_t(bad.size()),
            [&](auto, auto, auto) { ++visits; }) && !visits;
        for (auto header : {0x001FC030u, 0x001F4031u, 0x001F43F0u, 0x000E4030u}) {
            bad = packet; bad[0] = header;
            ok &= !VisitNativeConstantPackets(bad.data(), 33, [](auto, auto, auto) {});
        }
        auto padded = packet; padded.insert(padded.begin(), 0x80000000);
        ok &= VisitNativeConstantPackets(padded.data(), 34, [](auto, auto, auto) {});
        first = store->Publish(snapshot, constant, 0, 0x3000, packet.data(), 33, 1, 32);
        ok &= first && store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy);
        ok &= store->Copy(snapshot, 0x3000, 132, 0x4040, 16, copy);
        ok &= copy.source_word == 16 && copy.payload_offset == 68;
        ok &= !store->Copy(snapshot, 0x3004, 128, 0x4030, 32, copy);
        ok &= !store->Copy(snapshot, 0x3000, 132, 0x402F, 1, copy);
        auto mismatch = packet; mismatch[1] = 0;
        ok &= !store->Publish(snapshot, constant, 0, 0x4000, mismatch.data(), 33, 1, 32);
        ok &= !store->Publish(snapshot, constant, 1, 0x4000, packet.data(), 33, 1, 31);
    }
    { Coordinator::Write retire_scratch(owner, {0x2000, 128}); }
    {
        Coordinator::Snapshot snapshot(owner);
        for (int replay = 0; replay < 2; ++replay)
            ok &= store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy) && copy.id == first;
        ok &= !store->Publish(snapshot, constant, 0, 0x4000, packet.data(), 33, 1, 32);
    }
    { Coordinator::Write header_overwrite(owner, {0x3000, 4}); }
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= !store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy) && copy.id == first;
        constant.write = snapshot.Watch({0x2000, 128});
        ok &= store->Publish(snapshot, constant, 0, 0x3000, packet.data(), 33, 1, 32) > first;
        ok &= store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy);
        ok &= store->Publish(snapshot, constant, 0, 0x3000, packet.data(), 33, 1, 32) != 0;
        ok &= !store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy); // Ambiguous, not newest.
        store->Clear();
        ok &= !store->Copy(snapshot, 0x3000, 132, 0x4030, 32, copy);
    }
    // Slow-writer split: each completed16word chunk has its own packet ticket.
    {
        Coordinator::Snapshot snapshot(owner);
        std::vector<uint32_t> half(17); half[0] = 0x000F4040;
        for (uint32_t i = 0; i < 16; ++i) half[i + 1] = constant.source.packed[i + 16];
        ok &= VisitNativeConstantPackets(half.data(), 17, [](auto, auto, auto) {});
        ok &= store->Publish(snapshot, constant, 16, 0x5000, half.data(), 17, 1, 16) != 0;
        ok &= store->Copy(snapshot, 0x5000, 68, 0x4040, 16, copy);
        ok &= copy.source_word == 16 && copy.source.token.publication == 71;
        ok &= !store->Publish(snapshot, constant, 16, 0x1FFFFFF0, half.data(), 17, 1, 16);
        ok &= !store->Publish(snapshot, constant, 16, 0x5001, half.data(), 17, 1, 16);
        ok &= !store->Publish(snapshot, constant, 16, 0x5000, half.data(), 17, 1, 17);
    }
    { Coordinator::Write payload_overwrite(owner, {0x5004, 1}); }
    {
        Coordinator::Snapshot snapshot(owner);
        ok &= !store->Copy(snapshot, 0x5000, 68, 0x4040, 16, copy);
    }
    // Capacity saturation fails closed without evicting an earlier live packet.
    Coordinator fresh; ok &= fresh.ConfigureAtStartup(true);
    auto bounded = std::make_unique<PacketConstantStore>();
    {
        Coordinator::Snapshot snapshot(fresh);
        constant.write = snapshot.Watch({0x2000, 128});
        const auto retained = bounded->Publish(snapshot, constant, 0, 0x6000, packet.data(), 33, 1, 32);
        for (size_t i = 1; i < PacketConstantStore::kCapacity; ++i)
            ok &= bounded->Publish(snapshot, constant, 0, 0x7000, packet.data(), 33, 1, 32) != 0;
        // Identical live watches share a dependency; the publication store fills first.
        ok &= !bounded->Publish(snapshot, constant, 0, 0x7000, packet.data(), 33, 1, 32);
        ok &= bounded->Copy(snapshot, 0x6000, 132, 0x4030, 32, copy) && copy.id == retained;
    }
    std::cout << (ok ? "owned camera packets PASS\n" : "owned camera packets FAIL\n");
    return ok ? 0 : 1;
}
