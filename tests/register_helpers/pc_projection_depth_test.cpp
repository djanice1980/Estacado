#include <rex/graphics/pc_draw_transform_history.h>
#include <iostream>
#include <limits>

using namespace rex::graphics;

int main() {
  bool ok = true;
  pc_sparse_projection::Projection p;
  p.valid = true;
  p.words[0] = 0x3F800000; p.words[5] = 0xBF800000;
  p.words[10] = 0x40000000; p.words[11] = 0x3F800000;
  p.words[14] = 0xC0000000;
  const std::array<uint32_t,6> viewport{
      0x44200000,0x44200000,0xC3B40000,0x43B40000,0xBF800000,0x3F800000};
  const auto derive = [&](const pc_sparse_projection::Projection& source) {
    return pc_projection_depth::Derive(source,viewport,0x43F,0x80000);
  };
  auto parameters = derive(p);
  ok &= parameters.valid;
  // Independently project homogeneous positions (input W need not be one),
  // apply the native viewport, then recover Z/W rather than claiming Z.
  for (double w : {0.5,1.0,4.0}) {
    for (double q : {1.125,1.25,1.5,1.875}) {
      const double z = q*w;
      const double clip_z = 2*z - 2*w, clip_w = z;
      const double d = 1 - clip_z/clip_w;
      double recovered = -1;
      ok &= parameters.ProjectionCoordinate(d,recovered) && std::abs(recovered-q)<1e-14;
    }
  }
  for (double bad : {0.,1.,-1.,2.,std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity()}) {
    double untouched = -37;
    ok &= !parameters.ProjectionCoordinate(bad,untouched) && untouched == -37;
  }
  auto changed = p; changed.valid = false; ok &= !derive(changed).valid;
  changed = p; changed.words[1] = 1; ok &= !derive(changed).valid;
  changed = p; changed.words[14] = 0; ok &= !derive(changed).valid;
  changed = p; changed.words[10] = 0x7FC00000; ok &= !derive(changed).valid;
  changed = p; changed.words[11] ^= 1; ok &= !derive(changed).valid;
  for (unsigned i=0;i<6;++i) {
    auto vp=viewport; vp[i]^=1;
    ok &= !pc_projection_depth::Derive(p,vp,0x43F,0x80000).valid;
  }
  ok &= !pc_projection_depth::Derive(p,viewport,0x43E,0x80000).valid;
  ok &= !pc_projection_depth::Derive(p,viewport,0x43F,0).valid;
  pc_draw_transform_history::Frame frame;
  frame.frame=10; frame.count=1; frame.projection=p;
  frame.draws[0].viewport=viewport; frame.draws[0].vte=0x43F; frame.draws[0].clip=0x80000;
  ok &= !frame.ProjectionDepth().valid; // Not sealed: cannot expose partial input.
  frame.Seal(10); ok &= frame.ProjectionDepth().valid;
  frame.failed=true; ok &= !frame.ProjectionDepth().valid;
  frame.failed=false; frame.projection.valid=false; ok &= !frame.ProjectionDepth().valid;
  frame.Reset(); ok &= !frame.ProjectionDepth().valid;
  // Actual captured native projection, independently forward-projected over a
  // broad geometric range. This tests conversion, not captured pixel ownership.
  p.words[0]=0x3F891A29; p.words[5]=0xBFF3BCBB;
  p.words[10]=0x3F8002F3; p.words[14]=0xBFE66BB6;
  parameters=derive(p);
  const double a=pc_sparse_projection::Float(p.words[10]);
  const double b=pc_sparse_projection::Float(p.words[14]);
  for (double z : {2.,10.,100.,1000.,10000.}) {
    const double clip_z=a*z+b;
    const double d=1-clip_z/z;
    double recovered=0;
    ok &= parameters.ProjectionCoordinate(d,recovered) && std::abs(recovered/z-1)<1e-11;
  }
  if (!ok) std::cerr << "Projection depth conversion checks failed\n";
  return ok?0:1;
}
