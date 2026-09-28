#include <rex/graphics/pc_draw_transform_history.h>
#include <iostream>
#include <limits>

using namespace rex::graphics::pc_constant_writer;
int main() {
  bool ok = true;
  const uintptr_t base = 0x100000000ull;
  ok &= PhysicalWord(base,base,512)==0;
  ok &= PhysicalWord(base+508,base,512)==508;
  for (uintptr_t address : {base-4,base+1,base+509,base+512})
    ok &= PhysicalWord(address,base,512)==UINT32_MAX;
  ok &= PhysicalWord(base,base,3)==UINT32_MAX;
  // Subtraction-based range checks must not wrap at the host address limit.
  const auto high=std::numeric_limits<uintptr_t>::max()-3;
  ok &= PhysicalWord(high,high,4)==0;
  ok &= PhysicalWord(0,high,512)==UINT32_MAX;
  Record writer;
  writer.sequence=7; writer.value=0x3F800000;
  writer.physical_address=0x1004; writer.packet_physical=0x1000;
  auto owned=Matching(writer,0x3F800000);
  ok &= owned.sequence==7 && owned.packet_physical==0x1000 && owned.physical_address==0x1004;
  writer.sequence=8; writer.value=0x40000000; // Same address is a different write.
  ok &= owned.sequence==7 && owned.value==0x3F800000;
  ok &= !Matching(writer,0x3F800000).sequence;
  ok &= Matching(writer,0x40000000).sequence==8;
  writer={}; ok &= !Matching(writer,0).sequence;
  // Owned frame reset cannot republish stale metadata in array tails.
  rex::graphics::pc_draw_transform_history::Frame frame;
  frame.frame=1; frame.count=1; frame.sealed=true;
  frame.draws[0].writers[0]=owned;
  ok &= frame.Valid(1);
  frame.Reset(); ok &= !frame.Valid(1) && frame.count==0;
  if (!ok) std::cerr << "Constant writer provenance checks failed\n";
  return ok?0:1;
}
