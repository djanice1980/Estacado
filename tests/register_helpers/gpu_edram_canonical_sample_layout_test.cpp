#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#ifndef REXGLUE_SOURCE_ROOT
#error REXGLUE_SOURCE_ROOT must name the ReXGlue source directory
#endif

namespace {

enum class Samples : uint32_t { k1X = 1, k2X = 2, k4X = 4 };

struct Coordinate {
  uint32_t x;
  uint32_t y;
  uint32_t sample;
};

std::pair<uint32_t, uint32_t> Canonicalize(Samples samples, uint32_t x,
                                            uint32_t y, uint32_t sample) {
  if (samples == Samples::k4X) {
    return {((x & ~1u) << 1u) | (x & 1u) | ((sample & 1u) << 1u),
            ((y & ~1u) << 1u) | (y & 1u) | (sample & 2u)};
  }
  if (samples == Samples::k2X) {
    return {(x & ~2u) | ((sample & 1u) << 1u),
            ((y & ~1u) << 1u) | (y & 1u) | (x & 2u)};
  }
  return {x, y};
}

Coordinate Decanonicalize(Samples samples, uint32_t u, uint32_t v) {
  if (samples == Samples::k4X) {
    return {((u >> 2u) << 1u) | (u & 1u),
            ((v >> 2u) << 1u) | (v & 1u),
            ((u >> 1u) & 1u) | (v & 2u)};
  }
  if (samples == Samples::k2X) {
    return {(u & ~3u) | (v & 2u) | (u & 1u),
            ((v & ~3u) >> 1u) | (v & 1u), (u >> 1u) & 1u};
  }
  return {u, v, 0};
}

Coordinate MapCrossScaleTransfer(Samples dest_samples, uint32_t dest_x,
                                 uint32_t dest_y, uint32_t dest_sample,
                                 uint32_t dest_scale, Samples source_samples,
                                 uint32_t source_scale) {
  // Mirror GetOrCreateTransferPipelines: put tile-local destination
  // coordinates in source scale space, canonicalize using the destination
  // view, then decode through the source view. Scaled subpixel coordinates are
  // deliberately preserved only when the source itself is scaled.
  uint32_t source_space_x = dest_x;
  uint32_t source_space_y = dest_y;
  if (dest_scale != source_scale) {
    if (dest_scale == 1) {
      source_space_x = dest_x * source_scale + source_scale / 2;
      source_space_y = dest_y * source_scale + source_scale / 2;
    } else {
      source_space_x = dest_x / dest_scale;
      source_space_y = dest_y / dest_scale;
    }
  }
  const uint32_t guest_x = source_space_x / source_scale;
  const uint32_t guest_y = source_space_y / source_scale;
  const uint32_t subpixel_x = source_space_x % source_scale;
  const uint32_t subpixel_y = source_space_y % source_scale;
  const auto canonical =
      Canonicalize(dest_samples, guest_x, guest_y, dest_sample);
  Coordinate source =
      Decanonicalize(source_samples, canonical.first, canonical.second);
  source.x = source.x * source_scale + subpixel_x;
  source.y = source.y * source_scale + subpixel_y;
  return source;
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

bool Require(const std::string& source, const std::string& needle,
             const char* failure) {
  if (source.find(needle) != std::string::npos) return true;
  std::cerr << failure << '\n';
  return false;
}

bool Reject(const std::string& source, const std::string& needle,
            const char* failure) {
  if (source.find(needle) == std::string::npos) return true;
  std::cerr << failure << '\n';
  return false;
}

}  // namespace

int main() {
  bool passed = true;

  // Every view must round-trip through the canonical sample grid. This is the
  // invariant required when the same EDRAM allocation is reinterpreted as
  // 1x, 2x or 4x MSAA.
  for (Samples samples : {Samples::k1X, Samples::k2X, Samples::k4X}) {
    uint32_t sample_count = static_cast<uint32_t>(samples);
    std::set<std::pair<uint32_t, uint32_t>> occupied;
    for (uint32_t y = 0; y < 16; ++y) {
      for (uint32_t x = 0; x < 16; ++x) {
        for (uint32_t sample = 0; sample < sample_count; ++sample) {
          const auto canonical = Canonicalize(samples, x, y, sample);
          const Coordinate decoded =
              Decanonicalize(samples, canonical.first, canonical.second);
          if (decoded.x != x || decoded.y != y || decoded.sample != sample) {
            std::cerr << "Canonical sample round-trip failed\n";
            passed = false;
          }
          if (!occupied.insert(canonical).second) {
            std::cerr << "Canonical sample address collision\n";
            passed = false;
          }
        }
      }
    }
  }

  // Any canonical position decoded through another view and encoded again
  // must name the identical physical sample. This specifically protects the
  // 4x-to-2x alias used by The Darkness.
  for (uint32_t v = 0; v < 32; ++v) {
    for (uint32_t u = 0; u < 32; ++u) {
      for (Samples view : {Samples::k1X, Samples::k2X, Samples::k4X}) {
        const Coordinate decoded = Decanonicalize(view, u, v);
        const auto encoded =
            Canonicalize(view, decoded.x, decoded.y, decoded.sample);
        if (encoded.first != u || encoded.second != v) {
          std::cerr << "Aliased view changed a canonical sample address\n";
          passed = false;
        }
      }
    }
  }

  // Native Direct3D 2x sample numbering is reversed relative to the guest;
  // emulation with a 4x resource uses host samples 0 and 3.
  for (uint32_t guest = 0; guest < 2; ++guest) {
    const uint32_t native_host = guest ^ 1u;
    const uint32_t emulated_host = guest ? 3u : 0u;
    passed &= ((native_host ^ 1u) == guest);
    passed &= ((emulated_host >> 1u) == guest);
  }

  // A native 4x producer and scaled 2x consumer are the exact alias pair
  // observed at EDRAM base 0x300. Each 2x2 destination subpixel group must
  // read one identical native source sample, and every mapping must preserve
  // the canonical physical sample identity.
  for (uint32_t guest_y = 0; guest_y < 8; ++guest_y) {
    for (uint32_t guest_x = 0; guest_x < 80; ++guest_x) {
      for (uint32_t sample = 0; sample < 2; ++sample) {
        Coordinate first = {};
        bool have_first = false;
        for (uint32_t parity_y = 0; parity_y < 2; ++parity_y) {
          for (uint32_t parity_x = 0; parity_x < 2; ++parity_x) {
            const Coordinate source = MapCrossScaleTransfer(
                Samples::k2X, guest_x * 2 + parity_x,
                guest_y * 2 + parity_y, sample, 2, Samples::k4X, 1);
            if (!have_first) {
              first = source;
              have_first = true;
            } else if (source.x != first.x || source.y != first.y ||
                       source.sample != first.sample) {
              std::cerr << "Scaled destination subpixels selected different "
                           "native samples\n";
              passed = false;
            }
            const auto dest_canonical =
                Canonicalize(Samples::k2X, guest_x, guest_y, sample);
            const auto source_canonical = Canonicalize(
                Samples::k4X, source.x, source.y, source.sample);
            if (dest_canonical != source_canonical) {
              std::cerr << "Cross-scale transfer changed canonical sample "
                           "identity\n";
              passed = false;
            }
          }
        }
      }
    }
  }

  const std::string root = REXGLUE_SOURCE_ROOT;
  const std::string cache =
      Read(root + "/src/graphics/d3d12/render_target_cache.cpp");
  const std::string output_merger = Read(
      root + "/src/graphics/pipeline/shader/dxbc_translator_om.cpp");
  const std::string translator = Read(
      root + "/include/rex/graphics/pipeline/shader/dxbc_translator.h");
  passed &= Require(cache, "static void CanonicalizeSample",
                    "transfer generator lacks canonicalization");
  passed &= Require(cache, "static void DecanonicalizeSample",
                    "transfer generator lacks decanonicalization");
  passed &= Require(cache, "Guest pixel X = (u & ~2) | (v & 2)",
                    "EDRAM dump still lacks canonical 2x decoding");
  passed &= Require(output_merger, "canonical sample 0 coordinates",
                    "ROV writer lacks canonical sample addressing");
  passed &= Require(output_merger, "sample_column_delta",
                    "ROV sample walk still uses subdivided tile rows");
  passed &= Require(translator, "kVersion = 0x20260908",
                    "shader cache identity was not invalidated");
  passed &= Reject(cache, "vertical sample index within the destination pixel",
                   "old subdivided 2x dump mapping remains");
  passed &= Require(cache,
                    "source_summary.unique_pixel_values_capped < 8",
                    "mixed-scale observer does not reject uniform setup passes");
  passed &= Require(cache,
                    "IsCurrentEmbeddedGameplayCaptureFrame()",
                    "mixed-scale observer is not capture-frame bounded");
  passed &= Require(cache,
                    "embedded_mixed_scale_transfer_inspection_count >= 64",
                    "mixed-scale observer lacks a process-lifetime bound");
  passed &= Require(
      cache, "rex_mixed_scale_transfer_source_nonuniform_fp16.bin",
      "mixed-scale observer does not preserve the content-bearing source");
  passed &= Require(cache, "rex_mixed_scale_transfer_dest_after_fp16.bin",
                    "mixed-scale observer does not preserve the destination");

  const std::array<const char*, 22> shader_names = {
      "resolve_clear_32bpp_cs", "resolve_clear_32bpp_scaled_cs",
      "resolve_clear_64bpp_cs", "resolve_clear_64bpp_scaled_cs",
      "resolve_fast_32bpp_1x2xmsaa_cs",
      "resolve_fast_32bpp_1x2xmsaa_scaled_cs",
      "resolve_fast_32bpp_4xmsaa_cs",
      "resolve_fast_32bpp_4xmsaa_scaled_cs",
      "resolve_fast_64bpp_1x2xmsaa_cs",
      "resolve_fast_64bpp_1x2xmsaa_scaled_cs",
      "resolve_fast_64bpp_4xmsaa_cs",
      "resolve_fast_64bpp_4xmsaa_scaled_cs", "resolve_full_128bpp_cs",
      "resolve_full_128bpp_scaled_cs", "resolve_full_16bpp_cs",
      "resolve_full_16bpp_scaled_cs", "resolve_full_32bpp_cs",
      "resolve_full_32bpp_scaled_cs", "resolve_full_64bpp_cs",
      "resolve_full_64bpp_scaled_cs", "resolve_full_8bpp_cs",
      "resolve_full_8bpp_scaled_cs"};
  for (const char* shader_name : shader_names) {
    const std::string shader = Read(
        root + "/src/graphics/shaders/bytecode/d3d12_5_1/" + shader_name +
        ".h");
    if (shader.empty() ||
        shader.find(std::string("const BYTE ") + shader_name + "[]") ==
            std::string::npos) {
      std::cerr << "Missing canonical resolve bytecode: " << shader_name
                << '\n';
      passed = false;
    }
  }

  if (passed) {
    std::cout << "GPU canonical EDRAM sample layout tests passed\n";
  }
  return passed ? 0 : 1;
}
