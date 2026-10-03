#include "../../runtime/runtime_camera_policy.h"
#include "../../runtime/runtime_motion_origins_policy.h"
#include "../../runtime/runtime_motion_delta_policy.h"
#include "../../runtime/runtime_temporal_camera_policy.h"

#include <cstring>
#include <iostream>

int main() {
    bool ok = true;
    {
        uint32_t bytes = 123;
        ok &= RuntimePacketSegmentBytes(0x10000,0x1003C,bytes) && bytes == 64;
        ok &= RuntimePacketSegmentBytes(0x10000,0xFFFC,bytes) && bytes == 0;
        ok &= !RuntimePacketSegmentBytes(0x10004,0x1003C,bytes) && bytes == 0;
        ok &= !RuntimePacketSegmentBytes(0x10000,0xFFF8,bytes);
        ok &= !RuntimePacketSegmentBytes(0xFFFFFFE0,0xFFFFFFFC,bytes);
        ok &= !RuntimePacketSegmentBytes(0x10000,0x50000,bytes);
        RuntimeCameraCaptureWindow window;
        RuntimePacketSourceBudget packets;
        const void* owner = &packets;
        ok &= !packets.Select(window,owner);
        window.manual=true;
        ok &= window.SelectCopy(1,owner);
        ok &= !packets.Select(window,&window);
        for (unsigned i=0;i<128;++i) ok &= packets.Select(window,owner);
        ok &= !packets.Select(window,owner) && packets.records==128 && packets.dropped==1;
        window.generation=2;
        ok &= !packets.Select(window,owner) && packets.dropped==1;
        RuntimePacketSourceBudget memory;
        ok &= !memory.Reserve(262145) && memory.bytes==0;
        for (unsigned i=0;i<32;++i) ok &= memory.Reserve(262144);
        ok &= memory.bytes==8388608 && !memory.Reserve(4) && memory.bytes==8388608;
        window.generation=1; window.copies=4;
        ok &= packets.FinishBeforeCopy(window,owner) && !packets.Select(window,owner);
    }

    ok &= RuntimeMotionDeltaFunction(0x825EB6B8u, 0) == RuntimeMotionDeltaSite::CameraSource;
    ok &= RuntimeMotionDeltaFunction(0x82498F68u, 0x825E572Cu) == RuntimeMotionDeltaSite::ViewCallback;
    ok &= RuntimeMotionDeltaFunction(0x82498F68u, 0x825E58B8u) == RuntimeMotionDeltaSite::ViewCallback;
    ok &= RuntimeMotionDeltaFunction(0x82498F68u, 0) == RuntimeMotionDeltaSite::None;
    for (uint32_t caller : {0x82499664u, 0x824998F4u, 0x82499A38u})
        ok &= RuntimeMotionDeltaFunction(0x82114838u, caller) == RuntimeMotionDeltaSite::Inverse;
    ok &= RuntimeMotionDeltaFunction(0x82114838u, 0x825E5518u) == RuntimeMotionDeltaSite::None;
    ok &= RuntimeMotionDeltaFunction(0x82498B08u, 0x82499A28u) == RuntimeMotionDeltaSite::HistoryStore;
    ok &= RuntimeMotionDeltaFunction(0x82498B08u, 0) == RuntimeMotionDeltaSite::None;
    ok &= RuntimeMotionDeltaItem(0x1000u, 3u, 4u, 0x11E0u);
    ok &= !RuntimeMotionDeltaItem(0x1000u, 2u, 4u, 0x11E0u);
    ok &= !RuntimeMotionDeltaItem(0x1000u, 3u, 4u, 0x10F4u);
    ok &= !RuntimeMotionDeltaItem(0x1000u, 5u, 4u, 0x1000u);
    ok &= !RuntimeMotionDeltaItem(0xFFFFFF00u, 2u, 2u, 0xFFFFFFF0u);
    ok &= RuntimeMotionDeltaKeyBucket(0xFFFFu, 0xFFFFu, 0xFFFFFFF0u) == 253u;
    ok &= RuntimeMotionDeltaKeyBucket(1u, 2u, 0x12345678u) == 106u;
    ok &= RuntimeMotionDeltaHistoryRecord(0x1000u, 3u, 2u) == 0x10C0u;
    ok &= !RuntimeMotionDeltaHistoryRecord(0x1000u, 3u, 3u);
    ok &= !RuntimeMotionDeltaHistoryRecord(0x1000u, 32768u, 1u);
    ok &= !RuntimeMotionDeltaHistoryRecord(0xFFFFFFC0u, 1u, 0u);
    RuntimeTransformObservationTicket ticket;
    int owner{}, other{};
    RuntimeCameraCaptureWindow selected;
    selected.manual = true;
    for (unsigned i = 0; i < 1000; ++i)
        ok &= !selected.SelectCopy(0, &owner); // startup cannot spend the budget
    ok &= selected.copies == 0 && !selected.SelectBuilder(&owner);
    for (unsigned copy = 0; copy < 16; ++copy) {
        ok &= selected.SelectCopy(7, &owner);
        ok &= !selected.SelectBuilder(&other);
        for (unsigned builder = 0; builder < 16; ++builder)
            ok &= selected.SelectBuilder(&owner);
        ok &= !selected.SelectBuilder(&owner);
        selected.FrameEnter();
        ok &= !selected.SelectBuilder(&owner);
    }
    ok &= selected.copies == 16 && selected.generation == 7;
    ok &= !selected.SelectCopy(7, &owner) && !selected.SelectCopy(8, &owner);
    RuntimeCameraCaptureWindow superseded;
    superseded.manual = true;
    ok &= superseded.SelectCopy(9, &owner);
    ok &= !superseded.SelectCopy(10, &owner);
    ok &= !superseded.SelectBuilder(&owner); // cannot mix different requests
    RuntimeCameraCaptureWindow startup;
    ok &= startup.SelectCopy(0, &owner) && startup.generation == 0;
    ok &= !startup.SelectCopy(0, nullptr) && !startup.SelectBuilder(&owner);
    RuntimeMotionSourceBudget sources;
    RuntimeCameraCaptureWindow source_window;
    ok &= !sources.Select(source_window, &owner) && !sources.FinishBeforeCopy(source_window, &owner);
    source_window.manual = true;
    source_window.SelectCopy(7, &owner);
    ok &= !sources.Select(source_window, &other) && !sources.Select(source_window, nullptr);
    ok &= sources.Select(source_window, &owner) && sources.generation == 7 && sources.records == 1;
    ok &= !sources.FinishBeforeCopy(source_window, &owner);
    source_window.FrameEnter();
    ok &= !sources.Select(source_window, &owner);
    source_window.SelectCopy(7, &owner);
    ok &= sources.Select(source_window, &owner) && sources.records == 2;
    for (unsigned copy = 2; copy < 4; ++copy) {
        ok &= !sources.FinishBeforeCopy(source_window, &owner);
        ok &= source_window.SelectCopy(7, &owner, 0x1000u + copy * 0x1000u);
        ok &= source_window.source_arena == 0x1000u + copy * 0x1000u;
        ok &= sources.Select(source_window, &owner);
    }
    ok &= !sources.FinishBeforeCopy(source_window, nullptr) && !sources.FinishBeforeCopy(source_window, &other);
    ok &= sources.FinishBeforeCopy(source_window, &owner) && sources.closed;
    ok &= !sources.Select(source_window, &owner) && !sources.FinishBeforeCopy(source_window, &owner);
    source_window.SelectCopy(7, &owner);
    RuntimeMotionSourceBudget late_sources;
    ok &= !late_sources.Select(source_window, &owner); // cannot start on a later CPU copy
    RuntimeMotionSourceBudget full_sources;
    source_window = {};
    source_window.manual = true;
    source_window.SelectCopy(9, &owner);
    for (unsigned i = 0; i < RuntimeMotionSourceBudget::kMaximumRecords; ++i)
        ok &= full_sources.Select(source_window, &owner);
    ok &= !full_sources.Select(source_window, &owner) && full_sources.dropped == 1;
    full_sources.dropped = UINT32_MAX;
    ok &= !full_sources.Select(source_window, &owner) && full_sources.dropped == UINT32_MAX;
    source_window.generation = 10;
    ok &= !full_sources.Select(source_window, &owner) && !full_sources.FinishBeforeCopy(source_window, &owner);
    source_window.generation = 9;
    source_window.copies = 4;
    ok &= full_sources.FinishBeforeCopy(source_window, &owner); // dropping never enlarges storage
    // The native render loop can issue all viewport copies without re-entering
    //820E2E50. Completion must occur at copy5 entry, without FrameEnter calls.
    RuntimeMotionSourceBudget looping_sources;
    RuntimeCameraCaptureWindow looping_window;
    looping_window.manual = true;
    for (unsigned copy = 0; copy < 16; ++copy) {
        ok &= looping_sources.FinishBeforeCopy(looping_window, &owner) == (copy == 4);
        ok &= looping_window.SelectCopy(11, &owner);
        ok &= looping_sources.Select(looping_window, &owner) == (copy < 4);
    }
    ok &= looping_sources.closed && looping_sources.records == 4 && looping_sources.dropped == 0;
    RuntimeCameraCaptureWindow arena_window;
    arena_window.manual = true;
    ok &= arena_window.SelectCopy(12, &owner, 0x1000) && arena_window.source_arena == 0x1000;
    ok &= !arena_window.SelectCopy(13, &owner, 0x2000) && !arena_window.source_arena;
    ok &= arena_window.SelectCopy(12, &owner, 0x3000);
    arena_window.FrameEnter();
    ok &= !arena_window.source_arena && !arena_window.owner;
    ok &= RuntimeIsMotionVectorPair(1, 1, 0, 0, 8);
    ok &= !RuntimeIsMotionVectorPair(0, 1, 0, 0, 8) && !RuntimeIsMotionVectorPair(1, 4, 0, 0, 8);
    ok &= !RuntimeIsMotionVectorPair(1, 1, 0x1000, 0, 8) && !RuntimeIsMotionVectorPair(1, 1, 0, 0x1000, 8);
    ok &= !RuntimeIsMotionVectorPair(1, 1, 0, 0, 7);
    ok &= RuntimeMotionVectorPairCount(0) == 0 && RuntimeMotionVectorPairCount(0xFF00) == 0;
    ok &= RuntimeMotionVectorPairCount(0xFF) == 8 && RuntimeMotionVectorPairCount(0xFFFFFFFF) == 8;
    ok &= RuntimeMotionVectorPairCount(0x77) == 6 && RuntimeMotionVectorPairCount(0xA5) == 4;
    ok &= RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0x1000, 0x10A0, 1);
    ok &= RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0xFFFFFDC0, 0xFFFFFE60, 64);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x8227695C, 0x1000, 0x10A0, 1);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B0, 0x82276960, 0x1000, 0x10A0, 1);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0x1000, 0x10A4, 1);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0x1004, 0x10A4, 1);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0x1000, 0x10A0, 0);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0x1000, 0x10A0, 65);
    ok &= !RuntimeIsCompactMotionPublication(0x8259D3B8, 0x82276960, 0xFFFFFDD0, 0xFFFFFE70, 1);
    RuntimeMotionProducerBudget producers;
    RuntimeCameraCaptureWindow producer_window;
    ok &= !producers.Select(producer_window, &owner);
    producer_window.manual = true;
    producer_window.SelectCopy(14, &owner);
    ok &= producers.Select(producer_window, &other); // real scene worker, not main-copy identity
    ok &= !producers.Select(producer_window, nullptr);
    ok &= !producers.FinishBeforeCopy(producer_window, &owner);
    producer_window.SelectCopy(14, &owner);
    ok &= !producers.FinishBeforeCopy(producer_window, &other);
    ok &= producers.Select(producer_window, &owner) && producers.records == 2;
    producer_window.generation = 15;
    ok &= !producers.Select(producer_window, &owner) && !producers.FinishBeforeCopy(producer_window, &owner);
    producer_window.generation = 14;
    ok &= producers.FinishBeforeCopy(producer_window, &owner);
    ok &= !producers.Select(producer_window, &other) && !producers.FinishBeforeCopy(producer_window, &owner);
    RuntimeMotionProducerBudget empty_producers;
    ok &= empty_producers.FinishBeforeCopy(producer_window, &owner);
    ok &= empty_producers.generation == 14 && empty_producers.records == 0;
    RuntimeMotionProducerBudget full_producers;
    for (unsigned i = 0; i < RuntimeMotionProducerBudget::kMaximumRecords; ++i)
        ok &= full_producers.Select(producer_window, &other);
    ok &= !full_producers.Select(producer_window, &owner) && full_producers.dropped == 1;
    full_producers.dropped = UINT32_MAX;
    ok &= !full_producers.Select(producer_window, &other) && full_producers.dropped == UINT32_MAX;
    producer_window.SelectCopy(14, &owner);
    ok &= !full_producers.Select(producer_window, &other);
    ok &= full_producers.FinishBeforeCopy(producer_window, &owner);
    ok &= RuntimeMotionSourceRange(0x1000, 64, 16);
    ok &= RuntimeMotionSourceRange(0xFFFFFFC0, 64, 16);
    ok &= !RuntimeMotionSourceRange(0, 64, 16) && !RuntimeMotionSourceRange(0x1001, 64, 16);
    ok &= !RuntimeMotionSourceRange(0xFFFFFFC0, 65, 16) && !RuntimeMotionSourceRange(0x1000, 0);
    ok &= !RuntimeMotionSourceRange(0x1000, 64, 0) && !RuntimeMotionSourceRange(0x1000, 64, 3);
    ok &= RuntimeIsMotionConstantCopy(0x822490D4, 0x1000, 0x1080, 12, 0x1060, 0x82A6DD00);
    ok &= RuntimeIsMotionConstantCopy(0x822490D4, 0xFFFFFF60, 0xFFFFFFE0, 12, 0xFFFFFFC0, 0x82A6DD00);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D0, 0x1000, 0x1080, 12, 0x1060, 0x82A6DD00);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D4, 0x1000, 0x1084, 12, 0x1060, 0x82A6DD00);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D4, 0x1000, 0x1080, 16, 0x1060, 0x82A6DD00);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D4, 0x1000, 0x1080, 12, 0x1064, 0x82A6DD00);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D4, 0x1000, 0x1080, 12, 0x1060, 0x82A6DD04);
    ok &= !RuntimeIsMotionConstantCopy(0x822490D4, 0xFFFFFF70, 0xFFFFFFF0, 12, 0xFFFFFFD0, 0x82A6DD00);
    // Origin observation must include worker registrations and later main-copy
    // boundaries without treating either CPU context as a persistent frame.
    RuntimeMotionOriginBudget origins;
    RuntimeCameraCaptureWindow origin_window;
    ok &= !origins.Select(origin_window, &owner) && !origins.FinishBeforeCopy(origin_window, &owner);
    origin_window.manual = true;
    ok &= !origins.Select(origin_window, &other);
    for (unsigned copy = 0; copy < 6; ++copy) {
        ok &= origins.FinishBeforeCopy(origin_window, &owner) == (copy == 4);
        origin_window.SelectCopy(16, &owner);
        ok &= origins.Select(origin_window, &other) == (copy < 4);
        ok &= !origins.FinishBeforeCopy(origin_window, &other);
    }
    ok &= origins.closed && origins.records == 4 && !origins.dropped;
    RuntimeMotionOriginBudget bounded_origins;
    origin_window.copies = 4;
    ok &= !bounded_origins.Select(origin_window, nullptr);
    for (unsigned i = 0; i < RuntimeMotionOriginBudget::kMaximumRecords; ++i)
        ok &= bounded_origins.Select(origin_window, &other);
    ok &= !bounded_origins.Select(origin_window, &owner) && bounded_origins.dropped == 1;
    bounded_origins.dropped = UINT32_MAX;
    ok &= !bounded_origins.Select(origin_window, &owner) && bounded_origins.dropped == UINT32_MAX;
    origin_window.generation = 17;
    ok &= !bounded_origins.Select(origin_window, &owner) && !bounded_origins.FinishBeforeCopy(origin_window, &owner);
    origin_window.generation = 16;
    origin_window.FrameEnter();
    ok &= !bounded_origins.Select(origin_window, &other) && !bounded_origins.FinishBeforeCopy(origin_window, &owner);
    origin_window.owner = &owner;
    RuntimeMotionOriginBudget empty_origins;
    ok &= empty_origins.FinishBeforeCopy(origin_window, &owner) && empty_origins.generation == 16 && !empty_origins.records;
    ok &= bounded_origins.FinishBeforeCopy(origin_window, &owner);
    using OriginSite = RuntimeMotionOriginSite;
    ok &= RuntimeMotionOriginFunction(0x825DF350, 0x825DFC98) == OriginSite::CameraReady;
    ok &= RuntimeMotionOriginFunction(0x825DF350, 0x825DFC94) == OriginSite::None;
    ok &= RuntimeMotionOriginFunction(0x825DFD50, 0x825DFD04) == OriginSite::SpecialCopy;
    ok &= RuntimeMotionOriginFunction(0x825DFD50, 0x825ED384) == OriginSite::None; // array resize also calls copier
    ok &= RuntimeMotionOriginFunction(0x8259D3B8, 0x82276960) == OriginSite::CompactOutput;
    ok &= RuntimeMotionOriginFunction(0x8259D3B8, 0x8227695C) == OriginSite::None;
    ok &= RuntimeMotionOriginFunction(0x825E2958, 0) == OriginSite::RenderDispatch;
    ok &= RuntimeMotionOriginFunction(0x825E295C, 0) == OriginSite::None;
    ok &= RuntimeMotionOriginFunction(0x825E54B0, 0) == OriginSite::CameraParent;
    ok &= RuntimeMotionOriginFunction(0x821D3700, 0x825E47A8) == OriginSite::RegisterSingleReady;
    ok &= RuntimeMotionOriginFunction(0x821D3700, 0x825E4990) == OriginSite::RegisterPairReady;
    ok &= RuntimeMotionOriginFunction(0x821D3700, 0x825E4614) == OriginSite::None;
    ok &= RuntimeMotionOriginFunction(0x820C88B0, 0x825E47A8) == OriginSite::None; // tail target must not double count
    ok &= RuntimeCompletedMotionRegistration(0x1000, 1, 2, 0x1000, 0x2000, 0x2000);
    ok &= RuntimeCompletedMotionRegistration(0x1000, 2, 2, 0x10F0, 0x2000, 0x2000);
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 2, 2, 0x1000, 0x2000, 0x2000); // stale pre-lock slot
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 0, 2, 0x1000, 0x2000, 0x2000);
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 3, 2, 0x11E0, 0x2000, 0x2000);
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 1, 2, 0x1000, 0x2000, 0);
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 1, 2, 0x1000, 0, 0);
    ok &= !RuntimeCompletedMotionRegistration(0xFFFFFF20, 1, 2, 0xFFFFFF20, 0x2000, 0x2000);
    ok &= !RuntimeCompletedMotionRegistration(0x1000, 1, 2, 0, 0x2000, 0x2000);
    ok &= RuntimeMotionRegistrationSlot(0x1000, 0, 1) == 0x1000;
    ok &= RuntimeMotionRegistrationSlot(0x1000, 1, 2) == 0x10F0;
    ok &= !RuntimeMotionRegistrationSlot(0x1000, 1, 1) && !RuntimeMotionRegistrationSlot(0x1000, 0, 0);
    ok &= !RuntimeMotionRegistrationSlot(0, 0, 1) && !RuntimeMotionRegistrationSlot(0x1004, 0, 1);
    ok &= RuntimeMotionRegistrationSlot(0xFFFFFF10, 0, 1) == 0xFFFFFF10;
    ok &= !RuntimeMotionRegistrationSlot(0xFFFFFF20, 0, 1);
    ok &= !RuntimeMotionRegistrationSlot(0x1000, 0x10000000, 0x10000001);
    uint32_t sequence{};
    ok &= !ticket.Consume(&owner, sequence);
    ticket.Arm(&owner, 3);
    ok &= !ticket.Consume(&owner, sequence); // before publisher
    ticket.Arm(&owner, 4);
    ticket.Publication(&owner, 0x82762844);
    ok &= ticket.Consume(&owner, sequence) && sequence == 4;
    ok &= !ticket.Consume(&owner, sequence); // exactly once
    ticket.Arm(&owner, 5);
    ticket.Publication(&other, 0x82762844);
    ok &= !ticket.Consume(&owner, sequence);
    ticket.Arm(&owner, 6);
    ticket.Publication(&owner, 0x82762844);
    ticket.Publication(&owner, 0x82762844); // replaced viewport
    ok &= !ticket.Consume(&owner, sequence);
    ticket.Arm(&owner, 7);
    ticket.Publication(&owner, 0x1234);
    ok &= !ticket.Consume(&owner, sequence);
    ticket.Arm(&owner, 8);
    ticket.Clear();
    ok &= !ticket.Consume(&owner, sequence);
    ok &= RuntimeObservedTransformAddress(0x1000, 0) == 0x1010;
    ok &= RuntimeObservedTransformAddress(0x1000, 1) == 0x12A0;
    ok &= RuntimeObservedTransformAddress(0xFFFFFFB0, 0) == 0xFFFFFFC0;
    ok &= RuntimeObservedTransformAddress(0xFFFFFFC0, 0) == 0;
    ok &= RuntimeObservedTransformAddress(0x1000, 0xFFFFFFFF) == 0;
    ok &= RuntimeObservedTransformAddress(0, 0) == 0;
    ok &= RuntimeObservedTransformAddress(0x1001, 0) == 0;
    ok &= RuntimeIsFrameViewportCopy(0x82762790, 0x820E3630, 0x1000, 0x18B0);
    ok &= !RuntimeIsFrameViewportCopy(0x82762790, 0x820E3634, 0x1000, 0x18B0);
    ok &= !RuntimeIsFrameViewportCopy(0x82762794, 0x820E3630, 0x1000, 0x18B0);
    ok &= !RuntimeIsFrameViewportCopy(0x82762790, 0x820E3630, 0x1000, 0x18B4);
    ok &= !RuntimeIsFrameViewportCopy(0x82762790, 0x820E3630, 0, 2224);
    ok &= !RuntimeIsFrameViewportCopy(0x82762790, 0x820E3630, 0xFFFFF800, 0xB0);
    // V376 field of view: 86 at 16:9 is the title's 70 at 4:3 and changes
    // nothing; other values scale every view FOV by one tangent ratio.
    ok &= RuntimeGameplayFovIsOriginal(86.0f);
    ok &= !RuntimeGameplayFovIsOriginal(95.0f);
    ok &= RuntimeGameplayFovTangentScale(86.0f) == 1.0;
    ok &= RuntimeScaledViewFov(70.0f, 1.0) == 70.0f;
    {
        const double wider = RuntimeGameplayFovTangentScale(105.0f);
        // 105 at 16:9 = 2 atan(tan(52.5) * 0.75) = 88.69 at 4:3.
        ok &= wider > 1.0 && std::fabs(RuntimeScaledViewFov(70.0f, wider) - 88.69f) < 0.05f;
        // A 35-degree zoom keeps its magnification against the base view.
        const float zoom = RuntimeScaledViewFov(35.0f, wider);
        const double magnification_before = std::tan(35.0 * 3.14159265358979 / 360.0) /
                                            std::tan(70.0 * 3.14159265358979 / 360.0);
        const double magnification_after =
            std::tan(zoom * 3.14159265358979 / 360.0) /
            std::tan(RuntimeScaledViewFov(70.0f, wider) * 3.14159265358979 / 360.0);
        ok &= std::fabs(magnification_before - magnification_after) < 1e-4;
        const double narrower = RuntimeGameplayFovTangentScale(60.0f);
        ok &= narrower < 1.0 && RuntimeScaledViewFov(70.0f, narrower) < 70.0f;
        ok &= RuntimeScaledViewFov(0.0f, wider) == 0.0f;
        ok &= RuntimeScaledViewFov(179.0f, wider) <= 170.0f;
    }
    ok &= RuntimeIsClientViewCopy(0x8249A12Cu, 416u);
    ok &= !RuntimeIsClientViewCopy(0x8249A1ECu, 416u);
    ok &= !RuntimeIsClientViewCopy(0x8249A12Cu, 352u);
    ok &= kClientViewWriteBackReturn == 0x8249A1ECu;
    {
        // V391: frames of the view builder (sub_8249A168) with FOV 105. The
        // client's copy and the persistent view (which sub_82347380 copies to
        // client +0x640 for the renderer) both get the scaled value, and it
        // never compounds when the view owner stops rewriting the FOV.
        // (V382-V390 restored the title's value in the persistent view, so
        // the renderer drew the original view whatever the setting.)
        const double wider = RuntimeGameplayFovTangentScale(105.0f);
        auto bits = [](float value) {
            uint32_t out;
            std::memcpy(&out, &value, sizeof(out));
            return out;
        };
        auto value = [](uint32_t in) {
            float out;
            std::memcpy(&out, &in, sizeof(out));
            return out;
        };
        RuntimeViewFovScaling last;
        uint32_t persistent = bits(70.0f);
        float client = 0.0f;
        float render = 0.0f;
        auto frame = [&](bool owner_rewrites, float owner_fov) {
            uint32_t view = persistent;                        // memcpy(local, persistent)
            if (owner_rewrites) view = bits(owner_fov);        // message 36
            const uint32_t title = RuntimeViewFovTitleBits(last, view);
            last = {title, bits(RuntimeScaledViewFov(value(title), wider))};
            view = last.scaled_bits;                           // the scaler
            client = value(view);                              // memcpy(client + 2112, local)
            persistent = view;                                 // memcpy(persistent, local)
            render = value(persistent);                        // sub_82347380 -> client +0x744
        };
        frame(true, 70.0f);
        ok &= std::fabs(client - 88.69f) < 0.05f && render == client;
        for (int i = 0; i < 600; ++i) frame(false, 0.0f);      // no FOV writer
        ok &= std::fabs(render - 88.69f) < 0.05f && render == client;
        frame(true, 90.0f);                                    // Darkness mode
        ok &= std::fabs(render - 108.77f) < 0.05f;
        frame(true, 35.0f);                                    // zoom
        ok &= render == RuntimeScaledViewFov(35.0f, wider);
        for (int i = 0; i < 600; ++i) frame(false, 0.0f);
        ok &= render == RuntimeScaledViewFov(35.0f, wider);
        frame(true, 70.0f);
        ok &= std::fabs(render - 88.69f) < 0.05f;
        // A title value that happens to differ from the last output is scaled.
        ok &= RuntimeViewFovTitleBits(last, bits(70.0f)) == bits(70.0f) &&
              RuntimeViewFovTitleBits(last, last.scaled_bits) == bits(70.0f) &&
              RuntimeViewFovTitleBits({}, bits(0.0f)) == bits(0.0f);
    }
    ok &= RuntimeTemporalCameraFunction(0x8249ADF0, 0) == RuntimeTemporalCameraSite::Begin;
    ok &= RuntimeTemporalCameraFunction(0x82114838, 0x8249B0D4) == RuntimeTemporalCameraSite::Inverse;
    ok &= RuntimeTemporalCameraFunction(0x82114838, 0x82499664) == RuntimeTemporalCameraSite::None;
    ok &= RuntimeTemporalCameraFunction(0x823EBAB8, 0x8249B1E8) == RuntimeTemporalCameraSite::Clear;
    ok &= RuntimeTemporalCameraFunction(0x823EBAB8, 0x8249ADD0) == RuntimeTemporalCameraSite::Clear;
    ok &= RuntimeTemporalCameraFunction(0x823EBAB8, 0x8249ADD8) == RuntimeTemporalCameraSite::Clear;
    ok &= RuntimeTemporalCameraFunction(0x823EBAB8, 0) == RuntimeTemporalCameraSite::None;
    ok &= RuntimeTemporalCameraFunction(0x829B917C, 0x8249B228) == RuntimeTemporalCameraSite::End;
    ok &= RuntimeTemporalCameraFunction(0x829B917C, 0) == RuntimeTemporalCameraSite::None;
    ok &= RuntimeTemporalCameraFunction(0x825EB6B8, 0x8249A388) == RuntimeTemporalCameraSite::Forward;
    ok &= RuntimeTemporalCameraFunction(0x825EB6B8, 0) == RuntimeTemporalCameraSite::None;
    ok &= RuntimeTemporalCameraFunction(0x8249AD40, 0) == RuntimeTemporalCameraSite::Reset;
    ok &= RuntimeTemporalCameraFunction(0x82498F68, 0x823F8570) == RuntimeTemporalCameraSite::Callback;
    ok &= RuntimeTemporalCameraFunction(0x82498F68, 0x825E572C) == RuntimeTemporalCameraSite::None;
    ok &= RuntimeTemporalCameraTable(0x10000, 0) == 0x10A70;
    ok &= RuntimeTemporalCameraTable(0x10000, 1) == 0x10A9C;
    ok &= !RuntimeTemporalCameraTable(0x10000, 2) && !RuntimeTemporalCameraTable(0, 0);
    ok &= !RuntimeTemporalCameraTable(0x10004, 0) && !RuntimeTemporalCameraTable(0xFFFFF000, 1);
    ok &= RuntimeTemporalCameraBucket(-1, 0, 0) && RuntimeTemporalCameraBucket(2, 3, 4);
    ok &= !RuntimeTemporalCameraBucket(-2, 3, 4) && !RuntimeTemporalCameraBucket(3, 3, 4);
    ok &= !RuntimeTemporalCameraBucket(0, 0, 4) && !RuntimeTemporalCameraBucket(-1, 5, 4);
    ok &= !RuntimeTemporalCameraBucket(-1, 0, 32768);
    ok &= RuntimeTemporalCameraFrame(0x1100, false) == 0x1100;
    ok &= RuntimeTemporalCameraFrame(0x1100, true) == 0xF60;
    ok &= !RuntimeTemporalCameraFrame(400, true) && !RuntimeTemporalCameraFrame(0x1104, false);
    ok &= !RuntimeTemporalCameraFrame(0xFFFFFF00, false);
    {
        // 0.9.1 (#6): the rendered FOV follows the title's 30 Hz steps over
        // one tick, every frame, continuously; 30 FPS and cuts are unchanged.
        RuntimeFovSmoother s;
        ok &= s.Next(70.0f, 1.0) == 70.0f && s.Next(70.0f, 1.007) == 70.0f;
        const double tick = 1.0 / 30.0;
        ok &= s.Next(66.0f, 1.014) == 70.0f;
        ok &= std::fabs(s.Next(66.0f, 1.014 + tick / 2) - 68.0f) < 0.01f;
        ok &= std::fabs(s.Next(66.0f, 1.014 + tick) - 66.0f) < 1e-4f;
        // A change in the middle of a step continues from the shown value.
        ok &= std::fabs(s.Next(62.0f, 1.014 + tick + 0.007) - 66.0f) < 1e-4f;
        const float mid = s.Next(62.0f, 1.014 + tick + 0.007 + tick / 2);
        ok &= std::fabs(mid - 64.0f) < 0.01f;
        ok &= std::fabs(s.Next(58.0f, 1.014 + tick + 0.007 + tick / 2) - mid) < 1e-4f;
        RuntimeFovSmoother thirty;
        thirty.Next(70.0f, 2.0);
        ok &= thirty.Next(60.0f, 2.0 + tick) == 60.0f;
        RuntimeFovSmoother cut;
        cut.Next(70.0f, 3.0);
        cut.Next(70.0f, 3.007);
        ok &= cut.Next(40.0f, 3.014) == 40.0f;
    }
    if (!ok) {
        std::cerr << "runtime camera policy regression\n";
        return 1;
    }
    std::cout << "runtime camera policy: PASS\n";
    return 0;
}
