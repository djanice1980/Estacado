#include <rex/graphics/embedded_geometry_readback_policy.h>
#include <iostream>
int main() {
  namespace p = rex::graphics::embedded_geometry_readback_policy;
  const bool passed = p::CaptureBytes(0, 0, 0) == 0 &&
      p::CaptureBytes(0, UINT64_MAX, 0) == 4096 &&
      p::CaptureBytes(p::kMemoryBytes - 8, UINT64_MAX, 0) == 8 &&
      p::CaptureBytes(p::kMemoryBytes, 16, 0) == 0 &&
      p::CaptureBytes(UINT32_MAX, UINT64_MAX, 0) == 0 &&
      p::CaptureBytes(4096, 48, 35) == 48 &&
      p::CaptureBytes(4096, 48, 36) == 0 &&
      !p::MayMap(0, 100) && !p::MayMap(100, 99) && p::MayMap(100, 100) && p::MayMap(100, 101) &&
      p::TextureBytes(0x1118A000, 3768320, 7) == 3768320 &&
      p::TextureBytes(0, 8u*1024u*1024u, 0) == 8u*1024u*1024u &&
      p::TextureBytes(0, 8u*1024u*1024u+1, 0) == 0 &&
      p::TextureBytes(0, 0, 0) == 0 && p::TextureBytes(0, 16, 8) == 0 &&
      p::TextureBytes(p::kMemoryBytes-8, 16, 0) == 0 &&
      p::TextureBytes(p::kMemoryBytes, 1, 0) == 0 &&
      p::TextureBytes(0, UINT64_MAX, 0) == 0;
  const uint8_t known_bytes[] = {'h', 'e', 'l', 'l', 'o'};
  const bool hashes = p::ContentHash(nullptr, 0) == 2166136261u &&
      p::ContentHash(known_bytes, 5) == 0x4F9F2CABu;
  if (!passed || !hashes) std::cerr << "geometry observer bounds/fence/hash regression\n";
  return passed && hashes ? 0 : 1;
}
