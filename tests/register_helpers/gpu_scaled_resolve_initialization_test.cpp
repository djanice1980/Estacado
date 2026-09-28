#include <rex/graphics/pipeline/texture/scaled_resolve_util.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {

bool CheckPageRanges() {
  std::array<bool, 8> scaled = {false, true, false, false,
                                true,  false, true,  false};
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  rex::graphics::scaled_resolve_util::CollectUnscaledPageRanges(
      4096 + 37, 5 * 4096 - 74, 8 * 4096,
      [&scaled](uint32_t page) { return scaled[page]; }, ranges);
  const std::vector<std::pair<uint32_t, uint32_t>> expected = {
      {2 * 4096, 2 * 4096}, {5 * 4096, 4096}};
  if (ranges == expected) {
    return true;
  }
  std::cerr << "Scaled resolve missing-page coalescing mismatch\n";
  return false;
}

bool CheckPageLayoutSummary() {
  std::array<bool, 7> scaled = {false, true, true, true,
                                false, true, true};
  // Encoded as bytes-per-block-log2 + 1, zero for an unowned layout.
  std::array<uint8_t, 7> layouts = {0, 3, 4, 3, 0, 0, 3};
  const auto summary =
      rex::graphics::scaled_resolve_util::SummarizePageLayouts(
          0, 6, 2,
          [&scaled](uint32_t page) { return scaled[page]; },
          [&layouts](uint32_t page) { return layouts[page]; });
  if (summary.unscaled_page_count == 2 &&
      summary.matching_page_count == 3 &&
      summary.mismatching_page_count == 2 &&
      summary.first_mismatching_page == 2 &&
      summary.first_mismatching_bytes_per_block_log2 == 3) {
    return true;
  }
  std::cerr << "Scaled resolve page-layout summary mismatch\n";
  return false;
}

