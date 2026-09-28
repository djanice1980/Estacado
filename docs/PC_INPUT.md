# PC input foundation

Status: **BUILT / REGRESSION VERIFIED — consolidated live validation pending**

The validated physical Xbox/XInput path remains the default. With
`[input].keyboard_mouse = false`, runtime capability, state, packet-number, and
vibration calls use the existing direct XInput implementation without entering
the keyboard/mouse merge path.

V60 adds `input.vibration_scale` as a startup-only physical-output control.
The Original value 1.0 passes both guest motor words exactly; values down to
0.0 deterministically scale or disable only `XInputSetState` motor output.
Guest vibration commands, results, controller polling, and title-owned input
shaping are unchanged.

V67 adds startup-only physical digital-button remapping. All 15 defined Xbox
digital guest destinations can source one named physical XInput digital button
or `none`; every preset and missing key use exact identity. The mapping occurs
before guest publication and before optional keyboard/mouse guest-action merge.
Triggers, sticks, packets, connection state, and title analog shaping remain
unchanged.

When explicitly enabled, the ReXGlue-owned game window records real physical
keyboard and mouse events. SDL relative mouse mode supplies high-resolution
motion without desktop-edge clipping or cursor-warp deltas. The runtime merges
buttons, triggers, and the larger-magnitude 16-bit axes with a simultaneously
connected controller. If no controller is attached, the physical
keyboard/mouse source legitimately occupies the configured controller slot.
No guest memory, branch, or controller edge is synthesized.

The separate latency recorder is diagnostics rather than input. Normal
launches leave it off; `--input-diagnostics` enables its bounded transition
log for a deliberate validation run. The consolidated launcher passes that
flag, so controller latency/coexistence evidence is retained while ordinary
users no longer pay for an independent 250 Hz XInput observer.

Current schema-1 bindings cover all Xbox face, shoulder, trigger, stick-click,
D-pad, Start, and Back inputs. Keyboard/mouse is disabled on focus loss, the
relative mode and cursor capture are released, and both may coexist with a
controller. The merge policy and controller-absent contract have focused
coverage in `runtime_input_merge_policy_test`.

The decoded title registers native `lookvelocity_x`, `lookvelocity_y`,
`mousemove`, and directional look actions. Phase 1 used the full 16-bit Xbox
look axes (the right-stick bridge). Mouse-wheel bindings are implemented as
bounded physical pulses.

Native mouse look (V310+, `input.mouse_look = "native"`, the default). The
title's own `look(dx, dy)` (CWClient_Mod slot 0x420, `sub_823FCF68`, the
handler its `mousemove` bind calls) is invoked by the runtime right after the
client's per-frame stick update (`sub_823F8AF0`): raw relative counts become
exact look units (1/65536 revolution; the title applies them 1:1 outside
zoom) at 12 units per count at sensitivity 1.0 (0.066 deg/count), on both
axes, with no stick dead zone, acceleration ramp or turn cap. Sub-unit
remainders carry; commands stay inside int16; one command per frame (max
240/s) so the predicted view turns every frame at 144 Hz, and only while the
client's command ring (client +0xB08, 36-byte entries) keeps half its room -
the title drops every queued command when that ring overflows. The runtime
mirrors the title's input routing: console mode, the debug overlay and a modal
window (pause menu) block it; the gameplay HUD window is non-modal. Live
evidence (V311-V314): exact units per count in both directions and axes,
10 slow counts carried exactly, 1400-count flicks split and exact, pause menu
0 deg, walking 101.4 vs 100.0 u/s while mouse-looking, Darkness summoned
exact, 144.0 images/s with 0 missed refreshes. Title-side: after a slow motion
stops, the simulation can settle 1-2 counts short and applies them with the
next look input (self-correcting, <0.13 deg). `input.mouse_acceleration` and
`input.mouse_smoothing` apply to the stick bridge only.

`input.mouse_look = "stick"` keeps the right-stick bridge. The title reads the
pad from up to three pad-object methods per frame (~2.7 polls per frame);
before V314 the first poll drained the mouse counts, so the look poll often
saw no deflection. V314 computes one deflection per frame and holds it for
every poll in that frame. The bridge still inherits the stick's dead zone and
ramp (slow motion does not turn), which is why native look is the default.

Developer test input (V311+, off by default): `input_test_script` (launcher
`-TestInputScript`) makes the keyboard/mouse driver tail a command file
(`wait`, `down`/`up`/`tap <bind key>`, `mouse <dx> <dy> [ms]`, `mark`, `arm`;
see ReXGlue `test_script_policy.h`, `scripts/test-input.ps1`). It drives the
same driver state as the physical devices without window focus, never
synthesizes system input, and yields (keys released, queue dropped) whenever
local input is newer than 1.5 s until an `arm` line. Tools built on it:
`scripts/street-autopilot-script.py` (spawn to street, exact mouse turns),
`scripts/analyze-mouse-look-sweeps.py`, `scripts/analyze-walk-speed.py`.

Raw-XEX analysis also verifies a separate, title-owned controller shaping path.
The controller initializer at `0x823F7558..0x823F7904` reads native sensitivity,
per-axis sensitivity, dead-zone, acceleration, free-look, and Y-inversion
properties before storing the derived values in the title controller object.
The Xbox profile setting `0x10040018` selects the native
`CONTROLLER_SENSITIVTY` enum in `0x820CC670..0x820CCA90`. V68 uses this verified
boundary: `input.controller_sensitivity` supplies standard Medium=0, Low=1, or
High=2 through `XamUserReadProfileSettings`, while
`input.controller_invert_y` supplies standard profile setting `0x10040002`.
The title then applies its own property constants and inversion logic. Medium
and off remain compatibility defaults. The runtime still adds no host
dead-zone, acceleration, axis scaling, or trigger transform.

V69 adds optional mouse acceleration only inside the already opt-in keyboard/
mouse bridge. The compatibility value `input.mouse_acceleration = 0.0` is an
exact identity path in every preset. Nonzero values are applied to each real
relative-motion event before accumulation, use a continuous bounded curve,
and cannot exceed 2x gain at the declared maximum. This avoids making the
nonlinear result depend on guest poll batching. Controller/XInput axes and the
title-owned controller curve are never passed through this policy.

V71 adds optional finite mouse smoothing at that same guest-poll boundary.
`input.mouse_smoothing = 0.0` is exact identity and resets history; every
packaged preset retains it. Nonzero values blend the current and previous poll
with a maximum 50% history weight, so an isolated movement has at most one
poll of tail and retains its total displacement. Capture start/release, focus
loss, and inactive input clear the filter. Physical XInput/controller state
does not enter this path.

The follow-up dead-zone/trigger audit found no safe control boundary. The
title has exactly one direct `CONTROLLER_DEADZONE` use: its own property getter
during controller construction, followed by storage at object offset `+0x22A0`.
No setter or content override was found. The complete controller-property
inventory has no trigger-threshold setting; `CONTROLLER_XBOX_TRIGGER` is an
input-mapping type. Host-side dead-zone or trigger shaping therefore remains
absent rather than double-applying or inventing a curve.
