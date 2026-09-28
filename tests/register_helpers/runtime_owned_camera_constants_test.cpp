#include "runtime_owned_camera_constants.h"
#include <iostream>
#include <memory>
using namespace runtime_owned_camera_source;
using Coordinator = RuntimeSourceMemoryCoordinator;
int main() {
    bool ok = true;
    Coordinator owner;
    ok &= owner.ConfigureAtStartup(true);
    auto store = std::make_unique<ConstantStore>();
    Source source;
    source.token = {71, 2}; source.packed[0] = 0x80000000;
    source.camera_current[0] = 0x3F800000;
    const auto watch = [&] {
        Coordinator::Snapshot snapshot(owner);
        return snapshot.Watch({0x2000, 128});
    };
    ConstantPublication copy;
    const auto read = [&](uint32_t address, uint32_t bytes) {
        Coordinator::Snapshot snapshot(owner);
        return store->Copy(snapshot, address, bytes, copy);
    };
    const auto first = store->Publish(source, 0x2000, watch());
    ok &= first && read(0x2000, 64) && copy.source.token.publication == 71;
    ok &= read(0x2040, 64) && copy.id == first; // Two packet groups, one source.
    ok &= read(0x207C, 4) && read(0x2000, 128);
    ok &= !read(0x207C, 8) && !read(0x1FFC, 4) && !read(0x2001, 4);
    ok &= !read(0x2000, 0) && !read(0x2000, 3) && copy.id == first;
    // Native source retirement doesn't revoke a completed owned publication.
    { Coordinator::Write retire_native(owner, {0x1000, 128}); }
    source.packed[0] = 7;
    ok &= read(0x2000, 128) && copy.source.packed[0] == 0x80000000;
    // Destination reuse does revoke copying, even if restored to identical bits.
    { Coordinator::Write overwrite(owner, {0x207F, 1}); }
    ok &= !read(0x2000, 64) && copy.id == first;
    const auto second = store->Publish(source, 0x2000, watch());
    ok &= second > first && read(0x2000, 128) && copy.source.packed[0] == 7;
    ok &= store->Publish(source, 0x2000, watch()) != 0;
    ok &= !read(0x2000, 128); // Unknown if more than one publication qualifies.
    store->Clear();
    ok &= !read(0x2000, 128) && copy.id == second;
    ok &= store->Publish(source, 0x2000, {}) == 0;
    ok &= store->Publish(source, 0x1FFFFFF0, watch()) == 0;
    const auto retained = watch();
    for (size_t i = 0; i < ConstantStore::kCapacity; ++i)
        ok &= store->Publish(source, 0x2000, retained) != 0;
    ok &= store->Publish(source, 0x2000, retained) == 0;
    std::cout << (ok ? "owned camera constants PASS\n" : "owned camera constants FAIL\n");
    return ok ? 0 : 1;
}
