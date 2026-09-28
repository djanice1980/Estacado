#include "runtime_motion_origins.h"
#include "ppc_recomp_shared.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
RuntimeMotionOriginBudget budget;
constexpr const char* path = "logs/pc_motion_origins.log";

const char* SiteName(RuntimeMotionOriginSite site) {
    switch (site) {
    case RuntimeMotionOriginSite::CameraBegin: return "camera_begin";
    case RuntimeMotionOriginSite::CameraReady: return "camera_ready";
    case RuntimeMotionOriginSite::RegisterSingle: return "register_single";
    case RuntimeMotionOriginSite::RegisterPair: return "register_pair";
    case RuntimeMotionOriginSite::SpecialCopy: return "special_copy_input";
    case RuntimeMotionOriginSite::RenderDispatch: return "render_dispatch";
    case RuntimeMotionOriginSite::ModelBuilder: return "model_builder";
    case RuntimeMotionOriginSite::MeshBuilder: return "mesh_builder";
    case RuntimeMotionOriginSite::CompactOutput: return "compact_output";
    case RuntimeMotionOriginSite::CameraParent: return "camera_parent";
    case RuntimeMotionOriginSite::RegisterSingleReady: return "register_single_ready";
    case RuntimeMotionOriginSite::RegisterPairReady: return "register_pair_ready";
    default: return "invalid";
    }
}

void Header(std::ostream& out) {
    out << "MOTION_ORIGIN_CAPTURE version=2 generation=" << budget.generation
        << " maximum_records=" << RuntimeMotionOriginBudget::kMaximumRecords
        << " maximum_copies=" << RuntimeMotionOriginBudget::kMaximumCopies
        << " scope=native_input_lifecycle_observations_not_temporal_history\n";
}
}

void RuntimeMotionOriginsReset() { budget = {}; }
uint32_t RuntimeMotionOriginsObservedCount() noexcept { return budget.records; }

