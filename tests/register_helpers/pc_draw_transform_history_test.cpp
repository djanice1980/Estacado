#include <rex/graphics/pc_draw_transform_history.h>
#include <iostream>
#include <memory>

using namespace rex::graphics::pc_draw_transform_history;
int main() {
  bool ok = true;
  Draw draw;
  draw.vertex_shader = UINT64_C(0x81D611665A691E95);
  std::array<uint64_t, 4> map{};
  // Include unrelated registers between required rows, plus distant c255.
  for (uint32_t r : {0u,1u,2u,3u,5u,7u,8u,12u,13u,14u,15u,255u})
    map[r / 64] |= UINT64_C(1) << (r % 64);
  std::array<uint32_t, 48> upload{};
  for (uint32_t i = 0; i < upload.size(); ++i) upload[i] = 0x3F000000 + i;
  ok &= draw.ReadUpload(upload.data(), sizeof(upload), 12, map.data());
  ok &= draw.constants[16] == upload[20] && draw.constants[20] == upload[28];
  const auto saved = draw.constants;
  upload.fill(0);
  ok &= draw.constants == saved;  // Owned, independent of upload lifetime.
  ok &= !draw.ReadUpload(nullptr, sizeof(upload), 12, map.data());
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload)-1, 12, map.data());
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 11, map.data());
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 13, map.data());
  map[0] &= ~(UINT64_C(1) << 7);
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 11, map.data());
  map[0] |= UINT64_C(1) << 7;
  upload[0] = 0x7FC00000;
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 12, map.data());
  upload[0] = 0xFF800000;
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 12, map.data());
  upload[0] = 0;
  draw.vertex_shader = 123;
  ok &= !draw.ReadUpload(upload.data(), sizeof(upload), 12, map.data());
  draw.vertex_shader = UINT64_C(0x87355DB0803F82C2);
  ok &= draw.ReadUpload(upload.data(), sizeof(upload), 12, map.data());
  auto pending_storage = std::make_unique<Frame>();
  Frame& pending = *pending_storage;
  pending.Add(10, draw, true);
  ok &= !pending.Valid(10);
  pending.Seal(10);
  ok &= pending.Valid(10) && !pending.Valid(11);
  auto owned_storage = std::make_unique<Frame>(pending);
  Frame& owned = *owned_storage;
  pending.Reset();
  ok &= owned.Valid(10) && !pending.Valid(10);
  pending.Add(11, draw, true);
  pending.Seal(11);
  pending.Add(11, draw, true); // Late motion after selected final source.
  ok &= !pending.Valid(11);
  pending.Reset();
  pending.Seal(12);
  ok &= !pending.Valid(12);
  pending.Reset();
  pending.Add(13, draw, true);
  pending.Add(14, draw, true);
  pending.Seal(13);
  ok &= !pending.Valid(13);
  pending.Reset();
  pending.Add(15, draw, false);
  pending.Add(15, draw, true);
  pending.Seal(15);
  ok &= !pending.Valid(15);
  pending.Reset();
  for (uint32_t i = 0; i < Frame::kMaximumDraws; ++i) pending.Add(16, draw, true);
  pending.Seal(16);
  ok &= pending.Valid(16);
  pending.Add(16, draw, true);
  ok &= !pending.Valid(16) && pending.count == Frame::kMaximumDraws;
  pending.Reset();
  pending.Add(17, draw, true);
  pending.Seal(17);
  pending.Seal(17);
  ok &= !pending.Valid(17);
  rex::graphics::pc_owned_camera_packet::Source camera;
  ok &= !draw.CopyOwnedCamera(camera);
  for (uint32_t i = 0; i < 16; ++i) {
    auto& w = draw.writers[20 + i];
    w.sequence = i + 1; w.value = draw.constants[20 + i];
    w.execution.buffer = 11; w.execution.packet = 12; w.execution.depth = 1;
    auto& s = w.camera_source;
    s.packet = 1; s.constant = 2; s.publication = 3; s.item = 4;
    s.first_register = 0x4030; s.words = 32; s.camera_current[0] = 0x3F800000;
  }
  ok &= draw.CopyOwnedCamera(camera) && camera.publication == 3 && camera.item == 4;
  auto valid_draw = draw;
  // An identical ordinary/bulk write still loses source identity.
  draw.writers[25].camera_source = {};
  ok &= !draw.CopyOwnedCamera(camera);
  draw = valid_draw; draw.writers[25].value ^= 1;
  ok &= !draw.CopyOwnedCamera(camera);
  draw = valid_draw; ++draw.writers[25].camera_source.publication;
  ok &= !draw.CopyOwnedCamera(camera);
  draw = valid_draw; ++draw.writers[25].execution.packet;
  ok &= !draw.CopyOwnedCamera(camera);
  draw = valid_draw; ++draw.writers[25].camera_source.camera_current[0];
  ok &= !draw.CopyOwnedCamera(camera);
  // Replayed packet has a new execution identity while retaining owned source.
  draw = valid_draw;
  for (auto& w : draw.writers) { w.execution.buffer = 21; w.execution.packet = 22; }
  ok &= draw.CopyOwnedCamera(camera) && camera.packet == 1;
  // Follow-up observation starts after proof, skips unsupported frames and is
  // bounded even if more proofs arrive. Frame rewind closes it conservatively.
  rex::graphics::pc_draw_transform_history::FollowupSamples followup;
  ok &= !followup.Select(99, false, true);
  ok &= !followup.Select(100, true, true) && !followup.Select(101, true, true);
  ok &= !followup.Select(116, false, true) && !followup.Select(117, false, false);
  ok &= followup.Select(118, false, true) && !followup.Select(118, false, true);
  ok &= !followup.Select(119, true, true); // cannot restart after sampling begins
  ok &= !followup.Select(164, false, true) && followup.Select(165, false, true);
  ok &= !followup.Select(356, false, true) && followup.Select(357, false, true);
  ok &= !followup.Select(1000, true, true) && !followup.Select(2000, false, true);
  rex::graphics::pc_draw_transform_history::FollowupSamples rewind;
  ok &= !rewind.Select(100, true, true) && !rewind.Select(99, false, true);
  ok &= !rewind.Select(1000, false, true);
  if (!ok) std::cerr << "Owned draw input association/lifetime checks failed\n";
  return ok ? 0 : 1;
}
