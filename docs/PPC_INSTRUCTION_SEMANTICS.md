# PowerPC instruction semantics

## VMX `vslh` — Vector Shift Left Integer Halfword

### Authoritative instruction semantics

The Vector/SIMD Multimedia Extension Technology Programming Environments Manual,
version 2.07c, section 6 (`vslh`, page 249), defines opcode VX 324 as:

```
for i = 0, 16, ..., 112:
    sh = vB[i+12:i+15]
    vD[i:i+15] = vA[i:i+15] << unsigned(sh)
```

The manual explicitly states that the registers contain eight 16-bit elements.
Thus the architectural count is the low four bits of the corresponding `vB`
halfword (`& 0xF`), in the range 0–15. Each lane shifts independently;
vacated low bits are zero, shifted-out high bits are discarded, and the result
is truncated to 16 bits. The instruction has no record bit and does not modify
CR, XER, FPSCR, or VSCR.

Primary source: [PowerPC Vector/SIMD Multimedia Extension Technology Programming
Environments Manual v2.07c, page 249](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/vector_simd_pem_v_2.07c_26Oct2006_cell.pdf).

### XenonRecomp representation evidence

`PPCVRegister` is a 16-byte aligned union with `u8[16]`, `u16[8]`, `u32[4]`,
and other views. The guest presents lane 0 as the most-significant halfword;
the little-endian host union therefore stores guest lane 0 in `u16[7]` and
guest lane 7 in `u16[0]`. XenonRecomp reverses the complete 16-byte vector on
guest VMX loads and stores using `VectorMaskL`; its source comments explicitly
state that the reversal is accounted for by instruction code generation.

For elementwise same-width operations, a complete reversal maps both source
lanes and destination lanes identically. Therefore `u16[i]` paired with
`u16[i]` is correct for this operation; no extra shuffle is required. This is
consistent with the existing scalar `vslb` implementation and the neighboring
`vslw` implementation, which operate corresponding host-order elements.

### Production representation

`PPC_VSLH` in `XenonUtils/ppc_context.h` computes the eight lanes into a local
`PPCVRegister` before storing the result. This is deliberately scalar and
alias-safe: `vD` may equal `vA` or `vB`. `recompiler.cpp` emits one
`PPC_VSLH(vD, vA, vB)` call for `PPC_INST_VSLH`; it does not emit any CR update.

### Test oracle

`tests/register_helpers/vslh_test.cpp` contains an independent scalar oracle
that uses multiplication by `2^shift` and truncation rather than the production
helper. It covers deterministic edge cases, aliasing, unique lane values, and
10,000 fixed-seed randomized vectors.

### The Darkness regeneration verification

`logs/vslh_recomp.log` contains no `vslh` unsupported diagnostic. It contains
5,250 remaining unsupported diagnostics across 39 forms, exactly 2,354 fewer
than the pre-patch total. The generated output contains 2,354 `PPC_VSLH` calls.
Representative sites in `generated/ppc/ppc_recomp.116.cpp` include:

- `vslh v31,v9,v12` → `PPC_VSLH(ctx.v31, ctx.v9, ctx.v12);`
- `vslh v4,v7,v13` → `PPC_VSLH(ctx.v4, ctx.v7, ctx.v13);`
- `vslh v3,v7,v11` → `PPC_VSLH(ctx.v3, ctx.v7, ctx.v11);`

Each sample writes only its specified destination vector and retains its
surrounding instruction ordering. `logs/vslh_generated_compile.log` records a
successful complete generated-code compile with zero errors.

## PPC batch 1 VMX operations

The following implementations were added only after checking the relevant
per-instruction pseudocode in the Vector/SIMD Multimedia Extension Technology
Programming Environments Manual v2.07c: `vsrah` p.261, `vspltish` p.255,
`vandc` p.155, `vmaxsh` p.187, and `vminsh` p.196. The same host/guest vector
lane reversal reasoning described above applies to every same-width,
elementwise operation below.

| Instruction | Verified operation | Architectural side effects | Production representation |
| --- | --- | --- | --- |
| `vsrah` | For each signed halfword, arithmetic-right-shift `vA` by `vB & 15`. | None | Alias-safe `PPC_VSRAH` temporary. |
| `vspltish` | Sign-extend its decoded five-bit immediate and copy it to all eight halfwords. | None | `PPC_VSPLTISH`; decoder marks `SIMM` signed. |
| `vandc` | Per bit, `vA & ~vB`. | None | Alias-safe `PPC_VANDC` temporary. |
| `vmaxsh` | Per signed-halfword lane, select `vA` if `vA >= vB`, otherwise `vB`. | None | Alias-safe `PPC_VMAXSH` temporary. |
| `vminsh` | Per signed-halfword lane, select `vA` if `vA < vB`, otherwise `vB`. | None | Alias-safe `PPC_VMINSH` temporary. |
| `vsel128` | Per bit, select `vB` where mask bit is one, else `vA`. | None | Shares the verified existing `vsel` expression. |

`vsel128` is a Xenon VX128 decoder form. `ppc-dis.c` defines its four operands
as `{ VD128, VA128, VB128, VS128 }`; standard `vsel` is `{ VD, VA, VB, VC }`.
Both feed the same destination/source/mask ordering, so the code generator
uses the existing expression `(~mask & A) | (mask & B)`. The synthetic
`vmx_batch1` test decodes `VX128(5, 848)` and verifies that it selects this
code path rather than emitting an unsupported diagnostic.

`vmx_batch1_test.cpp` uses independent scalar oracles and fixed-seed random
vectors for 10,000 cases each. It covers negative values, shift counts with
high bits set, signed extrema, equality, bit patterns, and destination aliasing
for the five helper operations. Its `vspltish` cases include `-16`, `-1`, and
`15`; the decoder's `PPC_OPERAND_SIGNED` SIMM definition verifies the source
immediate is sign-extended before generated C++ sees it.

Batch regeneration in `logs/ppc_batch1_recomp.log` records zero diagnostics
for all six forms. `logs/ppc_batch1_compile.log` records a successful complete
generated-code compile with zero errors.

## Saturating VMX operations

The Group 3 operations were implemented after adding a guest VSCR model:

| Instruction | Verified data result | Required side effect | Why it remains diagnosed |
| --- | --- | --- | --- |
| `vsubshs` | Signed 16-bit `vA - vB`, clamped independently to `[-32768, 32767]`. | Set persistent `VSCR[SAT]` if any lane saturates. | Implemented. |
| `vpkswss` | Pack four signed 32-bit lanes from `vA`, then four from `vB`, into signed-saturated 16-bit lanes. | Set persistent `VSCR[SAT]` if any packed value saturates. | Implemented. |
| `vpkswus` | Pack four signed 32-bit lanes from `vA`, then four from `vB`, into unsigned-saturated 16-bit lanes, clamped to `[0, 65535]`. | Set persistent `VSCR[SAT]` if any packed value saturates. | Implemented. |

`PPCContext` now stores the right-aligned 32-bit VSCR. SAT is architectural
bit 31, represented by value bit 0; `mfvscr` clears the upper 96 vector bits
and returns the value in the low 32 bits, while `mtvscr` restores that field.
The helpers set SAT only for a true clamp and never clear it. Regeneration
records zero diagnostics for all three forms; focused tests cover signed and
unsigned clamp boundaries, packing order, and SAT persistence.