uint32_t TiledAddressCombine(uint32_t outer_inner_bytes, uint32_t bank,
                             uint32_t pipe, uint32_t y_lsb) {
  return (y_lsb << 4) | (pipe << 6) | (bank << 11) |
         (outer_inner_bytes & 0xF) |
         (((outer_inner_bytes >> 4) & 1) << 5) |
         (((outer_inner_bytes >> 5) & 7) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

uint32_t GetTiled2DAddress(uint32_t x, uint32_t y,
                           uint32_t pitch_macro_tiles,
                           uint32_t bytes_per_block_log2) {
  const uint32_t outer_blocks =
      (((y >> 5) * pitch_macro_tiles) + (x >> 5)) << 6;
  const uint32_t inner_blocks = (((y >> 1) & 7) << 3) | (x & 7);
  const uint32_t outer_inner_bytes =
      (outer_blocks | inner_blocks) << bytes_per_block_log2;
  const uint32_t bank = (y >> 4) & 1;
  const uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return TiledAddressCombine(outer_inner_bytes, bank, pipe, y & 1);
}

// Independent reference for the Xenia 0f23f056 rectangular-group addressing
// contract embedded in the ReXGlue scaled load and resolve shaders.
uint32_t GetReferenceScaledAddress(uint32_t host_x, uint32_t host_y,
                                   uint32_t pitch_macro_tiles,
                                   uint32_t scale_x, uint32_t scale_y,
                                   uint32_t bytes_per_block_log2) {
  const uint32_t group_x_log2 =
      bytes_per_block_log2 >= 3 ? 5 - bytes_per_block_log2 : 4;
  const uint32_t group_y_log2 =
      3 - std::min(bytes_per_block_log2, uint32_t(2));
  const uint32_t host_group_x = host_x >> group_x_log2;
  const uint32_t host_group_y = host_y >> group_y_log2;
  const uint32_t guest_group_x = host_group_x / scale_x;
  const uint32_t guest_group_y = host_group_y / scale_y;
  const uint32_t guest_group_address = GetTiled2DAddress(
      guest_group_x << group_x_log2, guest_group_y << group_y_log2,
      pitch_macro_tiles, bytes_per_block_log2);
  const uint32_t host_group_in_guest_x =
      host_group_x - guest_group_x * scale_x;
  const uint32_t host_group_in_guest_y =
      host_group_y - guest_group_y * scale_y;
  const uint32_t host_group_index =
      host_group_in_guest_x * scale_y + host_group_in_guest_y;
  const uint32_t group_width_bytes_log2 =
      group_x_log2 + bytes_per_block_log2;
  const uint32_t group_size_log2 =
      group_width_bytes_log2 + group_y_log2;
  const uint32_t local_x = host_x & ((uint32_t(1) << group_x_log2) - 1);
  const uint32_t local_y = host_y & ((uint32_t(1) << group_y_log2) - 1);
  const uint32_t host_byte_in_group =
      (local_y << group_width_bytes_log2) |
      (local_x << bytes_per_block_log2);
  return guest_group_address * (scale_x * scale_y) +
         (host_group_index << group_size_log2) + host_byte_in_group;
}

bool CheckScaledTiledReplication(uint32_t bytes_per_block_log2,
                                 uint32_t scale_x, uint32_t scale_y) {
  const uint32_t bytes_per_block = uint32_t(1) << bytes_per_block_log2;
  constexpr uint32_t kPitchMacroTiles = 4;
  for (uint32_t host_y = 0; host_y < 64 * scale_y; ++host_y) {
    for (uint32_t host_x = 0; host_x < 96 * scale_x; ++host_x) {
      const uint32_t scaled_address = GetReferenceScaledAddress(
          host_x, host_y, kPitchMacroTiles, scale_x, scale_y,
          bytes_per_block_log2);
      // Initialization expands EACH native texel, not a whole rectangular
      // storage group. For example ABCD at2x must become AABBCCDD, not ABCDABCD.
      const uint32_t guest_x = host_x / scale_x;
      const uint32_t guest_y = host_y / scale_y;
      const uint32_t guest_address =
          GetTiled2DAddress(guest_x, guest_y, kPitchMacroTiles,
                            bytes_per_block_log2);
      for (uint32_t byte = 0; byte < bytes_per_block; ++byte) {
        const uint32_t mapped =
            rex::graphics::scaled_resolve_util::
                GetInitializationSourceByteOffset(
                    scaled_address + byte, scale_x, scale_y,
                    bytes_per_block_log2);
        if (mapped != guest_address + byte) {
          std::cerr << "Scaled tiled initialization mismatch for "
                    << bytes_per_block << " B/block at " << scale_x << "x"
                    << scale_y << ": destination " << scaled_address + byte
                    << " mapped to " << mapped << " instead of "
                    << guest_address + byte << "\n";
          return false;
        }
      }
    }
  }

  const uint32_t group_size_log2 =
      rex::graphics::scaled_resolve_util::GetGroupSizeLog2(
          bytes_per_block_log2);
  const uint32_t group_size = uint32_t(1) << group_size_log2;
  const uint32_t scaled_group_size = group_size * scale_x * scale_y;
  // The mapping must advance to the matching guest group, not wrap.
  for (uint32_t byte = 0; byte < scaled_group_size; ++byte) {
    const uint32_t first =
        rex::graphics::scaled_resolve_util::
            GetInitializationSourceByteOffset(
                byte, scale_x, scale_y, bytes_per_block_log2);
    const uint32_t second =
        rex::graphics::scaled_resolve_util::
            GetInitializationSourceByteOffset(
                scaled_group_size + byte, scale_x, scale_y,
                bytes_per_block_log2);
    if (second != first + group_size) {
      std::cerr << "Scaled tiled group progression mismatch\n";
      return false;
    }
  }

  // The group-local row-major/tiled permutations must be exact inverses.
  for (uint32_t byte = 0; byte < group_size; ++byte) {
    const uint32_t tiled =
        rex::graphics::scaled_resolve_util::HostGroupByteToGuestTiledByte(
            byte, bytes_per_block_log2);
    const uint32_t linear =
        rex::graphics::scaled_resolve_util::GuestTiledByteToHostGroupByte(
            tiled, bytes_per_block_log2);
    if (linear != byte) {
      std::cerr << "Scaled tiled group permutation mismatch\n";
      return false;
    }
  }

  // Every complete repeated host group must round-trip to the same guest
  // group, including each byte inside an element.
  for (uint32_t guest_byte = 0; guest_byte < group_size * 19; ++guest_byte) {
    for (uint32_t subunit_x = 0; subunit_x < scale_x; ++subunit_x) {
      for (uint32_t subunit_y = 0; subunit_y < scale_y; ++subunit_y) {
        const uint32_t scaled_byte =
            rex::graphics::scaled_resolve_util::
                GetDownscaledSourceByteOffset(
                    guest_byte, scale_x, scale_y, bytes_per_block_log2,
                    subunit_x, subunit_y);
        const uint32_t round_trip =
            rex::graphics::scaled_resolve_util::
                GetInitializationSourceByteOffset(
                    scaled_byte, scale_x, scale_y, bytes_per_block_log2);
        if (round_trip != guest_byte) {
          std::cerr << "Scaled tiled downscale round-trip mismatch for "
                    << bytes_per_block << " B/block at " << scale_x << "x"
                    << scale_y << ": guest " << guest_byte << " via subunit "
                    << subunit_x << "," << subunit_y << " mapped to "
                    << round_trip << "\n";
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace

int main() {
  bool passed = CheckPageRanges() && CheckPageLayoutSummary();
  for (uint32_t bytes_per_block_log2 = 0; bytes_per_block_log2 <= 4;
       ++bytes_per_block_log2) {
    passed &= CheckScaledTiledReplication(bytes_per_block_log2, 2, 2);
    passed &= CheckScaledTiledReplication(bytes_per_block_log2, 3, 3);
    passed &= CheckScaledTiledReplication(bytes_per_block_log2, 2, 3);
    passed &= CheckScaledTiledReplication(bytes_per_block_log2, 1, 1);
  }
  if (passed) {
    std::cout << "GPU scaled resolve initialization tests passed\n";
  }
  return passed ? 0 : 1;
}
