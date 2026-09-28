#include <rex/graphics/pc_ring_publication.h>
#include <rex/graphics/pc_command_execution.h>
#include <iostream>

using namespace rex::graphics::pc_ring_publication;
int main() {
  bool ok = true;
  State state;
  ok &= !state.Enabled() && !state.Publish(3) && !state.Consume(0,1);
  for (uint32_t size : {0u,1u,3u,12u}) { state.Reset(size); ok &= !state.Enabled(); }
  state.Reset(16);
  auto first = state.Publish(3), second = state.Publish(8);
  ok &= first && second > first && !state.Publish(8);
  // Two publications coalesce into one native execute batch. Each packet keeps
  // its original ticket, not the latest producer's ticket.
  ok &= state.Consume(0,3) == first;
  ok &= state.Consume(3,2) == second && state.Consume(5,3) == second;
  auto wrapped = state.Publish(2);
  ok &= wrapped > second && state.Consume(8,6) == wrapped && state.Consume(14,4) == wrapped;
  // Partial packets spanning publication boundaries cannot claim either owner.
  auto split_a = state.Publish(4), split_b = state.Publish(8);
  ok &= split_a && split_b && !state.Consume(2,4) && state.Enabled();
  ok &= state.Consume(6,2) == split_b;
  // Reinitialization cannot recycle an old ticket.
  state.Reset(16);
  auto reinitialized = state.Publish(3);
  ok &= reinitialized > split_b && state.Consume(0,3) == reinitialized;
  state.Publish(6);
  ok &= !state.Consume(4,2) && !state.Enabled(); // Wrong native read position.
  state.Reset(16); state.Publish(3);
  ok &= !state.Consume(0,4) && !state.Enabled(); // Not published yet.
  state.Reset(16); state.Publish(12);
  ok &= !state.Publish(8) && !state.Enabled(); // Overruns outstanding words.
  state.Reset(16);
  ok &= !state.Publish(16) && !state.Enabled();
  state.Reset(4096);
  for (uint32_t i=1;i<=State::kCapacity;++i) ok &= state.Publish(i)!=0;
  ok &= !state.Publish(State::kCapacity+1) && !state.Enabled();
  // Capacity can be reused after consumption without reusing ticket identities.
  state.Reset(4096);
  uint64_t previous=0;
  for (uint32_t i=0;i<5000;++i) {
    auto ticket=state.Publish((i+1)&4095);
    ok &= ticket>previous && state.Consume(i&4095,1)==ticket;
    previous=ticket;
  }
  ok &= PacketWords(0)==1 && PacketWords(0x80000000)==1;
  ok &= PacketWords(0x40000000)==3 && PacketWords(0x4000)==2;
  ok &= PacketWords(0xC0013F00)==3 && PacketWords(0x3FFF4000)==16385;
  rex::graphics::pc_command_execution::State execution;
  {
    rex::graphics::pc_command_execution::State::Scope root(execution);
    execution.BeginPacket(); execution.SetRootPublication(first);
    {
      rex::graphics::pc_command_execution::State::Scope child(execution);
      execution.BeginPacket();
      ok &= execution.Current().root_publication==first;
    }
    execution.BeginPacket(); execution.SetRootPublication(second);
    ok &= execution.Current().root_publication==second;
  }
  ok &= execution.Current().root_publication==0;
  if (!ok) std::cerr << "Root ring publication ownership failed\n";
  return ok?0:1;
}
