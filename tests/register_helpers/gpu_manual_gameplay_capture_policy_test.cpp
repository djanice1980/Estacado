#include <iostream>

#include <rex/graphics/embedded_manual_capture_policy.h>
#include <rex/graphics/embedded_texture_readback_policy.h>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  using rex::graphics::embedded_manual_capture_policy::Action;
  using rex::graphics::embedded_manual_capture_policy::Advance;
  using rex::graphics::embedded_manual_capture_policy::IsCompleteFrameArmed;
  using rex::graphics::embedded_manual_capture_policy::IsPending;
  using rex::graphics::embedded_manual_capture_policy::State;

  bool passed = true;
  State state;
  uint64_t capture_generation = 99;
  passed &= Check(!IsPending(0, state), "idle state must not collect frames");
  passed &= Check(!IsCompleteFrameArmed(state),
                  "idle state must not be a complete capture frame");
  passed &= Check(Advance(0, state, &capture_generation) == Action::kNone &&
                      capture_generation == 0,
                  "idle state must not arm or capture");

  passed &= Check(IsPending(1, state), "fresh request must enable collection");
  passed &= Check(!IsCompleteFrameArmed(state),
                  "a mid-frame request must not capture a partial frame");
  passed &= Check(Advance(1, state, &capture_generation) == Action::kArm &&
                      state.armed_generation == 1 && capture_generation == 0,
                  "first swap must only arm and discard a partial frame");
  passed &= Check(IsCompleteFrameArmed(state),
                  "the frame after the arm-only swap must be captured in full");
  passed &= Check(Advance(1, state, &capture_generation) == Action::kCapture &&
                      capture_generation == 1 &&
                      state.consumed_generation == 1 &&
                      !IsPending(1, state),
                  "second swap must capture exactly one complete frame");
  passed &= Check(!IsCompleteFrameArmed(state),
                  "consuming a capture must close the detailed-frame gate");
  passed &= Check(Advance(1, state, &capture_generation) == Action::kNone,
                  "one request must not produce repeated captures");

  passed &= Check(Advance(2, state, &capture_generation) == Action::kArm,
                  "later physical requests must be independently armed");
  passed &= Check(Advance(3, state, &capture_generation) == Action::kCapture &&
                      capture_generation == 2 && IsPending(3, state),
                  "a queued request must not replace an already armed capture");
  passed &= Check(Advance(3, state, &capture_generation) == Action::kArm &&
                      Advance(3, state, &capture_generation) == Action::kCapture &&
                      capture_generation == 3 && !IsPending(3, state),
                  "queued requests must complete on separate full frames");

  using rex::graphics::embedded_texture_readback_policy::ReserveResult;
  using rex::graphics::embedded_texture_readback_policy::TryReserve;
  rex::graphics::embedded_texture_readback_policy::State readback_state;
  passed &= Check(TryReserve(readback_state, 0x1234, 0, 4096) ==
                      ReserveResult::kAccepted &&
                      readback_state.key_count == 1 &&
                      readback_state.reserved_bytes == 4096,
                  "first texture subresource must reserve bounded storage");
  passed &= Check(TryReserve(readback_state, 0x1234, 0, 4096) ==
                      ReserveResult::kDuplicate &&
                      readback_state.key_count == 1 &&
                      readback_state.reserved_bytes == 4096,
                  "repeated shader bindings must not duplicate readbacks");
  passed &= Check(TryReserve(readback_state, 0x1234, 1, 8192) ==
                      ReserveResult::kAccepted &&
                      readback_state.key_count == 2 &&
                      readback_state.reserved_bytes == 12288,
                  "different array slices must retain independent evidence");
  passed &= Check(
      TryReserve(
          readback_state, 0x5678, 0,
          rex::graphics::embedded_texture_readback_policy::kMaximumBytes) ==
          ReserveResult::kByteLimit,
      "texture capture must enforce its aggregate byte budget");
  passed &= Check(TryReserve(readback_state, 0, 0, 4096) ==
                      ReserveResult::kInvalid,
                  "invalid resource identities must never be captured");
  passed &= Check(TryReserve(readback_state, 0x1234, 0, 4096, 327) ==
                      ReserveResult::kAccepted &&
                      TryReserve(readback_state, 0x1234, 0, 4096, 327) ==
                          ReserveResult::kDuplicate &&
                      TryReserve(readback_state, 0x1234, 0, 4096, 328) ==
                          ReserveResult::kAccepted,
                  "explicit rewrite epochs must be distinct but deduplicate within a draw");
  passed &= Check(TryReserve(
                      readback_state, 0x1234, 0,
                      rex::graphics::embedded_texture_readback_policy::kMaximumBytes,
                      329) == ReserveResult::kByteLimit,
                  "rewrite epochs must not bypass the aggregate byte budget");
  while (readback_state.key_count <
         rex::graphics::embedded_texture_readback_policy::kMaximumSubresources) {
    passed &= Check(TryReserve(readback_state, 0x1234, 0, 1,
                               1000 + readback_state.key_count) == ReserveResult::kAccepted,
                    "bounded distinct rewrite epochs must be accepted");
  }
  passed &= Check(TryReserve(readback_state, 0x1234, 0, 1, 9999) ==
                      ReserveResult::kSubresourceLimit,
                  "rewrite epochs must not bypass the snapshot count budget");

  if (passed) {
    std::cout << "Manual gameplay capture lifecycle passed\n";
  }
  return passed ? 0 : 1;
}
