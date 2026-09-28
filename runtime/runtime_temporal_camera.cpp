#include "runtime_temporal_camera.h"
#include "runtime_motion_origins.h"
#include "runtime_motion_delta.h"
#include "ppc_recomp_shared.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
RuntimeMotionOriginBudget budget;
std::array<const PPCContext*, 16> contexts{};
constexpr const char* path = "logs/pc_camera_history_source.log";

const char* SiteName(RuntimeTemporalCameraSite site) {
    switch (site) {
    case RuntimeTemporalCameraSite::Begin: return "begin";
    case RuntimeTemporalCameraSite::Inverse: return "inverse";
    case RuntimeTemporalCameraSite::Clear: return "clear";
    case RuntimeTemporalCameraSite::End: return "end";
    case RuntimeTemporalCameraSite::Forward: return "forward";
    case RuntimeTemporalCameraSite::Reset: return "reset";
    case RuntimeTemporalCameraSite::Callback: return "callback";
    default: return "invalid";
    }
}

void Header(std::ostream& out) {
    out << "CAMERA_HISTORY_SOURCE_CAPTURE version=1 generation=" << budget.generation
        << " maximum_records=" << RuntimeMotionOriginBudget::kMaximumRecords
        << " maximum_copies=" << RuntimeMotionOriginBudget::kMaximumCopies
        << " scope=native_camera_rotation_not_gpu_history_acceptance\n";
}
}

void RuntimeTemporalCameraReset() { budget = {}; contexts = {}; }

