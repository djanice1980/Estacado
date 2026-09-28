#include <rex/graphics/resolve_rectangle.h>

#include <iostream>

namespace {

bool CheckRectangle(const int32_t vertices_fixed[6],
                    bool vertex_window_offset_enable, int32_t window_offset_x,
                    int32_t window_offset_y, int32_t scissor_left,
                    int32_t scissor_top, int32_t scissor_right,
                    int32_t scissor_bottom, int32_t expected_x0,
                    int32_t expected_y0, int32_t expected_x1,
                    int32_t expected_y1, bool expected_empty,
                    const char* message) {
  const auto rectangle =
      rex::graphics::draw_util::GetResolveRectangleFromVertices(
          vertices_fixed, vertex_window_offset_enable, window_offset_x,
          window_offset_y, scissor_left, scissor_top, scissor_right,
          scissor_bottom);
  if (rectangle.x0 == expected_x0 && rectangle.y0 == expected_y0 &&
      rectangle.x1 == expected_x1 && rectangle.y1 == expected_y1 &&
      rectangle.empty() == expected_empty) {
    return true;
  }
  std::cerr << message << ": got (" << rectangle.x0 << "," << rectangle.y0
            << ")-(" << rectangle.x1 << "," << rectangle.y1 << ")\n";
  return false;
}

constexpr int32_t FixedAfterD3DHalfPixel(int32_t pixel) {
  return pixel * 256 + 128;
}

}  // namespace

int main() {
  bool passed = true;

  // Preserved front-end evidence: the title submits the same 256x256 triangle
  // against successive 256-row EDRAM bands. With vertex window offset enabled,
  // the second-band copy is deliberately clipped rather than copied from the
  // wrong band. The known-good front end used this hardware behavior; the
  // local-only regression turned the copy into a non-empty write.
  const int32_t band_256_vertices[6] = {
      FixedAfterD3DHalfPixel(0),   FixedAfterD3DHalfPixel(0),
      FixedAfterD3DHalfPixel(256), FixedAfterD3DHalfPixel(0),
      FixedAfterD3DHalfPixel(256), FixedAfterD3DHalfPixel(256)};
  passed &= CheckRectangle(band_256_vertices, true, 0, -256, 0, 0, 1280,
                           256, 0, 0, 256, 0, true,
                           "offset second-band resolve");

  // The first EDRAM band has no offset and remains a normal 256x256 copy.
  passed &= CheckRectangle(band_256_vertices, true, 0, 0, 0, 0, 1280, 256,
                           0, 0, 256, 256, false,
                           "zero-offset first-band resolve");

  // A captured front-end rectangle in the (384,512)-(1280,720) window band is
  // shifted by (-384,-512) before clipping. It is vertically outside the
  // normalized bottom-band scissor and must remain empty.
  const int32_t partial_vertices[6] = {
      FixedAfterD3DHalfPixel(320), FixedAfterD3DHalfPixel(0),
      FixedAfterD3DHalfPixel(640), FixedAfterD3DHalfPixel(0),
      FixedAfterD3DHalfPixel(640), FixedAfterD3DHalfPixel(184)};
  passed &= CheckRectangle(partial_vertices, true, -384, -512, 0, 0, 896,
                           208, 0, 0, 256, 0, true,
                           "offset partial front-end resolve");

  // When the guest explicitly disables the vertex window offset, the exact
  // same CPU vertices are render-target-local and are clipped only by the
  // normalized scissor.
  passed &= CheckRectangle(partial_vertices, false, -384, -512, 0, 0, 1280,
                           336, 320, 0, 640, 184, false,
                           "offset-disabled local resolve");

  if (passed) {
    std::cout << "GPU resolve window-offset tests passed\n";
  }
  return passed ? 0 : 1;
}
