#include <rex/graphics/pc_draw_transform_history.h>
#include <fstream>
#include <iostream>
#include <memory>

using namespace rex::graphics::pc_sparse_projection;
using rex::graphics::pc_draw_transform_history::Draw;
using rex::graphics::pc_draw_transform_history::Frame;

Draw Sample() {
  Draw d;
  d.vertex_shader = UINT64_C(0x81D611665A691E95);
  d.constants = {0xBE8DEEB3,0x3F846E53,0x2F449D3D,0,
      0xBDAB7A9A,0xBCB7C82E,0x3FF37C0B,0,
      0xBF770B2E,0xBE846255,0xBD3A79D6,0xBFE66BB6,
      0xBF77057D,0xBE845F48,0xBD3A758A,0,
      0xC2C74B23,0xC1F96C1A,0xC15F0C70,0,
      0xBE848272,0x3F77471C,0x2F378FA0,0xC08A7000,
      0x3D341B34,0x3C41073E,0xBF7FBC0F,0x4112D5B0,
      0xBF77057D,0xBE845F48,0xBD3A758A,0x42D1B1C0,
      0,0,0,0x3F800000};
  return d;
}

int main(int argc, char** argv) {
  // Optional local captured-input audit uses this exact production extractor.
  // Each row supplies36 owned words and16 independently recorded native words.
  if (argc == 2) {
    std::ifstream input(argv[1]);
    unsigned count = 0;
    while (input >> std::ws && input.peek() != EOF) {
      std::array<uint32_t,36> words{};
      std::array<uint32_t,16> expected{};
      for (auto& word : words) if (!(input >> std::hex >> word)) return 2;
      for (auto& word : expected) if (!(input >> std::hex >> word)) return 2;
      const auto p = Extract(words);
      if (!p.valid || p.words != expected) return 3;
      ++count;
    }
    std::cout << "Exact production projection extractions: " << count << '\n';
    return count ? 0 : 4;
  }
  bool ok = true;
  auto d = Sample();
  const auto p = Extract(d.constants);
  ok &= p.valid && p.words[0] == 0x3F891A29 && p.words[5] == 0xBFF3BCBB &&
        p.words[10] == 0x3F8002F3 && p.words[11] == 0x3F800000 &&
        p.words[14] == 0xBFE66BB6;
  for (unsigned i : {3u,7u,15u,19u,32u,35u,12u}) {
    auto changed = d.constants;
    changed[i] ^= 1;
    ok &= !Extract(changed).valid;
  }
  auto bad = d.constants;
  bad[0] ^= 64;  // One row component can no longer use the same scale.
  ok &= !Extract(bad).valid;
  bad = d.constants; bad[20] = 0x7FC00000;
  ok &= !Extract(bad).valid;
  bad = d.constants; bad[11] = 0;
  ok &= !Extract(bad).valid;
  uint32_t factor = 0;
  const uint32_t zero[3]{};
  ok &= !UniqueFactor(zero,zero,factor);
  const uint32_t tiny[3]{1,0,0};
  ok &= !UniqueFactor(tiny,tiny,factor);
  const uint32_t ambiguous_x[3]{0x3F420A7D,0x3F420A7D,0x3F420A7D};
  const uint32_t ambiguous_y[3]{0x3F800053,0x3F800053,0x3F800053};
  // Both adjacent factors3FA8DF8B/3FA8DF8C round to every observed product.
  ok &= !UniqueFactor(ambiguous_x,ambiguous_y,factor);
  // Exact unique positive and negative scale, including zero-sign handling.
  const uint32_t x[3]{0x3F800000,0x3F000000,0xBF000000};
  const uint32_t y[3]{0xC0000000,0xBF800000,0x3F800000};
  ok &= UniqueFactor(x,y,factor) && factor == 0xC0000000;
  auto first_storage = std::make_unique<Frame>();
  auto& first = *first_storage;
  first.Add(10,d,true); first.Seal(10);
  auto next_storage = std::make_unique<Frame>();
  auto& next = *next_storage;
  next.Add(11,d,true); next.Seal(11);
  ok &= first.projection.valid && next.SameProjection(first);
  next.draws[0].viewport[0] ^= 1;
  ok &= !next.SameProjection(first);
  next.Reset();
  auto changed = d;
  changed.constants[11] = 0xC0000000; // Supported but different near-depth offset.
  next.Add(11,changed,true); next.Seal(11);
  ok &= next.projection.valid && !next.SameProjection(first);
  next.Reset();
  next.Add(11,d,true); next.Add(11,changed,true); next.Add(11,d,true); next.Seal(11);
  ok &= next.Valid(11) && !next.projection.valid; // Mixed never recovers mid-frame.
  next.Reset();
  next.Add(12,d,true); next.Seal(12);
  ok &= !next.SameProjection(first); // No equality across a missing frame.
  next.failed = true;
  ok &= !next.SameProjection(first);
  if (!ok) std::cerr << "Sparse projection/continuity checks failed\n";
  return ok ? 0 : 1;
}
