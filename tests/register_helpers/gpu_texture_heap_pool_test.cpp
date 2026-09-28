// Texture heap pool (V300): the offset allocator never overlaps live ranges,
// honors alignment and coalesces; the D3D12 texture cache places textures in
// heaps before falling back to committed resources and releases a placed
// resource before its range can be reused.
#include <rex/graphics/offset_allocator.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

int main() {
  bool passed = true;
  const auto check = [&passed](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      passed = false;
    }
  };
  using rex::graphics::OffsetAllocator;
  constexpr uint64_t k64K = 65536, k4K = 4096;

  {
    OffsetAllocator a(4 * k64K);
    uint64_t o1, o2, o3;
    check(a.Allocate(k4K, k4K, o1) && o1 == 0, "first small allocation at 0");
    check(a.Allocate(k64K, k64K, o2) && o2 == k64K, "64 KB alignment skips to the next boundary");
    check(a.Allocate(k4K, k4K, o3) && o3 == k4K, "small allocation fills the gap before it");
    check(a.free_bytes() == 4 * k64K - k64K - 2 * k4K, "free bytes track allocations");
    uint64_t big;
    check(!a.Allocate(3 * k64K, k64K, big), "no fit beyond capacity");
    a.Free(o2, k64K);
    a.Free(o1, k4K);
    a.Free(o3, k4K);
    check(a.free_bytes() == 4 * k64K && a.free_ranges() == 1, "frees coalesce to one range");
    check(a.Allocate(4 * k64K, k64K, big) && big == 0, "full capacity available again");
    check(!a.Allocate(k4K, 3, big), "non power-of-two alignment rejected");
  }
  {
    // Randomized: live ranges never overlap and all bytes come back.
    OffsetAllocator a(64 * k64K);
    std::mt19937 rng(1234);
    struct Live { uint64_t offset, size; };
    std::vector<Live> live;
    bool overlap = false;
    for (int step = 0; step < 4000; ++step) {
      if (!live.empty() && (rng() % 3 == 0)) {
        const size_t i = rng() % live.size();
        a.Free(live[i].offset, live[i].size);
        live.erase(live.begin() + ptrdiff_t(i));
        continue;
      }
      const bool small = rng() % 2;
      const uint64_t size = small ? k4K * (1 + rng() % 16) : k64K * (1 + rng() % 4);
      const uint64_t alignment = small ? k4K : k64K;
      uint64_t offset;
      if (!a.Allocate(size, alignment, offset)) continue;
      if (offset % alignment) overlap = true;
      for (const Live& l : live) {
        if (offset < l.offset + l.size && l.offset < offset + size) overlap = true;
      }
      live.push_back({offset, size});
    }
    check(!overlap, "no overlapping or misaligned live ranges");
    for (const Live& l : live) a.Free(l.offset, l.size);
    check(a.free_bytes() == 64 * k64K && a.free_ranges() == 1, "all ranges return and coalesce");
  }
  {
    // Source policy.
    std::ifstream file(std::string(REXGLUE_SOURCE_ROOT) + "/src/graphics/d3d12/texture_cache.cpp");
    const std::string source((std::istreambuf_iterator<char>(file)), {});
    const auto placed = source.find(
        "resource = CreatePlacedTexture(desc, resource_state, heap_index, heap_offset, heap_size);");
    const auto committed = source.find("if (!resource &&", placed);
    check(placed != std::string::npos && committed != std::string::npos,
          "heap placement is tried before a committed resource");
    const auto dtor = source.find("D3D12TextureCache::D3D12Texture::~D3D12Texture()");
    const auto reset = source.find("resource_.Reset();", dtor);
    const auto free_range = source.find("FreeTextureHeapRange(heap_index_, heap_offset_, heap_size_);", dtor);
    check(dtor != std::string::npos && reset != std::string::npos && free_range > reset,
          "placed resource released before its range returns to the pool");
    check(source.find("REXCVAR_DEFINE_BOOL(d3d12_texture_heap_pool, true,") != std::string::npos,
          "pool enabled by default, switchable");
    check(source.find("D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES") != std::string::npos,
          "heaps hold non-render-target textures only (tier 1 compatible)");
  }

  if (!passed) return 1;
  std::cout << "gpu texture heap pool: PASS\n";
  return 0;
}
