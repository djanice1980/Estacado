// Temporal AA policy (rex/graphics/temporal_aa_policy.h): the camera of a
// rendered frame from the world draws' vertex constants, the majority vote,
// the reprojection matrix, cut detection and the jitter sequence. The camera
// constants below are the street frames 13831/13832 of the V396 measurement
// capture (a steady mouse turn, yaw -0.264 degrees between them).
#include <rex/graphics/temporal_aa_policy.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
const uint32_t kFrameA[32] = {
    0xBE410CB7u, 0xBF86F63Bu, 0xAF3563DEu, 0x00000000u, 0x3DAEC190u, 0xBC79F8B3u, 0x3FF37C0Cu,
    0x00000000u, 0x3F7BC3D0u, 0xBE341000u, 0xBD3A79DCu, 0xBFE66BB6u, 0x3F7BBE03u, 0xBE340BDAu,
    0xBD3A7590u, 0x00000000u, 0xBE343BAFu, 0xBF7C00E5u, 0xAF295900u, 0xC33F4760u, 0xBD378C60u,
    0x3C034622u, 0xBF7FBC10u, 0xC10FC622u, 0x3F7BBE03u, 0xBE340BDAu, 0xBD3A7590u, 0x44A16214u,
    0x44A2F52Au, 0xC21B2173u, 0xC2472EBEu, 0x00000000u};
const uint32_t kFrameB[32] = {
    0xBE46042Du, 0xBF86D96Fu, 0x2FED4EB4u, 0x00000000u, 0x3DAE9C48u, 0xBC803387u, 0x3FF37C0Cu,
    0x00000000u, 0x3F7B8E19u, 0xBE38B1ECu, 0xBD3A79DCu, 0xBFE66BB6u, 0x3F7B884Du, 0xBE38ADABu,
    0xBD3A7590u, 0x00000000u, 0xBE38DEBBu, 0xBF7BCB20u, 0x2FDD8D7Au, 0xC3453670u, 0xBD376537u,
    0x3C06A6BAu, 0xBF7FBC10u, 0xC10F2DFFu, 0x3F7B884Du, 0xBE38ADABu, 0xBD3A7590u, 0x44A1469Au,
    0x44A2F63Fu, 0xC21B291Bu, 0xC2472CB6u, 0x00000000u};

bool Check(bool condition, const std::string& message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

// Project a world point with a camera to NDC (x, y, z) - the game's own math.
bool Project(const rex::graphics::temporal_aa::Camera& camera, const double world[3],
             double ndc[3]) {
  double rel[4] = {world[0] - camera.position[0], world[1] - camera.position[1],
                   world[2] - camera.position[2], 1.0};
  double clip[4] = {};
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) clip[r] += camera.m[r * 4 + k] * rel[k];
  if (clip[3] <= 0.0) return false;
  for (int i = 0; i < 3; ++i) ndc[i] = clip[i] / clip[3];
  return true;
}
}  // namespace