void RuntimeMotionOriginsObserve(PPCContext& context, uint8_t* base, uint32_t address,
                                 const RuntimeCameraCaptureWindow& window, uint32_t producer_sequence) {
    const auto site = RuntimeMotionOriginFunction(address, uint32_t(context.lr));
    if (site == RuntimeMotionOriginSite::None ||
        (site == RuntimeMotionOriginSite::CompactOutput && producer_sequence == UINT32_MAX) ||
        !budget.Select(window, &context)) return;
    const uint32_t sequence = budget.records - 1u;
    uint32_t owner = 0, camera = 0, owner_vtable = 0, camera_vtable = 0;
    uint32_t viewport_index = 0, camera_flags = 0, owner_flags = 0;
    uint32_t array = 0, count = 0, capacity = 0, item = 0, object = 0, object_vtable = 0, target = 0;
    uint32_t input_current = 0, input_delta = 0, input_inverse = 0, mode = 0, item_flags = 0, source_object = 0;
    bool valid = true, active = true;
    const bool camera_site = site == RuntimeMotionOriginSite::CameraBegin || site == RuntimeMotionOriginSite::CameraReady;
    const bool registration = site == RuntimeMotionOriginSite::RegisterSingle || site == RuntimeMotionOriginSite::RegisterPair;
    const bool registration_ready = site == RuntimeMotionOriginSite::RegisterSingleReady || site == RuntimeMotionOriginSite::RegisterPairReady;
    const bool camera_parent = site == RuntimeMotionOriginSite::CameraParent;
    const bool builder = site == RuntimeMotionOriginSite::ModelBuilder || site == RuntimeMotionOriginSite::MeshBuilder;

    if (camera_site || site == RuntimeMotionOriginSite::SpecialCopy) {
        camera = camera_site ? context.r3.u32 : context.r30.u32;
        valid = RuntimeMotionSourceRange(camera, 2184u, 16u);
        if (site == RuntimeMotionOriginSite::CameraReady) valid = valid && camera == context.r31.u32;
        if (site == RuntimeMotionOriginSite::SpecialCopy)
            valid = valid && context.r3.u32 == camera + 1520u && RuntimeMotionSourceRange(context.r4.u32, 228u, 16u);
        if (valid) owner = PPC_LOAD_U32(camera + 12u);
    } else if (registration_ready) {
        // 820CB7A0 installed 821D3700 as the lock-release callback. Its r3 is
        // owner+1812; the callback has not released the native lock yet.
        valid = context.r3.u32 >= 1812u;
        if (valid) owner = context.r3.u32 - 1812u;
    } else if (registration || camera_parent || site == RuntimeMotionOriginSite::RenderDispatch) {
        owner = context.r3.u32;
    } else if (builder) {
        owner = context.r4.u32;
        object = context.r3.u32;
    } else {
        valid = RuntimeIsCompactMotionPublication(address, uint32_t(context.lr), context.r1.u32,
                    context.r4.u32, context.r5.u32) && RuntimeMotionSourceRange(context.r16.u32, 24u);
        if (valid) owner = PPC_LOAD_U32(context.r16.u32 + 16u);
    }
    valid = valid && RuntimeMotionSourceRange(owner, 1264u);
    if (valid) {
        owner_vtable = PPC_LOAD_U32(owner);
        owner_flags = PPC_LOAD_U32(owner + 1192u);
        viewport_index = PPC_LOAD_U32(owner + 220u);
        if (registration || registration_ready || camera_parent || site == RuntimeMotionOriginSite::RenderDispatch) {
            const uint32_t viewports = PPC_LOAD_U32(owner + 1260u);
            valid = RuntimeMotionSourceRange(viewports, 28u);
            if (valid) {
                const uint32_t pointers = PPC_LOAD_U32(viewports + 24u);
                const uint64_t slot = uint64_t(pointers) + uint64_t(viewport_index) * 4u;
                valid = pointers && slot <= UINT32_MAX - 3u && RuntimeMotionSourceRange(uint32_t(slot), 4u);
                if (valid) camera = PPC_LOAD_U32(uint32_t(slot));
            }
        } else if (!camera) camera = PPC_LOAD_U32(owner + 224u);
        valid = valid && RuntimeMotionSourceRange(camera, 2184u, 16u);
    }
    if (valid) {
        camera_vtable = PPC_LOAD_U32(camera);
        array = PPC_LOAD_U32(camera + 1508u);
        capacity = PPC_LOAD_U32(camera + 2172u);
        count = PPC_LOAD_U32(camera + 2176u);
        camera_flags = PPC_LOAD_U32(camera + 2180u);
        if (camera_parent) {
            active = !(owner_flags & 0x8000u);
            if (active) { input_current = context.r5.u32; input_delta = context.r6.u32; }
        } else if (site == RuntimeMotionOriginSite::CameraBegin) {
            active = !(owner_flags & 0x8000u);
            if (active) { input_current = context.r4.u32; input_delta = context.r5.u32; }
        } else if (site == RuntimeMotionOriginSite::CameraReady) {
            valid = RuntimeMotionSourceRange(camera_vtable, 48u) && PPC_LOAD_U32(camera_vtable + 44u) == address;
        } else if (registration) {
            object = context.r4.u32;
            mode = site == RuntimeMotionOriginSite::RegisterPair ? context.r8.u32 : context.r7.u32;
            active = object && mode == 0u && count < capacity;
            if (active) {
                item = RuntimeMotionRegistrationSlot(array, count, capacity);
                valid = item != 0;
                input_current = context.r5.u32;
                // Single registration writes native identity; no invented input pointer.
                if (site == RuntimeMotionOriginSite::RegisterPair) input_delta = context.r6.u32;
            }
        } else if (registration_ready) {
            const uint32_t saved_camera = site == RuntimeMotionOriginSite::RegisterSingleReady
                ? context.r29.u32 : context.r28.u32;
            active = RuntimeCompletedMotionRegistration(array, count, capacity, context.r31.u32, camera, saved_camera);
            // Inactive modes and capacity exits have no completed render-list item.
            if (active) {
                item = context.r31.u32;
                input_current = item;
                input_delta = item + 64u;
                source_object = context.r30.u32;
                valid = RuntimeMotionSourceRange(source_object, 4u);
                object = PPC_LOAD_U32(item + 216u);
                item_flags = PPC_LOAD_U32(item + 224u);
            }
        } else if (site == RuntimeMotionOriginSite::SpecialCopy) {
            item = context.r3.u32;
            input_current = context.r4.u32;
            input_delta = input_current + 64u;
            object = PPC_LOAD_U32(context.r4.u32 + 216u);
            item_flags = PPC_LOAD_U32(context.r4.u32 + 224u);
        } else if (site == RuntimeMotionOriginSite::RenderDispatch) {
            item = context.r4.u32;
            valid = RuntimeMotionSourceRange(item, 228u, 16u);
            if (valid) {
                object = PPC_LOAD_U32(item + 216u);
                active = object != 0;
                item_flags = PPC_LOAD_U32(item + 224u);
                input_current = item;
                input_delta = item + 64u;
                input_inverse = camera + 1888u;
            }
        } else if (builder) {
            valid = RuntimeMotionSourceRange(context.r1.u32, 104u, 16u);
            if (valid) {
                input_current = context.r10.u32;
                input_inverse = PPC_LOAD_U32(context.r1.u32 + 84u);
                input_delta = PPC_LOAD_U32(context.r1.u32 + 92u);
                item_flags = PPC_LOAD_U32(context.r1.u32 + 100u);
            }
        } else {
            const uint32_t descriptor = context.r19.u32;
            valid = RuntimeMotionSourceRange(descriptor, 24u);
            if (valid) {
                const uint32_t inverse = PPC_LOAD_U32(descriptor + 16u);
                const uint32_t delta = PPC_LOAD_U32(descriptor + 20u);
                // The compact builder reads these three matrices only in its
                // composed branch. Match the existing endpoint observer's scope.
                if (inverse && delta) {
                    input_current = PPC_LOAD_U32(descriptor + 12u);
                    input_inverse = inverse;
                    input_delta = delta;
                }
            }
        }
        if (valid && object) {
            valid = RuntimeMotionSourceRange(object, 4u);
            if (valid) object_vtable = PPC_LOAD_U32(object);
            if (valid && site == RuntimeMotionOriginSite::RenderDispatch) {
                valid = RuntimeMotionSourceRange(object_vtable, 108u);
                if (valid) target = PPC_LOAD_U32(object_vtable + 104u);
            }
        }
    }
    struct Matrix { const char* role; uint32_t source; bool valid{}; uint32_t words[16]{}; } matrices[] = {
        {"camera_1760", valid ? camera + 1760u : 0u}, {"camera_1824", valid ? camera + 1824u : 0u},
        {"camera_1888", valid ? camera + 1888u : 0u}, {"camera_1952", valid ? camera + 1952u : 0u},
        {"input_current", input_current}, {"input_delta", input_delta}, {"input_inverse", input_inverse}
    };
    bool all_valid = valid;
    for (auto& matrix : matrices) {
        matrix.valid = valid && (!matrix.source || RuntimeMotionSourceRange(matrix.source, 64u, 16u));
        all_valid = all_valid && matrix.valid;
        if (matrix.source && matrix.valid) for (uint32_t i = 0; i < 16u; ++i)
            matrix.words[i] = PPC_LOAD_U32(matrix.source + i * 4u);
    }
    if (!all_valid) ++budget.invalid;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out(path, sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) Header(out);
    out << "MOTION_ORIGIN sequence=" << sequence << " generation=" << budget.generation
        << " window_copy_sequence=" << window.copies - 1u << " main_copy_context=" << (window.owner == &context ? 1 : 0)
        << " site=" << SiteName(site) << " producer_sequence=" << producer_sequence
        << " active=" << (active ? 1 : 0) << " valid=" << (all_valid ? 1 : 0)
        << " function=" << std::hex << address << " caller=" << uint32_t(context.lr) << " stack=" << context.r1.u32
        << " render_owner=" << owner << " owner_vtable=" << owner_vtable << " viewport_index=" << viewport_index
        << " owner_flags=" << owner_flags << " camera=" << camera << " camera_vtable=" << camera_vtable
        << " camera_flags=" << camera_flags << " array=" << array << " count=" << count << " capacity=" << capacity
        << " item=" << item << " object=" << object << " object_vtable=" << object_vtable << " target=" << target
        << " mode=" << mode << " item_flags=" << item_flags << " source_object=" << source_object << std::dec << '\n';
    for (const auto& matrix : matrices) {
        out << "MOTION_ORIGIN_MATRIX sequence=" << sequence << " role=" << matrix.role << " source=" << std::hex << matrix.source
            << " present=" << (matrix.source ? 1 : 0) << " valid=" << (matrix.valid ? 1 : 0) << " words=";
        if (matrix.source && matrix.valid) for (uint32_t i = 0; i < 16u; ++i)
            out << (i ? "," : "") << std::setw(8) << std::setfill('0') << matrix.words[i];
        out << std::dec << '\n';
    }
    out.close();
    if (error || out.fail()) ++budget.write_failures;
}

void RuntimeMotionOriginsFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window) {
    if (!budget.FinishBeforeCopy(window, &context)) return;
    std::ofstream out(path, budget.records ? std::ios::app : std::ios::trunc);
    if (!budget.records) Header(out);
    out << "MOTION_ORIGIN_END generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " copies=" << window.copies << " boundary=next_qualified_viewport_copy\n";
    out.close();
    if (out.fail()) ++budget.write_failures;
    std::cerr << "PC_MOTION_ORIGIN_CAPTURE generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " closed=1 boundary=next_qualified_viewport_copy path=" << path << '\n';
}
