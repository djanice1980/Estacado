#include "runtime_owned_camera_source.h"
#include <iostream>
#include <memory>

using namespace runtime_owned_camera_source;
int main() {
    bool ok = true;
    auto store = std::make_unique<Store>();
    Publication input;
    input.job = 1; input.render_owner = 2; input.camera = 3;
    input.buffer = 0x1000; input.compact = 0x1094; input.count = 2;
    input.items[0] = {0x10C0, 10, 11, 12};
    input.items[1] = {0x10F0, 20, 21, 22};
    input.camera_current[0] = 0x80000000; // Retain bits, including signed zero.
    input.packed[31] = 0x7FC00001; // No numerical reinterpretation at ownership.
    input.backing = {71, 0x1000, 4096};
    const auto first = store->Publish(input);
    Source a{}, b{};
    ok &= first != 0 && store->Copy({first, 0}, a) && store->Copy({first, 1}, b);
    ok &= a.native_item.primary == 10 && b.native_item.primary == 20;
    ok &= a.camera_current[0] == 0x80000000 && a.packed[31] == 0x7FC00001;
    ok &= a.backing.generation == 71 && a.backing.physical_base == 0x1000 && a.backing.bytes == 4096;
    // Caller storage changes cannot mutate the snapshot. Replaying is a read,
    // never consumption, including after a new publication reuses guest bytes.
    input.camera_current[0] = 42;
    input.backing.generation = 72;
    input.items[0].primary = 30;
    const auto second = store->Publish(input);
    ok &= second > first && store->Copy({first, 0}, b);
    ok &= b.camera_current == a.camera_current && b.native_item.primary == 10;
    ok &= b.backing.generation == 71;
    ok &= store->Copy({second, 0}, b) && b.camera_current[0] == 42;
    ok &= !store->Copy({0, 0}, b) && !store->Copy({first, 2}, b);
    ok &= b.token.publication == second; // Failed lookup doesn't corrupt output.
    // A reset invalidates handles but cannot revoke already-owned value copies.
    store->Clear();
    ok &= !store->Copy({first, 0}, b) && a.native_item.primary == 10;
    const auto after_reset = store->Publish(input);
    ok &= after_reset > second && !store->Copy({second, 0}, b);
    for (size_t i = 1; i < Store::kCapacity; ++i) ok &= store->Publish(input) != 0;
    ok &= store->Publish(input) == 0 && store->Size() == Store::kCapacity;
    ok &= store->Copy({after_reset, 0}, b); // Saturation cannot evict a source.
    store->Clear();
    auto bad = input;
    bad.count = 65; ok &= store->Publish(bad) == 0;
    bad = input; bad.items[1].record += 4; ok &= store->Publish(bad) == 0;
    bad = input; bad.items[0].primary = 0; ok &= store->Publish(bad) == 0;
    bad = input; bad.buffer = 0xFFFFFF00; bad.compact = 0xFFFFFF94;
    bad.count = 3; ok &= store->Publish(bad) == 0;
    ok &= store->Size() == 0;
    std::cout << (ok ? "owned camera source PASS\n" : "owned camera source FAIL\n");
    return ok ? 0 : 1;
}