int main() {
  using namespace rex::graphics::temporal_aa;
  bool ok = true;

  const Camera a = CameraFromConstants(kFrameA);
  const Camera b = CameraFromConstants(kFrameB);
  ok &= Check(a.valid && b.valid, "the captured world constant sets have the camera layout");
  // c7 = -C: C = (-1303.66, 38.78, 49.80) on the street (c6.w holds the view
  // translation, not the position).
  ok &= Check(std::fabs(a.position[0] + 1303.66) < 0.05 && std::fabs(a.position[1] - 38.78) < 0.05 &&
                  std::fabs(a.position[2] - 49.80) < 0.05,
              "camera position is -c7");
  // |c0.xy| = 1/tan(86.06/2) at the default field of view.
  ok &= Check(std::fabs(std::hypot(a.m[0], a.m[1]) - 1.0711) < 0.001, "x scale = default FOV");
  {
    uint32_t bad[32];
    std::copy(std::begin(kFrameA), std::end(kFrameA), bad);
    bad[15] = 0x3F800000u;  // c3.w = 1: not the camera layout
    ok &= Check(!CameraFromConstants(bad).valid, "a set without c3.w = 0 is rejected");
    bad[15] = 0;
    bad[0] = 0x7FC00000u;  // NaN
    ok &= Check(!CameraFromConstants(bad).valid, "non-finite constants are rejected");
  }

  // Vote: world draws win over a minority of object draws.
  {
    uint32_t object[32];
    std::copy(std::begin(kFrameB), std::end(kFrameB), object);
    object[28] = 0x44000000u;  // another c7: a model transform folded in
    CameraVote vote;
    for (int i = 0; i < 50; ++i) vote.Add(kFrameA);
    for (int i = 0; i < 14; ++i) vote.Add(object);
    vote.Add(object);  // beyond kVoteDraws: ignored
    const Camera c = vote.Result();
    ok &= Check(vote.draws == kVoteDraws && c.valid && c.position == a.position,
                "the world constant set wins the vote");
    CameraVote minority;
    for (int i = 0; i < 20; ++i) minority.Add(kFrameA);
    for (int i = 0; i < 30; ++i) minority.Add(object);
    ok &= Check(minority.Result().position[0] != a.position[0],
                "a camera without a majority is not the world set");
    CameraVote spread;
    uint32_t distinct[32];
    std::copy(std::begin(kFrameA), std::end(kFrameA), distinct);
    for (uint32_t i = 0; i < 40; ++i) {
      distinct[28] = 0x44000000u + i;
      spread.Add(distinct);
    }
    ok &= Check(!spread.Result().valid, "no majority, no camera");
  }

  // Reprojection: identical cameras map every point to itself.
  {
    std::array<double, 16> r;
    ok &= Check(ReprojectionMatrix(a, a, r), "reprojection matrix for a still camera");
    const double p[4] = {0.3, -0.2, 0.998, 1.0};
    double q[4] = {};
    for (int i = 0; i < 4; ++i)
      for (int k = 0; k < 4; ++k) q[i] += r[i * 4 + k] * p[k];
    ok &= Check(std::fabs(q[0] / q[3] - 0.3) < 1e-9 && std::fabs(q[1] / q[3] + 0.2) < 1e-9,
                "a still camera reprojects to the same NDC");
  }
  // Reprojection across the turn equals projecting the world point with the
  // previous camera.
  {
    std::array<double, 16> r;
    ok &= Check(ReprojectionMatrix(b, a, r), "reprojection matrix across the turn");
    double worst = 0.0, mean_dx = 0.0;
    int n = 0;
    // Points in front of the camera (it looks along +x).
    for (double wx = -1200.0; wx <= -600.0; wx += 100.0)
      for (double wy = -150.0; wy <= 250.0; wy += 50.0)
        for (double wz = 0.0; wz <= 150.0; wz += 50.0) {
          const double world[3] = {wx, wy, wz};
          double now[3], before[3];
          if (!Project(b, world, now) || !Project(a, world, before)) continue;
          if (std::fabs(now[0]) > 1 || std::fabs(now[1]) > 1) continue;
          const double p[4] = {now[0], now[1], now[2], 1.0};
          double q[4] = {};
          for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 4; ++k) q[i] += r[i * 4 + k] * p[k];
          const double ex = q[0] / q[3] - before[0], ey = q[1] / q[3] - before[1];
          worst = std::fmax(worst, std::hypot(ex, ey) * 640.0);
          mean_dx += (before[0] - now[0]) * 640.0;
          ++n;
        }
    ok &= Check(n > 20, "enough visible test points");
    ok &= Check(worst < 0.01, "reprojection matches the previous camera within 0.01 px");
    ok &= Check(n && std::fabs(mean_dx / n) > 1.0 && std::fabs(mean_dx / n) < 10.0,
                "the turn moves points a few pixels");
  }

  // Cuts.
  {
    ok &= Check(!IsCut(b, a), "a turning frame pair is not a cut");
    Camera moved = a;
    moved.position[0] += 100.0;
    ok &= Check(IsCut(moved, a), "a 100-unit jump is a cut");
    Camera turned = a;
    const double angle = 30.0 * 3.14159265358979 / 180.0;
    const double x = turned.m[12], y = turned.m[13];
    turned.m[12] = x * std::cos(angle) - y * std::sin(angle);
    turned.m[13] = x * std::sin(angle) + y * std::cos(angle);
    ok &= Check(IsCut(turned, a), "a 30-degree turn in one frame is a cut");
    ok &= Check(IsCut(Camera{}, a), "no camera, no history");
  }

  // Jitter: distinct phases inside [-0.5, 0.5), mean near the pixel center.
  {
    double sx = 0, sy = 0;
    bool inside = true, distinct = true;
    double px[kJitterPhases], py[kJitterPhases];
    for (uint32_t i = 0; i < kJitterPhases; ++i) {
      Jitter(i, px[i], py[i]);
      inside &= px[i] >= -0.5 && px[i] < 0.5 && py[i] >= -0.5 && py[i] < 0.5;
      sx += px[i];
      sy += py[i];
      for (uint32_t j = 0; j < i; ++j) distinct &= px[i] != px[j] || py[i] != py[j];
    }
    double rx, ry;
    Jitter(kJitterPhases, rx, ry);
    ok &= Check(inside && distinct, "jitter phases are distinct and sub-pixel");
    ok &= Check(std::fabs(sx / kJitterPhases) < 0.1 && std::fabs(sy / kJitterPhases) < 0.1,
                "jitter averages near the pixel center");
    ok &= Check(rx == px[0] && ry == py[0], "jitter repeats every kJitterPhases frames");
  }

  if (ok) std::cout << "gpu_temporal_aa_policy: ok\n";
  return ok ? 0 : 1;
}
