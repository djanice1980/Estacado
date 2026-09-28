#include "runtime_motion_delta.h"
#include "runtime_motion_origins.h"
#include "ppc_recomp_shared.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
RuntimeMotionOriginBudget budget;
constexpr const char* path = "logs/pc_motion_delta.log";

const char* SiteName(RuntimeMotionDeltaSite site) {
    switch (site) {
    case RuntimeMotionDeltaSite::CameraSource: return "camera_source";
    case RuntimeMotionDeltaSite::ViewCallback: return "view_callback";
    case RuntimeMotionDeltaSite::Inverse: return "inverse_input";
    case RuntimeMotionDeltaSite::HistoryStore: return "history_store_input";
    default: return "invalid";
    }
}

void Header(std::ostream& out) {
    out << "MOTION_DELTA_CAPTURE version=1 generation=" << budget.generation
        << " maximum_records=" << RuntimeMotionOriginBudget::kMaximumRecords
        << " maximum_copies=" << RuntimeMotionOriginBudget::kMaximumCopies
        << " scope=native_delta_selection_not_previous_frame_acceptance\n";
}
}

void RuntimeMotionDeltaReset() { budget = {}; }
uint32_t RuntimeMotionDeltaObservedCount() noexcept { return budget.records; }

void RuntimeMotionDeltaObserve(PPCContext& context, uint8_t* base, uint32_t address,
                              const RuntimeCameraCaptureWindow& window) {
    const uint32_t caller = uint32_t(context.lr), stack = context.r1.u32;
    const auto site = RuntimeMotionDeltaFunction(address, caller);
    if (site == RuntimeMotionDeltaSite::None || !budget.Select(window, &context)) return;
    const bool camera_source = site == RuntimeMotionDeltaSite::CameraSource;
    const bool callback = site == RuntimeMotionDeltaSite::ViewCallback;
    const bool inverse = site == RuntimeMotionDeltaSite::Inverse;
    const bool item_site = !camera_source && !callback;
    uint32_t owner = camera_source ? context.r3.u32 : callback ? context.r4.u32 : 0u;
    uint32_t state = callback ? context.r3.u32 : item_site ? context.r25.u32 : 0u;
    uint32_t camera = item_site ? context.r19.u32 : 0u;
    uint32_t array = 0, count = 0, capacity = 0, item = 0, object = 0, flags = 0;
    uint32_t entity = 0, entity_table = 0, entity_count = 0, key_entity = 0, key_part = 0;
    uint32_t slot = 0, enabled = 0, factor = 0, previous_table = 0, next_table = 0;
    uint32_t buckets = 0, history_array = 0, history_count = 0, history_capacity = 0;
    uint32_t selected_history = 0, lookup_steps = 0, inverse_input = 0, inverse_work = 0;
    uint32_t working_current = 0, store_current = 0, source_c = 0, source_v = 0;
    uint32_t phase = callback ? context.r6.u32 : item_site ? context.r14.u32 : 0u;
    bool valid = RuntimeMotionSourceRange(stack, item_site ? 576u : 4u, 16u);
    bool lookup_complete = true;
    if (item_site) {
        valid = valid && RuntimeMotionSourceRange(camera, 2184u, 16u);
        if (valid) owner = PPC_LOAD_U32(camera + 12u);
    }
    valid = valid && RuntimeMotionSourceRange(owner, 1264u);
    if (valid && callback) camera = PPC_LOAD_U32(owner + 224u);
    if (valid && !camera_source) valid = RuntimeMotionSourceRange(camera, 2184u, 16u);
    if (valid && camera_source) { source_c = context.r7.u32; source_v = context.r8.u32; }
    if (valid && !camera_source) {
        array = PPC_LOAD_U32(camera + 1508u);
        count = PPC_LOAD_U32(camera + 2176u);
        capacity = PPC_LOAD_U32(camera + 2172u);
        valid = RuntimeMotionSourceRange(state, 3496u) && state >= 448u;
        if (valid) {
            slot = PPC_LOAD_U32(state + 2312u);
            enabled = PPC_LOAD_U32(state + 2316u);
            factor = PPC_LOAD_U32(state + 2320u);
            // Native double-buffer index, not a GPU frame identity.
            valid = slot <= 1u;
            if (valid) {
                previous_table = state + 2224u + ((slot - 1u) & 1u) * 44u;
                next_table = state + 2224u + slot * 44u;
            }
        }
    }
    if (valid && item_site) {
        valid = context.r31.u32 >= 80u && PPC_LOAD_U32(owner + 224u) == camera;
        if (valid) {
            item = context.r31.u32 - 80u;
            valid = RuntimeMotionDeltaItem(array, count, capacity, item);
        }
        if (valid) {
            object = PPC_LOAD_U32(item + 216u);
            flags = PPC_LOAD_U32(item + 224u);
            key_entity = PPC_LOAD_U16(item + 180u);
            key_part = PPC_LOAD_U16(item + 182u);
            entity = context.r28.u32;
            const uint32_t table = PPC_LOAD_U32(state + 3492u);
            valid = RuntimeMotionSourceRange(table, 28u) && RuntimeMotionSourceRange(entity, 208u, 16u);
            if (valid) {
                entity_table = PPC_LOAD_U32(table + 24u);
                entity_count = PPC_LOAD_U32(table + 4u);
                const uint64_t entry = uint64_t(entity_table) + uint64_t(key_entity) * 4u;
                valid = key_entity && key_entity < entity_count && entity_table && entry <= UINT32_MAX - 3u &&
                    RuntimeMotionSourceRange(uint32_t(entry), 4u) && PPC_LOAD_U32(uint32_t(entry)) == entity;
            }
        }
        if (valid) {
            buckets = PPC_LOAD_U32(previous_table + 20u);
            history_array = PPC_LOAD_U32(previous_table + 36u);
            history_count = PPC_LOAD_U32(previous_table + 40u);
            history_capacity = PPC_LOAD_U32(previous_table + 8u);
            valid = history_count <= history_capacity && history_capacity <= 32767u &&
                RuntimeMotionSourceRange(buckets, 512u, 2u);
            if (valid) {
                int32_t index = int16_t(PPC_LOAD_U16(buckets + 2u * RuntimeMotionDeltaKeyBucket(
                    uint16_t(key_entity), uint16_t(key_part), object)));
                while (index != -1 && lookup_steps < 256u) {
                    const uint32_t record = RuntimeMotionDeltaHistoryRecord(history_array, history_count, uint32_t(index));
                    if (!record) { valid = false; break; }
                    ++lookup_steps;
                    if (PPC_LOAD_U16(record + 64u) == key_entity && PPC_LOAD_U16(record + 66u) == key_part &&
                        PPC_LOAD_U32(record + 68u) == object) { selected_history = record; break; }
                    index = int16_t(PPC_LOAD_U16(record + 86u));
                }
                lookup_complete = index == -1 || selected_history;
            }
            if (inverse) {
                inverse_input = context.r3.u32;
                const uint32_t expected_output = stack + (caller == 0x82499A38u ? 512u : 208u);
                valid = valid && context.r4.u32 == expected_output &&
                    inverse_input == (caller == 0x82499664u ? stack + 448u : entity + 144u);
            } else {
                valid = valid && context.r3.u32 == next_table && context.r4.u32 == stack + 272u &&
                    PPC_LOAD_U16(stack + 336u) == key_entity && PPC_LOAD_U16(stack + 338u) == key_part &&
                    PPC_LOAD_U32(stack + 340u) == object;
                inverse_work = stack + 208u;
                store_current = context.r4.u32;
                working_current = stack + 128u;
            }
        }
    }
    struct Matrix { const char* role; uint32_t source; bool valid{}; uint32_t words[16]{}; } matrices[] = {
        {"camera_c", camera ? camera + 1760u : 0u}, {"camera_v", camera ? camera + 1824u : 0u},
        {"item_current", item}, {"item_delta", item ? item + 64u : 0u},
        {"entity_current", entity ? entity + 80u : 0u}, {"entity_other", entity ? entity + 144u : 0u},
        {"selected_history", selected_history}, {"inverse_input", inverse_input},
        {"inverse_work", inverse_work}, {"working_current", working_current},
        {"store_current", store_current}, {"source_c", source_c}, {"source_v", source_v}
    };
    bool all_valid = valid;
    for (auto& matrix : matrices) {
        matrix.valid = valid && (!matrix.source || RuntimeMotionSourceRange(matrix.source, 64u, 16u));
        all_valid = all_valid && matrix.valid;
        if (matrix.source && matrix.valid) for (uint32_t i = 0; i < 16u; ++i)
            matrix.words[i] = PPC_LOAD_U32(matrix.source + i * 4u);
    }
    if (!all_valid) ++budget.invalid;
    const uint32_t sequence = budget.records - 1u;
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::ofstream out(path, sequence ? std::ios::app : std::ios::trunc);
    if (!sequence) Header(out);
    out << "MOTION_DELTA sequence=" << sequence << " generation=" << budget.generation
        << " window_copy_sequence=" << window.copies - 1u
        << " origin_records_seen=" << RuntimeMotionOriginsObservedCount()
        << " site=" << SiteName(site) << " valid=" << (all_valid ? 1 : 0)
        << " lookup_complete=" << (lookup_complete ? 1 : 0) << " lookup_steps=" << lookup_steps
        << " function=" << std::hex << address << " caller=" << caller << " stack=" << stack
        << " owner=" << owner << " camera=" << camera << " state=" << state << " phase=" << phase
        << " array=" << array << " count=" << count << " capacity=" << capacity << " item=" << item
        << " object=" << object << " flags=" << flags << " key_entity=" << key_entity << " key_part=" << key_part
        << " entity=" << entity << " entity_table=" << entity_table << " entity_count=" << entity_count
        << " slot=" << slot << " enabled=" << enabled << " factor=" << factor
        << " previous_table=" << previous_table << " next_table=" << next_table
        << " buckets=" << buckets << " history_array=" << history_array << " history_count=" << history_count
        << " history_capacity=" << history_capacity << " selected_history=" << selected_history << std::dec << '\n';
    for (const auto& matrix : matrices) {
        out << "MOTION_DELTA_MATRIX sequence=" << sequence << " role=" << matrix.role
            << " source=" << std::hex << matrix.source << " present=" << (matrix.source ? 1 : 0)
            << " valid=" << (matrix.valid ? 1 : 0) << " words=";
        if (matrix.source && matrix.valid) for (uint32_t i = 0; i < 16u; ++i)
            out << (i ? "," : "") << std::setw(8) << std::setfill('0') << matrix.words[i];
        out << std::dec << '\n';
    }
    out.close();
    if (error || out.fail()) ++budget.write_failures;
}

void RuntimeMotionDeltaFinish(PPCContext& context, const RuntimeCameraCaptureWindow& window) {
    if (!budget.FinishBeforeCopy(window, &context)) return;
    std::ofstream out(path, budget.records ? std::ios::app : std::ios::trunc);
    if (!budget.records) Header(out);
    out << "MOTION_DELTA_END generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " copies=" << window.copies << " boundary=next_qualified_viewport_copy\n";
    out.close();
    if (out.fail()) ++budget.write_failures;
    std::cerr << "PC_MOTION_DELTA_CAPTURE generation=" << budget.generation << " records=" << budget.records
        << " dropped=" << budget.dropped << " invalid=" << budget.invalid << " write_failures=" << budget.write_failures
        << " closed=1 boundary=next_qualified_viewport_copy path=" << path << '\n';
}
