#include <array>
#include <cstdint>
#include <iostream>

#include <rex/graphics/embedded_resolve_boundary_capture_policy.h>

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using rex::graphics::embedded_resolve_boundary_capture_policy::
      IsFirstSceneFeedbackResolve;
  using rex::graphics::embedded_resolve_boundary_capture_policy::
      ResolveRegisters;

  constexpr ResolveRegisters expected = {
      0x00100040, 0x000C0300, 0x003C0D01, 0x02D00500, 0x14010500};
  if (!IsFirstSceneFeedbackResolve(expected)) {
    return Fail("the verified scene-feedback resolve signature must match");
  }

  // Every register is part of the semantic identity. No guest destination
  // address or prior-resolve ordinal is accepted as a substitute.
  constexpr std::array<uint32_t, 5> mismatches = {
      0x00100041, 0x000C0301, 0x003C0D00, 0x02D00501, 0x14010501};
  for (size_t field = 0; field < mismatches.size(); ++field) {
    ResolveRegisters changed = expected;
    reinterpret_cast<uint32_t*>(&changed)[field] = mismatches[field];
    if (IsFirstSceneFeedbackResolve(changed)) {
      return Fail("a mismatching resolve register must reject the capture");
    }
  }

  std::cout << "GPU resolve boundary capture policy regression passed\n";
  return 0;
}
