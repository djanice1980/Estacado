#include <rex/graphics/pipeline/texture/native_resolve_regions.h>
#include <rex/graphics/shared_memory_watch_policy.h>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
int main() {
  using namespace rex::graphics::native_resolve;
  int failed = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) { std::cerr << name << '\n'; ++failed; }
  };
  const Layout atlas{0x11FED000, 1280, 26, 2, 3};
  const Write light{atlas, {0, 0, 320, 184}, 0x11FED000, 1720320, true};
  const Write neighbor{atlas, {320, 0, 640, 184}, 0x12001000, 1720320, true};
  RegionMap map;
  Rect region;
  map.Record(neighbor, false);
  check(!map.Latest(atlas, region), "scaled-only resource classified native");
  map.Record(light, true);
  check(map.Find(atlas, {0, 0, 320, 180}, region), "native filter result missing");
  map.Record(neighbor, false);
  check(map.Find(atlas, {0, 0, 320, 180}, region), "disjoint scaled write erased native light");
  check(!map.Find(atlas, {319, 0, 321, 2}, region), "mixed bilinear footprint accepted");
  check(!map.Find(atlas, {320, 0, 640, 180}, region), "scaled neighbor classified native");
  auto foreign = atlas; foreign.base = 0x10A59000;
  check(!map.Latest(foreign, region), "background classified native");
  map.Record(light, true);
  check(map.size() == 1, "repeated native filter grows metadata");
  check(!map.Find(atlas, {320, 0, 640, 180}, region), "native write downgraded neighbor");
  map.Invalidate(0x12001000, 4);
  check(!map.Latest(atlas, region), "CPU/unknown GPU alias write failed to invalidate");
  map.Record(light, true);
  auto overlapping = neighbor; overlapping.rect = {312, 0, 640, 184};
  map.Record(overlapping, false);
  check(!map.Latest(atlas, region), "overlapping rectangle failed to invalidate");
  for (unsigned kind = 0; kind < 6; ++kind) {
    map.Record(light, true);
    auto alias = neighbor;
    switch (kind) {
      case 0: alias.layout.base += 4096; break;
      case 1: alias.layout.pitch = 1024; break;
      case 2: alias.layout.format = 6; break;
      case 3: alias.layout.endian = 0; break;
      case 4: alias.layout.bytes_per_pixel_log2 = 2; break;
      case 5: alias.layout_known = false; break;
    }
    map.Record(alias, false);
    check(!map.Latest(atlas, region), "foreign layout preserved stale provenance");
  }
  map.Record(light, true);
  map.Invalidate(0, 4096);
  check(map.Latest(atlas, region), "unrelated write erased native region");
  auto malformed = light;
  malformed.extent_start = UINT32_MAX - 3; malformed.extent_length = 16;
  map.Record(malformed, true);
  check(map.size() == 0, "wrapping write established provenance");
  malformed = light; malformed.rect.right = 1281;
  map.Record(malformed, true);
  check(map.size() == 0, "out-of-pitch rectangle established provenance");
  for (unsigned i = 0; i < RegionMap::kCapacity + 2; ++i) {
    auto separate = light;
    separate.layout.base = i * 0x200000; separate.extent_start = separate.layout.base;
    map.Record(separate, true);
  }
  check(map.size() == RegionMap::kCapacity, "metadata capacity not bounded");
  auto first = atlas; first.base = 0;
  check(!map.Latest(first, region), "old entry not evicted conservatively");
  map.Clear(); check(map.size() == 0, "clear failed");
  int owner = 0, other = 0;
  using rex::graphics::shared_memory_watch_policy::ShouldNotify;
  check(!ShouldNotify(&owner, &owner, true), "known resolve erased its precise geometry");
  check(ShouldNotify(&other, &owner, true), "known resolve suppressed another watcher");
  check(ShouldNotify(&owner, &owner, false), "CPU callback was suppressed");
  check(ShouldNotify(&owner, nullptr, true), "unknown/nested GPU callback was suppressed");
  auto read = [](const std::string& relative) {
    std::ifstream in(std::string(DARKNESS_SOURCE_ROOT) + relative);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  const auto header = read("/external/ReXGlue/include/rex/graphics/pipeline/shader/dxbc_translator.h");
  check(header.find("native_region_sampling : 1") != std::string::npos,
        "native region variant not represented in shader cache identity");
  check(header.find("kVersion = 0x20260908") != std::string::npos,
        "native region translator cache version not advanced");
  const auto old_tail = header.find("float edram_blend_constant[4];");
  const auto regions = header.find("float native_texture_regions[32][4];");
  check(old_tail != std::string::npos && regions != std::string::npos && old_tail < regions,
        "new system constants shift old vertex/geometry offsets");
  const auto source = read("/external/ReXGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp");
  check(source.find("native_region_sampling = !native_filter_sampling") != std::string::npos,
        "region sampling overrides the verified image-filter footprint");
  return failed ? 1 : 0;
}
