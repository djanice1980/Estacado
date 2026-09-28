# External Switch Targets

Final regeneration used the preserved XenonAnalyse table file together with
`config/darkness_recomp_switch_correction.toml`. The original table file was
not modified.

| Source (`bctr`) | Table base | Targets | Final generated owner / flow | Classification | Match |
|---|---:|---|---|---|---|
| `0x82269A54` | `0x82269A40` | `0x82269E34`, `0x82269F5C`, `0x8226A0A4`, `0x8226A0D0`, `0x8226A0D8` | `sub_822699A0`; every case is a `goto loc_...` | LOCAL BASIC BLOCK | YES |
| `0x82274FDC` | `0x82274FC8` | `0x82275008` through `0x82275090` | `sub_82274FB0`; every case calls its exact generated target then returns from the current C++ frame | CROSS-FUNCTION TAIL TRANSFER | YES |
| `0x82758E94` | `0x82758E80` | `0x82758EB0`, `0x827591E0`, `0x82759060`, `0x827590E0`, `0x8275911C`, `0x82759158` | `sub_82758E50`; every case is a `goto loc_...` | PARSER FRAGMENT OF SAME LOGICAL FUNCTION | YES |
| `0x82758FF8` | `0x82758FE4` | `0x82758EBC`, `0x82759240` | `sub_82758E50`; every case is a `goto loc_...` | PARSER FRAGMENT OF SAME LOGICAL FUNCTION | YES |
| `0x82759178` | `0x82759164` | `0x82758EBC`, `0x8275923C` | `sub_82758E50`; every case is a `goto loc_...` | PARSER FRAGMENT OF SAME LOGICAL FUNCTION | YES |

There are zero ambiguous cases.

## Before and after

Before this change, a configured switch label outside the current generated
function unconditionally emitted an error comment and `return;`. That discarded
the required transfer for `0x82274FDC` and discarded internal control flow where
analysis had split the `0x822699A0..0x8226A0E0` and
`0x82758E50..0x82759248` regions.

After regeneration:

- The two verified ownership ranges are represented as `0x822699A0` size
  `0x740` and `0x82758E50` size `0x3F8`; their target labels are local C++
  `goto`s.
- `0x82275008` is represented as its verified 8-byte entry. The dispatch at
  `0x82274FDC` emits `sub_82275008(ctx, base); return;`. The target itself
  executes `mr r3,r11; b 0x82274A70`, generated as the same existing function
  call followed by C++ return.
- The code generator accepts an external configured switch label only when an
  exact `Symbol_Function` exists. Any other label retains its diagnostic and
  fail-safe return.

The C++ call plus immediate C++ return is the existing XenonRecomp representation
of a PPC non-linking branch: `ctx` and `base` are passed unchanged, no `ctx.lr`
assignment is emitted, and the source C++ activation does not resume.

Evidence is retained in
`logs/external_switch_codegen_recomp.log` and the generated output inspected in
`generated/ppc/ppc_recomp.17.cpp`, `.18.cpp`, and `.83.cpp`.