void RuntimeTemporalCameraObserve(PPCContext& context, uint8_t* base, uint32_t address,
                                  const RuntimeCameraCaptureWindow& window) {
    const uint32_t caller = uint32_t(context.lr), stack = context.r1.u32;
    const auto site = RuntimeTemporalCameraFunction(address, caller);
    if (site == RuntimeTemporalCameraSite::None || !budget.Select(window, &context)) return;
    uint32_t context_id = 0;
    for (uint32_t i = 0; i < contexts.size(); ++i) {
        if (!contexts[i]) contexts[i] = &context;
        if (contexts[i] == &context) { context_id = i + 1u; break; }
    }
    const bool begin = site == RuntimeTemporalCameraSite::Begin;
    const bool inverse = site == RuntimeTemporalCameraSite::Inverse;
    const bool end = site == RuntimeTemporalCameraSite::End;
    const bool forward = site == RuntimeTemporalCameraSite::Forward;
    const bool callback = site == RuntimeTemporalCameraSite::Callback;
    const bool reset = site == RuntimeTemporalCameraSite::Reset;
    const bool clear = site == RuntimeTemporalCameraSite::Clear;
    const bool rotation_clear = clear && caller == 0x8249B1E8u;
    const bool body = inverse || end || rotation_clear;
    uint32_t scene = begin || reset ? context.r3.u32 : callback ?
        (context.r3.u32 >= 448u ? context.r3.u32 - 448u : 0u) : context.r31.u32;
    uint32_t owner = forward ? context.r3.u32 : callback ? context.r4.u32 : 0u;
    uint32_t camera = 0, frame = 0, current_input = 0, output_pointer = 0;
    uint32_t inverse_input = 0, inverse_work = 0, output_delta = 0;
    uint32_t slot = 0, enabled = 0, factor = 0, flags = 0, duration = 0;
    uint64_t saved_source_time = 0, saved_scene_time = 0, scene_time = 0, sample_time = 0;
    const uint32_t phase = callback ? context.r6.u32 : 0u;
    bool valid = context_id && RuntimeMotionSourceRange(stack, 4u, 16u) &&
        RuntimeMotionSourceRange(scene, 4264u, 16u);
    if (valid && owner) {
        valid = RuntimeMotionSourceRange(owner, 1264u);
        if (valid) camera = PPC_LOAD_U32(owner + 224u);
    }
    if (valid) {
        flags = PPC_LOAD_U32(scene + 516u);
        duration = PPC_LOAD_U32(scene + 412u);
        slot = PPC_LOAD_U32(scene + 2760u);
        enabled = PPC_LOAD_U32(scene + 2764u);
        factor = PPC_LOAD_U32(scene + 2768u);
        saved_source_time = PPC_LOAD_U64(scene + 2656u);
        saved_scene_time = PPC_LOAD_U64(scene + 2664u);
        scene_time = PPC_LOAD_U64(scene + 3096u);
        valid = slot <= 1u && phase <= 1u;
    }
    if (valid && begin) { current_input = context.r4.u32; output_pointer = context.r5.u32; }
    if (valid && body) {
        frame = RuntimeTemporalCameraFrame(stack, end);
        valid = frame != 0u;
        current_input = context.r29.u32;
        output_pointer = context.r22.u32;
        if (valid) sample_time = PPC_LOAD_U64(frame + 96u);
        if (inverse) {
            inverse_input = context.r3.u32;
            valid = valid && inverse_input == frame + 112u && context.r4.u32 == frame + 176u;
        } else if (rotation_clear) {
            inverse_work = frame + 176u;
            valid = valid && context.r3.u32 == RuntimeTemporalCameraTable(scene, slot);
        } else {
            // Failed time/flag conditions skip the inverse. Read the returned
            // output only; never read an uninitialized stack inverse here.
            output_delta = output_pointer;
        }
    }
    if (valid && forward) {
        current_input = context.r7.u32;
        output_pointer = context.r8.u32;
        output_delta = output_pointer;
        valid = context.r4.u32 == scene + 448u;
    }
    if (valid && clear && !rotation_clear) {
        const uint32_t selected = caller == 0x8249ADD0u ? 0u : 1u;
        valid = context.r3.u32 == RuntimeTemporalCameraTable(scene, selected);
    }
    if (valid && callback) {
        valid = RuntimeMotionSourceRange(camera, 2184u, 16u);
        if (valid) { current_input = camera + 1760u; output_delta = camera + 1824u; }
    }
    if (valid && (begin || body || forward)) valid =
        RuntimeMotionSourceRange(current_input, 64u, 16u) && RuntimeMotionSourceRange(output_pointer, 64u, 16u);

    struct Table { uint32_t address{}, buckets{}, array{}, count{}, capacity{}, nonempty{}; bool valid{}; } tables[2];
    bool all_valid = valid;
    for (uint32_t i = 0; i < 2u; ++i) {
        auto& table = tables[i];
        table.address = RuntimeTemporalCameraTable(scene, i);
        table.valid = valid && table.address;
        if (table.valid) {
            table.buckets = PPC_LOAD_U32(table.address + 20u);
            table.array = PPC_LOAD_U32(table.address + 36u);
            table.count = PPC_LOAD_U32(table.address + 40u);
            table.capacity = PPC_LOAD_U32(table.address + 8u);
            table.valid = RuntimeMotionSourceRange(table.buckets, 512u, 2u) &&
                RuntimeTemporalCameraBucket(-1, table.count, table.capacity) &&
                (!table.count || RuntimeMotionSourceRange(table.array, table.count * 96u, 16u));
            if (table.valid) for (uint32_t j = 0; j < 256u; ++j) {
                const int32_t head = int16_t(PPC_LOAD_U16(table.buckets + j * 2u));
                if (head != -1) ++table.nonempty;
                table.valid = table.valid && RuntimeTemporalCameraBucket(head, table.count, table.capacity);
            }
        }
        all_valid = all_valid && table.valid;
    }
    struct Matrix { const char* role; uint32_t source; bool valid{}; uint32_t words[16]{}; } matrices[] = {
        {"current_input", current_input}, {"cached_current", scene ? scene + 2592u : 0u},
        {"motion_delta", scene ? scene + 2528u : 0u}, {"inverse_input", inverse_input},
        {"inverse_work", inverse_work}, {"output_delta", output_delta}
    };
    for (auto& matrix : matrices) {
        matrix.valid = valid && (!matrix.source || RuntimeMotionSourceRange(matrix.source, 64u, 16u));
        all_valid = all_valid && matrix.valid;
        if (matrix.source && matrix.valid) for (uint32_t j = 0; j < 16u; ++j)
            matrix.words[j] = PPC_LOAD_U32(matrix.source + j * 4u);
    }
    if (!all_valid) ++budget.invalid;
    const uint32_t sequence = budget.records - 1u;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out(path, sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) Header(out);
    out << "CAMERA_HISTORY_SOURCE sequence=" << sequence << " generation=" << budget.generation
        << " window_copy_sequence=" << window.copies - 1u << " context_id=" << context_id
        << " origin_records_seen=" << RuntimeMotionOriginsObservedCount()
        << " delta_records_seen=" << RuntimeMotionDeltaObservedCount()
        << " site=" << SiteName(site) << " valid=" << (all_valid ? 1 : 0)
        << " sample_time_present=" << (body ? 1 : 0)
        << " function=" << std::hex << address << " caller=" << caller << " stack=" << stack
        << " frame=" << frame << " scene=" << scene << " owner=" << owner << " camera=" << camera
        << " phase=" << phase << " output_pointer=" << output_pointer << " slot=" << slot << " enabled=" << enabled
        << " flags=" << flags << " duration=" << duration << " factor=" << factor
        << " saved_source_time=" << saved_source_time << " saved_scene_time=" << saved_scene_time
        << " scene_time=" << scene_time << " sample_time=" << sample_time << std::dec << '\n';
    for (uint32_t i = 0; i < 2u; ++i) {
        const auto& table = tables[i];
        out << "CAMERA_HISTORY_TABLE sequence=" << sequence << " slot=" << i << " valid=" << (table.valid ? 1 : 0)
            << " address=" << std::hex << table.address << " buckets=" << table.buckets << " array=" << table.array
            << " count=" << std::dec << table.count << " capacity=" << table.capacity << " nonempty_buckets=" << table.nonempty << '\n';
    }
    for (const auto& matrix : matrices) {
        out << "CAMERA_HISTORY_MATRIX sequence=" << sequence << " role=" << matrix.role
            << " source=" << std::hex << matrix.source << " present=" << (matrix.source ? 1 : 0)
            << " valid=" << (matrix.valid ? 1 : 0) << " words=";
        if (matrix.source && matrix.valid) for (uint32_t j = 0; j < 16u; ++j)
            out << (j ? "," : "") << std::setw(8) << std::setfill('0') << matrix.words[j];
        out << std::dec << '\n';
    }
    out.close();
    if (error || out.fail()) ++budget.write_failures;
}

void RuntimeTemporalCameraFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window) {
    if (!budget.FinishBeforeCopy(window, &context)) return;
    std::ofstream out(path, budget.records ? std::ios::app : std::ios::trunc);
    if (!budget.records) Header(out);
    out << "CAMERA_HISTORY_SOURCE_END generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " copies=" << window.copies << " boundary=next_qualified_viewport_copy\n";
    out.close();
    if (out.fail()) ++budget.write_failures;
    std::cerr << "PC_CAMERA_HISTORY_SOURCE_CAPTURE generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " closed=1 boundary=next_qualified_viewport_copy path=" << path << '\n';
}
