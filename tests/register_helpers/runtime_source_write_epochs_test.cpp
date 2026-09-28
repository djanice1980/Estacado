#include "runtime_source_write_epochs.h"
#include <iostream>
#include <atomic>
#include <thread>
int main() {
    RuntimeSourceWriteEpochs s;
    bool ok = true;
    auto a = s.Watch({100, 32});
    ok &= s.Unchanged(a);
    auto unrelated = s.Begin({132, 4});
    ok &= s.Unchanged(a); s.End(unrelated);
    auto writer = s.Begin({99, 2});
    ok &= !s.Unchanged(a) && !s.Watch({100, 4}).id && !s.Reset();
    auto nested = s.Begin({101, 4});
    s.End(writer);
    ok &= !s.Watch({101, 4}).id;
    s.End(nested);
    ok &= !s.Unchanged(a); // Same bytes restored later cannot revive old identity.
    auto b = s.Watch({100, 32}); ok &= s.Unchanged(b) && b.id > a.id;
    ok &= s.Reset() && !s.Unchanged(b);
    auto c = s.Watch({100, 32}); ok &= c.id > b.id;
    s.End(writer); ok &= !s.Unchanged(c); // Duplicate completion fails closed.
    ok &= !s.Reset();
    RuntimeSourceWriteEpochs capacity;
    for (int i = 0; i < 512; ++i) ok &= capacity.Watch({uint32_t(i), 1}).id != 0;
    ok &= !capacity.Watch({1000, 1}).id;
    ok &= capacity.Reset();
    auto d = capacity.Watch({100, 1});
    ok &= !capacity.Begin({0x1FFFFFFF, 2}) && !capacity.Unchanged(d);
    ok &= !capacity.Reset();
    RuntimeSourceWriteEpochs overflow;
    uint64_t writers[64]{};
    for (auto& id : writers) { id = overflow.Begin({100, 1}); ok &= id != 0; }
    ok &= !overflow.Begin({200, 1}) && !overflow.Watch({200, 1}).id;
    for (auto id : writers) overflow.End(id);
    ok &= !overflow.Reset();
    RuntimeSourceWriteEpochs concurrent;
    auto before = concurrent.Watch({100, 32});
    std::atomic<bool> started{}, finish{};
    std::thread thread([&] {
        auto id = concurrent.Begin({120, 4});
        started.store(true, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) std::this_thread::yield();
        concurrent.End(id);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    ok &= !concurrent.Unchanged(before) && !concurrent.Watch({120, 4}).id;
    finish.store(true, std::memory_order_release); thread.join();
    ok &= !concurrent.Unchanged(before) && concurrent.Watch({100, 32}).id != 0;
    RuntimeSourceWriteEpochs cursor;
    auto forward = cursor.WatchForwardU32({100, 4}, 240, 4096);
    auto ordinary = cursor.Watch({100, 4});
    auto append = cursor.Begin({100, 4}, 256);
    ok &= !cursor.Unchanged(forward) && !cursor.WatchForwardU32({100, 4}, 256, 4096).id;
    cursor.End(append);
    ok &= cursor.Unchanged(forward) && !cursor.Unchanged(ordinary);
    auto reset = cursor.Begin({100, 4}, 0); cursor.End(reset);
    auto reuse = cursor.Begin({100, 4}, 512); cursor.End(reuse);
    ok &= !cursor.Unchanged(forward); // Reset+reallocation never revives old identity.
    for (auto value : {240u, 239u, 4097u, UINT32_MAX}) {
        auto t = cursor.WatchForwardU32({100, 4}, 240, 4096);
        auto w = cursor.Begin({100, 4}, value); cursor.End(w);
        ok &= !cursor.Unchanged(t);
    }
    for (auto range : {RuntimeSourceWriteEpochs::Range{100, 1}, {102, 2}, {99, 8}, {100, 4}}) {
        auto t = cursor.WatchForwardU32({100, 4}, 240, 4096);
        auto w = cursor.Begin(range); cursor.End(w);
        ok &= !cursor.Unchanged(t);
    }
    auto t = cursor.WatchForwardU32({100, 4}, 240, 4096);
    auto outer = cursor.Begin({100, 4}, 256);
    auto inner = cursor.Begin({100, 4}, 272);
    cursor.End(inner); cursor.End(outer);
    ok &= !cursor.Unchanged(t); // Nested overlapping stores cannot certify order.
    ok &= !cursor.WatchForwardU32({101, 4}, 240, 4096).id;
    ok &= !cursor.WatchForwardU32({100, 4}, 0, 4096).id;
    ok &= !cursor.WatchForwardU32({100, 4}, 4097, 4096).id;
    // Page rejection is conservative: shared pages, crossings and the final
    // physical byte must still take the exact overlap path when necessary.
    RuntimeSourceWriteEpochs pages;
    auto crossing = pages.Watch({0xffff, 2});
    auto neighbor = pages.Watch({0x10004, 4});
    auto miss = pages.Begin({0x10002, 2}); pages.End(miss);
    ok &= pages.Unchanged(crossing) && pages.Unchanged(neighbor);
    auto hit = pages.Begin({0x10000, 1}); pages.End(hit);
    ok &= pages.Retired(crossing) && pages.Unchanged(neighbor);
    hit = pages.Begin({0x10007, 1}); pages.End(hit);
    ok &= pages.Retired(neighbor);
    auto final_byte = pages.Watch({0x1fffffff, 1});
    hit = pages.Begin({0x1ffffffe, 2}); pages.End(hit);
    ok &= pages.Retired(final_byte);
    auto whole = pages.Watch({0, 0x20000000});
    hit = pages.Begin({0x100000, 4}); pages.End(hit);
    ok &= pages.Retired(whole);
    ok &= pages.Reset();
    auto after_reset = pages.Watch({0xffff, 2});
    hit = pages.Begin({0xffff, 1}); pages.End(hit);
    ok &= pages.Retired(after_reset) && after_reset.id > whole.id;
    // An unobserved page still registers an active write. A later watch must
    // not be admitted inside it until the writer completes.
    auto unobserved = pages.Begin({0x20000, 4});
    ok &= !pages.Watch({0x20000, 4}).id;
    pages.End(unobserved);
    auto admitted = pages.Watch({0x20000, 4});
    ok &= admitted.id && pages.Unchanged(admitted);
    // Shared dependency reuse must not inflate page counts or hide a new watch.
    for (int i = 0; i < 1000; ++i) ok &= pages.Watch({0x20000, 4}).id == admitted.id;
    hit = pages.Begin({0x20000, 4}); pages.End(hit);
    ok &= pages.Retired(admitted);
    auto fresh = pages.WatchForwardU32({0x20000, 4}, 4, 128);
    hit = pages.Begin({0x20000, 4}, 8); pages.End(hit);
    ok &= pages.Unchanged(fresh);
    hit = pages.Begin({0x20000, 4}, 4); pages.End(hit);
    ok &= pages.Retired(fresh);
    // Evidence describes the actual invalidating write, never a later write or
    // the final cursor value. Eviction/reset may lose evidence, not retirement.
    RuntimeSourceWriteEpochs evidence;
    auto e = evidence.WatchForwardU32({400, 4}, 100, 1000);
    auto ew = evidence.Begin({400, 4}, 120); evidence.End(ew);
    ok &= evidence.Unchanged(e) && !evidence.ExplainRetirement(e).reasons;
    ew = evidence.Begin({400, 4}, 120); evidence.End(ew);
    auto reason = evidence.ExplainRetirement(e);
    ok &= reason.reasons == RuntimeSourceWriteEpochs::EqualValue && reason.previous == 120 &&
        reason.stored == 120 && reason.watched.address == 400 && reason.write.bytes == 4;
    ew = evidence.Begin({400, 4}, 0); evidence.End(ew);
    ok &= evidence.ExplainRetirement(e).reasons == RuntimeSourceWriteEpochs::EqualValue;
    for (auto pair : {std::pair{0u, RuntimeSourceWriteEpochs::BackwardValue},
                      std::pair{1001u, RuntimeSourceWriteEpochs::BeyondLimit}}) {
        auto next = evidence.WatchForwardU32({400, 4}, 100, 1000);
        ew = evidence.Begin({400, 4}, pair.first); evidence.End(ew);
        ok &= evidence.Retired(next) && evidence.ExplainRetirement(next).reasons == pair.second;
    }
    auto partial = evidence.WatchForwardU32({400, 4}, 100, 1000);
    ew = evidence.Begin({402, 2}); evidence.End(ew);
    ok &= evidence.ExplainRetirement(partial).reasons ==
        (RuntimeSourceWriteEpochs::Untyped | RuntimeSourceWriteEpochs::DifferentRange);
    auto nested_ticket = evidence.WatchForwardU32({400, 4}, 100, 1000);
    auto ew_outer = evidence.Begin({400, 4}, 120);
    ew = evidence.Begin({400, 4}, 140); evidence.End(ew); evidence.End(ew_outer);
    ok &= evidence.ExplainRetirement(nested_ticket).reasons == RuntimeSourceWriteEpochs::OverlappingWrite;
    for (unsigned i = 0; i < 128; ++i) {
        auto next = evidence.WatchForwardU32({400, 4}, 100, 1000);
        ew = evidence.Begin({400, 4}, 0); evidence.End(ew);
        ok &= evidence.ExplainRetirement(next).reasons == RuntimeSourceWriteEpochs::BackwardValue;
    }
    ok &= !evidence.ExplainRetirement(e).reasons && evidence.Retired(e);
    ok &= evidence.Reset() && !evidence.ExplainRetirement(nested_ticket).reasons;
    auto fault_ticket = evidence.WatchForwardU32({400, 4}, 100, 1000);
    evidence.Begin({0x20000000, 4});
    ok &= evidence.ExplainRetirement(fault_ticket).reasons == RuntimeSourceWriteEpochs::TrackingFault;
    ok &= !evidence.ExplainRetirement({}).reasons;
    std::cout << (ok ? "write epochs PASS\n" : "write epochs FAIL\n");
    return ok ? 0 : 1;
}
