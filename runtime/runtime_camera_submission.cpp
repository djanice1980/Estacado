#include "runtime_camera.h"
#include "runtime_owned_source_scope.h"
#include "runtime_source_memory_coordinator.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_82299EA0);
extern "C" PPC_FUNC(__imp__sub_8224A2E8);
extern "C" PPC_FUNC(__imp__sub_828714A0);
extern "C" PPC_FUNC(__imp__sub_8285D468);

PPC_FUNC(sub_828714A0) {
    if (!RuntimeGuestSourceCoordinator().TrackingEnabled()) {
        __imp__sub_828714A0(ctx, base); return;
    }
    RuntimeRunOwnedCameraPacketWrite(ctx, base, __imp__sub_828714A0);
}

PPC_FUNC(sub_8285D468) {
    if (!RuntimeGuestSourceCoordinator().TrackingEnabled()) {
        __imp__sub_8285D468(ctx, base); return;
    }
    RuntimeRunOwnedCameraPacketFlush(ctx, base, __imp__sub_8285D468);
}

PPC_FUNC(sub_8224A2E8) {
    if (!RuntimeGuestSourceCoordinator().TrackingEnabled()) {
        __imp__sub_8224A2E8(ctx, base);
        return;
    }
    RuntimeRunOwnedCameraConstantCopy(ctx, base, __imp__sub_8224A2E8);
}

// Strong wrapper around the unchanged generated implementation. The binder
//82299E30 can be skipped;82299EA0 still gets the selected record inr3.
PPC_FUNC(sub_82299EA0) {
    if (!RuntimeGuestSourceCoordinator().TrackingEnabled()) {
        __imp__sub_82299EA0(ctx, base);
        return;
    }
    runtime_owned_camera_source::Source source;
    const bool selected = RuntimeSelectOwnedCameraSource(ctx.r3.u32, source);
    runtime_owned_camera_source::SubmissionScope scope(&ctx,
        selected ? source.token : runtime_owned_camera_source::Token{});
    __imp__sub_82299EA0(ctx, base);
}
