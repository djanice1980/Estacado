#include <rex/graphics/pc_command_execution.h>
#include <rex/graphics/pc_constant_writer.h>
#include <iostream>

using namespace rex::graphics::pc_command_execution;
static bool Deep(State& state, unsigned depth) {
  State::Scope scope(state);
  state.BeginPacket();
  bool ok = state.Current().Valid() == (depth <= 64);
  if (depth < 67) ok &= Deep(state, depth + 1);
  ok &= state.Current().Valid() == (depth <= 64);
  return ok;
}
int main() {
  bool ok = true;
  State state;
  state.BeginPacket();
  ok &= !state.Current().Valid();
  Token first{}, child{}, second{};
  {
    State::Scope root(state);
    ok &= !state.Current().Valid();
    state.BeginPacket(); first = state.Current();
    ok &= first.Valid() && first.depth == 1 && first.parent == 0;
    {
      State::Scope nested(state);
      state.BeginPacket(); child = state.Current();
      ok &= child.Valid() && child.parent == first.buffer && child.depth == 2;
      ok &= child.parent_packet == first.packet;
      ok &= child.buffer != first.buffer && child.packet != first.packet;
    }
    ok &= state.Current().packet == first.packet;
    state.BeginPacket(); second = state.Current();
    ok &= second.buffer == first.buffer && second.packet != first.packet;
  }
  ok &= !state.Current().Valid();
  // Re-execution cannot reuse a prior token, regardless of guest addresses.
  {
    State::Scope replay(state);
    state.BeginPacket();
    ok &= state.Current().buffer > second.packet;
    rex::graphics::pc_constant_writer::Record writer;
    writer.sequence = 1; writer.value = 42; writer.execution = state.Current();
    auto owned = Matching(writer, 42);
    state.Invalidate();
    ok &= !state.Current().Valid() && owned.execution.Valid();
    ok &= !Matching(writer, 41).execution.Valid();
  }
  ok &= !state.Current().Valid();
  // Invalidation inside nesting must not resurrect the saved outer packet.
  {
    State::Scope root(state); state.BeginPacket();
    { State::Scope nested(state); state.BeginPacket(); state.Invalidate(); }
    ok &= !state.Current().Valid();
  }
  ok &= Deep(state, 1) && !state.Current().Valid();
  if (!ok) std::cerr << "Command execution ownership failed\n";
  return ok ? 0 : 1;
}
