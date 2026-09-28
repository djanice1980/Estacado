#include <rex/graphics/embedded_scene_resolve_capture_policy.h>
#include <rex/graphics/embedded_scene_transfer_capture_policy.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace p = rex::graphics::embedded_scene_resolve_capture_policy;

static std::string Read(const char* path) {
  std::ifstream file(std::string(DARKNESS_SOURCE_ROOT) + path);
  std::ostringstream result;
  result << file.rdbuf();
  return result.str();
}

int main() {
  unsigned failures = 0;
  const auto check = [&](bool value, const char* reason) {
    if (!value) { ++failures; std::cerr << reason << '\n'; }
  };
  p::ClearCommands clears;
  namespace u = rex::graphics::embedded_scene_transfer_capture_policy;
  u::FrameBudget updates;
  check(!updates.Begin(1, 1, false).budget && !updates.updates,
      "unselected updates cannot arm transfer recording");
  for (uint32_t i = 1; i <= 256; ++i) {
    auto context = updates.Begin(1, i, true);
    check(context.budget == &updates && context.update == i && !context.helper_completed,
        "selected update has a unique identity and begins incomplete");
    for (uint32_t record = 0; record < 64; ++record) check(context.Record(), "bounded update records");
    check(!context.Record() && context.records == 64 && context.dropped == 1,
        "per-update overflow explicitly prevents complete evidence");
  }
  auto exhausted = updates.Begin(1, 257, true);
  check(!exhausted.Record() && updates.records == 16384 && updates.dropped_records == 257,
      "frame-wide record bound remains effective across updates");
  exhausted.Drop(5);
  check(exhausted.dropped == 6 && updates.dropped_records == 262,
      "truncated ownership snapshot accounts for every missing record");
  check(!updates.Begin(2, 1, false).budget && updates.closed && !updates.Begin(1, 258, true).budget,
      "another frame permanently closes transfer recording");
  u::FrameBudget update_limit;
  for (uint32_t i = 1; i <= 2048; ++i)
    check(update_limit.Begin(7, 1, true).update == i, "failed draw retries keep unique update ordinals");
  check(!update_limit.Begin(7, 1, true).budget && update_limit.dropped_updates == 1,
      "failed/repeated guest draw frontier cannot bypass update bound");
  check(!clears.Record(2) && !clears.Record(0xFFFFFFFFu) && clears.count() == 0,
      "clear observer rejects targets outside the prepared depth/color pair");
  check(clears.Record(1) && !clears.Record(1) && clears.count() == 1,
      "one actual clear command per color slot");
  check(clears.Record(0) && !clears.Record(0) && clears.count() == 2 && clears.target_bits == 3,
      "at most two actual clear command records per selected resolve");
  p::Resolve motion{{100, 1, 251}, true, true, false, 1, 1,
      0x00100140, 0x14010500, 0x00030300, 0x00010000, 0x01000302, 0x02D00500,
      0x1271C000, 0x1271C000, 1966080};
  auto color = motion;
  color.control = 0x00100040;
  color.color = 0x000C0300;
  color.destination_info = 0x003C0D01;
  color.address = color.destination_base = 0x10A57000;
  color.bytes = 3932160;
  check(p::Classify(motion) == p::Kind::kMotion && p::Classify(color) == p::Kind::kSceneColor,
        "observed two native signatures");
  p::State state;
  check(!state.Observe(motion.context, false) && !state.frame, "startup/unselected resolves ignored");
  check(!state.SelectCopy(motion).copy, "copy requires a previously observed resolve");
  for (uint64_t i = 1; i <= 7; ++i) {
    auto r = i & 1 ? motion : color;
    r.context.ordinal = i;
    r.context.last_draw += i;
    check(state.Observe(r.context, true), "chronology continues after four copy reservations");
    const auto copy = state.SelectCopy(r);
    check(copy.copy == (i <= 4 ? i : 0), "two attempts per kind, no retries after allocation failure");
    check(!state.SelectCopy(r).copy && !state.Observe(r.context, true), "duplicate resolve/copy rejected");
  }
  check(state.copies == 4 && state.motion_copies == 2 && state.color_copies == 2 && state.events == 7,
        "separate copy counts and full metadata sequence");
  auto next = motion.context;
  next.frame++;
  check(!state.Observe(next, false) && state.closed, "unselected next frame permanently closes observer");
  check(!state.Observe(motion.context, true) && !state.Observe(next, true), "later manual requests cannot reopen");
  p::State bounded;
  for (uint32_t i = 1; i <= 66; ++i) {
    const p::Context c{200, i, i};
    check(bounded.Observe(c, true) == (i <= 64), "64-event storage bound");
  }
  check(bounded.events == 64 && bounded.dropped == 2, "overflow explicitly reported");
  p::State chronological;
  check(chronological.Observe({100, 2, 300}, true), "non-first ordinal remains observable but distinguishable");
  check(!chronological.Observe({100, 3, 299}, true), "draw frontier cannot regress");
  check(!chronological.SelectCopy(motion).copy, "older resolve cannot borrow a newer event's identity");
  check(!chronological.Observe({0, 0, 0}, false), "missing identity closes an active frame");

  const auto reject = [&](p::Resolve r, const char* reason) {
    check(p::Classify(r) == p::Kind::kNone, reason);
  };
  auto bad = motion; bad.metadata_selected = false; reject(bad, "unobserved metadata");
  bad = motion; bad.succeeded = false; reject(bad, "failed resolve");
  bad = motion; bad.scaled = true; reject(bad, "scaled destination");
  bad = motion; bad.scale_x = 2; reject(bad, "scaled draw");
  bad = motion; bad.control |= 1 << 9; reject(bad, "unreviewed depth-clear side effect");
  bad = motion; bad.color ^= 1; reject(bad, "other source allocation");
  bad = motion; bad.surface = 0x14000500; reject(bad, "other sample layout");
  bad = motion; bad.depth = 0; reject(bad, "other depth companion");
  bad = motion; bad.destination_info ^= 1; reject(bad, "other endian encoding");
  bad = motion; bad.destination_pitch = 0x02D00A00; reject(bad, "other dimensions");
  bad = motion; bad.address += 4; reject(bad, "unsupported partial range");
  bad = motion; bad.bytes = 0; reject(bad, "empty data");
  bad = motion; bad.bytes = 8u * 1024 * 1024 + 4; reject(bad, "oversized range");
  bad = motion; bad.address = bad.destination_base = 0x1FFFFFFC; reject(bad, "physical memory overflow");
  bad = motion; bad.address = bad.destination_base = 0; reject(bad, "zero address");
  bad = motion; bad.context.last_draw = 0; reject(bad, "no contributing draw");
  bad = color; bad.bytes += 4; reject(bad, "partial 64-bit pixel");
  p::RangeBudget ranges;
  check(!ranges.Reserve(0, 16), "invalid reservation consumes attempt");
  check(!ranges.Reserve(0x1FFFFFFC, 8), "reject complete range beyond physical memory");
  check(ranges.Reserve(color.address, color.bytes) == color.bytes, "complete GPU range");
  check(ranges.Reserve(color.address, color.bytes) == color.bytes, "same allocation later epoch is preserved");
  check(!ranges.Reserve(color.address, color.bytes) && ranges.attempts == 4 &&
      ranges.bytes == uint64_t(color.bytes) * 2, "four attempts, no silent crop or unbounded retry");

  const auto source = Read("/external/ReXGlue/src/graphics/d3d12/command_processor.cpp");
  const auto observe = source.find("embedded_scene_resolve_budget.Observe(scene_context,");
  const auto resolve = source.find("const bool resolved = render_target_cache_->Resolve(", observe);
  const auto copy = source.find("shared_memory_->QueueSceneResolveReadback(", resolve);
  const auto depth = source.find("if (!kernel_state_ && REXCVAR_GET(embedded_temporal_depth_resolve_capture))", copy);
  check(observe != std::string::npos && resolve != std::string::npos && copy != std::string::npos &&
      depth != std::string::npos && observe < resolve && resolve < copy && copy < depth,
      "reserve context before production resolve; scene copy after writes; old depth path independent");
  if (observe != std::string::npos && depth != std::string::npos) {
    const auto body = source.substr(observe, depth - observe);
    check(body.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos &&
        body.find("scene_metadata ? &scene_context : nullptr") != std::string::npos &&
        body.find("SelectCopy({") < body.find("QueueSceneResolveReadback("), "selected epoch reservation before allocation");
    check(body.find("Await") == std::string::npos && body.find("TranslatePhysical") == std::string::npos &&
        body.find("D3DDraw") == std::string::npos, "no diagnostic wait, CPU substitute or guest draw");
  }
  check(source.find("embedded_camera_scene_capture, false") != std::string::npos &&
      source.find("embedded_scene_resolve_budget.closed = true") != std::string::npos,
      "default off and sealed at selected frame publication");
  const auto pixel_allocate = source.find("if (!cbuffer_binding_float_pixel_.up_to_date) {");
  const auto pixel_source = source.find("embedded_float_pixel_cpu_address_ = float_constants;", pixel_allocate);
  const auto pixel_cursor = source.find("float_constants += 4 * sizeof(float);", pixel_allocate);
  check(pixel_allocate < pixel_source && pixel_source < pixel_cursor && pixel_cursor != std::string::npos,
      "retain beginning of actual PS upload before dense cursor advances");
  check(source.find("draw.pixel_constants.Capture(embedded_float_pixel_cpu_address_,") != std::string::npos &&
      source.find("pixel_map.float_count, pixel_map.float_bitmap") != std::string::npos &&
      source.find("draw.pixel_constant_buffer = cbuffer_binding_float_pixel_.address") != std::string::npos,
      "owned PS copy uses actual bound upload and sparse shader map");
  check(source.find("CAMERA_TARGET draw=") != std::string::npos && source.find("CAMERA_PIXEL draw=") != std::string::npos,
      "every selected draw has exact target and pixel constants");

  const auto shared = Read("/external/ReXGlue/src/graphics/d3d12/shared_memory.cpp");
  check(shared.find("scene_resolve_budget_.Reserve(address, requested_bytes)") != std::string::npos &&
      shared.find("if (!scene_resolve) {") != std::string::npos &&
      shared.find("it->scene_resolve ? \"REX_SCENE_RESOLVE_READBACK\"") != std::string::npos,
      "independent range budget and labeled completion preserve legacy counters");
  const auto readback = shared.find("bool D3D12SharedMemory::QueueBoundedReadback(");
  const auto begin = shared.find("void D3D12SharedMemory::BeginSubmission()", readback);
  if (readback != std::string::npos && begin != std::string::npos) {
    const auto body = shared.substr(readback, begin - readback);
    check(body.find("D3DCopyBufferRegion(") != std::string::npos &&
        body.find("CommitUAVWritesAndTransitionBuffer(original_state)") != std::string::npos &&
        body.find("Await") == std::string::npos && body.find("->Map(") == std::string::npos,
        "existing deferred copy restores source state without map or wait");
  }
  const auto rt = Read("/external/ReXGlue/src/graphics/d3d12/render_target_cache.cpp");
  const auto update_begin = source.find("auto scene_update = embedded_scene_update_budget.Begin(");
  const auto borrowed = source.find("SetSceneUpdateCapture(scene_update.budget ? &scene_update : nullptr)", update_begin);
  const auto update_call = source.find("const bool render_target_update_succeeded = render_target_cache_->Update(", borrowed);
  const auto released = source.find("SetSceneUpdateCapture(nullptr)", update_call);
  const auto failure_return = source.find("if (!render_target_update_succeeded)", update_call);
  check(update_begin != std::string::npos && update_begin < borrowed && borrowed < update_call &&
      update_call < released && released < failure_return,
      "borrowed diagnostic context is cleared before every failed-update return");
  if (update_begin != std::string::npos && borrowed != std::string::npos) {
    const auto gate = source.substr(update_begin, borrowed - update_begin);
    check(gate.find("embedded_camera_draw_capture") != std::string::npos &&
        gate.find("embedded_camera_scene_capture") != std::string::npos &&
        gate.find("IsCurrentEmbeddedGameplayCaptureFrame()") != std::string::npos,
        "actual update capture requires both default-off camera observers and selected frame");
  }
  const auto targets_observer = rt.find("void D3D12RenderTargetCache::RecordSceneUpdateTargets(");
  const auto transfer_helper = rt.find("void D3D12RenderTargetCache::PerformTransfersAndResolveClears(", targets_observer);
  if (targets_observer != std::string::npos && transfer_helper != std::string::npos) {
    const auto body = rt.substr(targets_observer, transfer_helper - targets_observer);
    check(body.find("CopyOwnershipSnapshot(owners") != std::string::npos &&
        body.find("OwnershipSnapshot owners[32]") != std::string::npos &&
        body.find("transfer.start_tiles, transfer.end_tiles") != std::string::npos &&
        body.find("c.Drop(c.owners") != std::string::npos,
        "bounded snapshots expose actual bookkeeping and complete planned transfer ranges");
    for (const char* forbidden : {"Await", "D3DDraw", "SubmitBarriers", "Queue", "CreateRenderTarget"})
      check(body.find(forbidden) == std::string::npos, "target observer must remain read-only");
  } else check(false, "actual update target observer exists");
  const auto transfer_draw = rt.find("command_list.D3DDrawInstanced(transfer_vertex_count, 1, 0, 0);", transfer_helper);
  const auto transfer_record = rt.find("REX_SCENE_UPDATE_COMMAND", transfer_draw);
  check(transfer_draw != std::string::npos && transfer_draw < transfer_record && transfer_record - transfer_draw < 1200,
      "executed transfer metadata follows the actual queued draw");
  for (const char* reason : {"host_depth_store_descriptors", "transfer_descriptors", "transfer_vertex_allocation", "transfer_pipeline"})
    check(rt.find(std::string("record_update_failure(\"") + reason + "\")", transfer_helper) != std::string::npos,
        "existing transfer failure branches remain explicit evidence failures");
  check(rt.find("if (update_capture) update_capture->helper_completed = true;", transfer_record) != std::string::npos &&
      source.find("CAMERA_SCENE_UPDATES frame=") != std::string::npos &&
      source.find("embedded_scene_update_budget.closed = true") != std::string::npos,
      "helper completion and frame overflow/sealing are independently reported");
  const auto prepare = rt.find("const bool clear_prepared = PrepareHostRenderTargetsResolveClear(");
  const auto prepare_log = rt.find("REX_SCENE_CLEAR_PREPARE", prepare);
  const auto prepared_branch = rt.find("if (clear_prepared)", prepare);
  check(prepare != std::string::npos && prepare < prepare_log && prepare_log < prepared_branch &&
      rt.find("&clear_rectangle, scene_capture_context);", prepared_branch) != std::string::npos,
      "record actual preparation result and forward selected identity only into existing prepared branch");
  const auto command_observer = rt.find("const auto record_scene_clear_command =");
  const auto command_observer_end = rt.find("if (dest_rt_key.is_depth)", command_observer);
  check(command_observer != std::string::npos && command_observer_end != std::string::npos,
      "actual host clear command observer exists");
  if (command_observer != std::string::npos && command_observer_end != std::string::npos) {
    const auto body = rt.substr(command_observer, command_observer_end - command_observer);
    check(body.find("!scene_capture_context || render_target_count != 2") != std::string::npos &&
        body.find("scene_clear_command_budget.Record(i)") != std::string::npos &&
        body.find("std::memcpy(words, values") != std::string::npos &&
        body.find("clear_rect.right") != std::string::npos && body.find("GetDesc()") != std::string::npos,
        "bounded actual resource/rectangle/clear-argument observation");
    check(body.find("Await") == std::string::npos && body.find("Queue") == std::string::npos &&
        body.find("D3DDraw") == std::string::npos && body.find("SubmitBarriers") == std::string::npos,
        "clear metadata does not issue GPU work or consume readback budgets");
  }
  for (const char* method : {"depth_stencil_clear", "uint_draw", "rtv_clear"}) {
    const auto call = rt.find(std::string("record_scene_clear_command(\"") + method + "\"", command_observer_end);
    const char* command = std::string(method) == "depth_stencil_clear" ? "command_list.D3DClearDepthStencilView(" :
        std::string(method) == "uint_draw" ? "command_list.D3DDrawInstanced(3, 1, 0, 0);" : "command_list.D3DClearRenderTargetView(";
    const auto actual = rt.find(command, command_observer_end);
    check(actual != std::string::npos && call != std::string::npos && actual < call && call - actual < 550,
        "metadata follows the corresponding existing host clear command");
  }
  check(rt.find("REX_SCENE_CLEAR_END") != std::string::npos &&
      rt.find("scene_clear_command_budget.target_bits") != std::string::npos,
      "explicit actual clear-command completeness footer");
  const auto metadata = rt.find("if (scene_capture_context && GetPath() == Path::kHostRenderTargets)");
  const auto metadata_end = rt.find("draw_util::ResolveCopyShaderConstants copy_shader_constants;", metadata);
  check(metadata != std::string::npos && metadata_end != std::string::npos, "bounded actual resolve-layout observation");
  if (metadata != std::string::npos && metadata_end != std::string::npos) {
    const auto body = rt.substr(metadata, metadata_end - metadata);
    check(body.find("GetResolveCopyRectanglesToDump(") != std::string::npos &&
        body.find("resolve_info.copy_dest_rect[3]") != std::string::npos &&
        body.find("scene_owner_trace_count == 128") != std::string::npos &&
        body.find("REX_SCENE_RESOLVE_OWNERS") != std::string::npos,
        "actual post-clipping layout and capped owners with explicit completeness counts");
    check(body.find("QueueDepthSourceReadback") == std::string::npos && body.find("Await") == std::string::npos,
        "passive metadata does not consume depth copies or wait");
  }
  return failures ? 1 : 0;
}
