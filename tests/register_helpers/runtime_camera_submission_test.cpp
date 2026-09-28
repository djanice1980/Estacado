#include "runtime_camera.h"
#include "runtime_owned_source_scope.h"
#include "runtime_source_memory_coordinator.h"
#include "ppc_recomp_shared.h"
#include <iostream>
#include <thread>

using namespace runtime_owned_camera_source;
bool ok = true;
uint32_t calls = 0;
uint32_t selections = 0;
bool disabled = false;
uint32_t constant_transactions = 0, constant_calls = 0;
uint32_t packet_transactions = 0, packet_calls = 0, flush_transactions = 0, flush_calls = 0;
void RuntimeRunOwnedCameraPacketWrite(PPCContext& ctx, uint8_t* base,
                                     void (*native)(PPCContext&, uint8_t*)) {
    ++packet_transactions; native(ctx, base);
}
void RuntimeRunOwnedCameraPacketFlush(PPCContext& ctx, uint8_t* base,
                                     void (*native)(PPCContext&, uint8_t*)) {
    ++flush_transactions; native(ctx, base);
}
__attribute__((alias("__imp__sub_828714A0"))) PPC_WEAK_FUNC(sub_828714A0);
PPC_FUNC_IMPL(__imp__sub_828714A0) { ++packet_calls; ctx.r5.u32 = 888; }
__attribute__((alias("__imp__sub_8285D468"))) PPC_WEAK_FUNC(sub_8285D468);
PPC_FUNC_IMPL(__imp__sub_8285D468) { ++flush_calls; ctx.r6.u32 = 999; }
void RuntimeRunOwnedCameraConstantCopy(PPCContext& context, uint8_t* base,
                                      void (*native_copy)(PPCContext&, uint8_t*)) {
    ++constant_transactions;
    native_copy(context, base);
}
__attribute__((alias("__imp__sub_8224A2E8"))) PPC_WEAK_FUNC(sub_8224A2E8);
PPC_FUNC_IMPL(__imp__sub_8224A2E8) { ++constant_calls; ctx.r4.u32 = 777; }
bool RuntimeSelectOwnedCameraSource(uint32_t record, Source& output) {
    ++selections;
    if (record != 1) return false;
    output.token = {71, 2}; return true;
}
// Same weak-alias shape as generated code. Calls must resolve to the actual
// runtime wrapper, which forwards here exactly once with registers intact.
__attribute__((alias("__imp__sub_82299EA0"))) PPC_WEAK_FUNC(sub_82299EA0);
PPC_FUNC_IMPL(__imp__sub_82299EA0) {
    ++calls;
    const auto token = SubmissionScope::Current(&ctx);
    if (disabled) {
        ok &= token.publication == 0;
        ctx.r3.u32 = 321;
        return;
    }
    if (ctx.r3.u32 == 1) {
        ok &= token.publication == 71 && token.item == 2;
        PPCContext other{};
        ok &= SubmissionScope::Current(&other).publication == 0;
        std::thread isolated([&] { ok &= SubmissionScope::Current(&ctx).publication == 0; });
        isolated.join();
        ctx.r3.u32 = 2;
        sub_82299EA0(ctx, base);
        ok &= SubmissionScope::Current(&ctx).publication == 71;
        ctx.r3.u32 = 123; // Guest results must pass through the wrapper.
    } else {
        ok &= token.publication == 0; // Unknown nested item hides outer source.
    }
}
int main(int argc, char**) {
    disabled = argc > 1;
    ok &= RuntimeGuestSourceCoordinator().ConfigureAtStartup(!disabled);
    PPCContext context{};
    alignas(32) uint8_t base[32]{};
    context.r3.u32 = 1;
    sub_82299EA0(context, base);
    sub_8224A2E8(context, base);
    ok &= constant_calls == 1 && constant_transactions == (disabled ? 0u : 1u);
    ok &= context.r4.u32 == 777;
    sub_828714A0(context, base);
    sub_8285D468(context, base);
    ok &= packet_calls == 1 && flush_calls == 1 && context.r5.u32 == 888 && context.r6.u32 == 999;
    ok &= packet_transactions == (disabled ? 0u : 1u) && flush_transactions == (disabled ? 0u : 1u);
    if (disabled) {
        ok &= calls == 1 && selections == 0 && context.r3.u32 == 321;
        std::cout << (ok ? "camera submission disabled PASS\n" : "camera submission disabled FAIL\n");
        return ok ? 0 : 1;
    }
    ok &= calls == 2 && context.r3.u32 == 123;
    ok &= SubmissionScope::Current(&context).publication == 0;
    // Test C++ scope unwinding without throwing through a generated extern-C
    // guest function (the established /EHsc build treats those as nonthrowing).
    try { SubmissionScope scope(&context, {88, 0}); throw 3; }
    catch (int value) { ok &= value == 3; }
    ok &= calls == 2 && SubmissionScope::Current(&context).publication == 0;
    std::cout << (ok ? "camera submission scope PASS\n" : "camera submission scope FAIL\n");
    return ok ? 0 : 1;
}
