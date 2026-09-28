# `frsqrte` execution-correctness analysis

## Status

**IMPLEMENTED AND LIVE-VALIDATED.** The Darkness contains 1,544 scalar
`frsqrte` instructions. XenonRecomp now emits `PPC_FRSQRTE` for all 1,544 and
the current regeneration reports zero `frsqrte` diagnostics.

Probe 47 isolated the earliest prompt-rendering divergence to two dynamically
executed instructions in `sub_82358038`:

- `0x82358198`: `frsqrte f13,f12`
- `0x823582B4`: `frsqrte f12,f12`

Before the fix, both instructions emitted comments only, leaving their
destinations stale and allowing canonical NaNs to enter the title's transform
pool, PM4 float constants, vertex constant buffer, and prompt draw.

## Implemented semantics

The helper uses the PowerISA reciprocal-square-root estimate table and integer
bit construction, including normal and denormal inputs, signed zero,
infinities, QNaN/SNaN, negative-input invalid operation, NI mode, enabled
VE/ZE destination suppression, FPSCR status/FPRF, and record-form CR1. It does
not substitute host `1.0 / sqrt()`.

The guest FPSCR value is preserved separately from the host CSR while host
rounding-mode synchronization remains intact. The Darkness uses only the
non-record form at its 1,544 sites, but record and exception-enabled behavior
is covered so the helper is reusable.

Primary evidence:

- [IBM AIX `frsqrte` reference](https://www.ibm.com/docs/en/ssw_aix_72/assembler/idalangref_frsqrte_instrs.html)
- [Xenia Canary PowerISA estimate implementation](https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/cpu/backend/a64/a64_sequences.cc)
- [Xbox-native Xenia Canary result vectors](https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/src/xenia/cpu/ppc/testing/instr__gen_frsqrte.s)

## Verification

- Focused suite: PASS, 13/13.
- Exact native vectors: signed zero, denormals, `1.0`, negative finite,
  infinities, QNaN, and SNaN all pass with CR1 checks.
- Estimate table: all 16 buckets pass the architectural 1/32 bound.
- VE/ZE no-result and FPSCR behavior: PASS.
- Regenerated code: 1,544 helper calls; zero `frsqrte` diagnostics.
- Generated compile: PASS, 128/128 translation units.
- Runtime build/link: PASS.
- Probe 48b: both live sites repeatedly receive `1.0`
  (`0x3FF0000000000000`) and produce the Xenon estimate
  `0x3FEF100000000000`.
- The former canonical-NaN transform, PM4, and constant-source probes are
  absent. The prompt vertex constant buffer contains finite values and the
  guest-rendered dark title artwork is visible.

Evidence files:

- `logs/frsqrte_probe48_recomp.log`
- `logs/frsqrte_probe48_generated_compile.log`
- `logs/m7_frsqrte_probe48.log`
- `logs/m7_frsqrte_fix_probe48b.stdout.log`
- `logs/m7_frsqrte_fix_probe48b.stderr.log`
- `logs/m7_frsqrte_fix_probe48b_live.bmp`
- `logs/m7_frsqrte_fix_probe48b_stable.bmp`
