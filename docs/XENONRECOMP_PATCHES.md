# XenonRecomp patches

## Configured-switch selector width

**Patch purpose:** dispatch configured `bctr` tables with the architectural
low 32-bit selector used by the PPC compare and table-index instructions.

**Pinned upstream commit:** `ddd128bcca99fe8bfbb99bea583c972351fa6ace`

**Upstream file modified:** `external/XenonRecomp/XenonRecomp/recompiler.cpp`

Probe70 reached the table at guest `0x824CCB34` with a selector GPR of
`0x0000000100000008`: `lwz` had loaded `0xFFFFFFFF`, then `addi 9` retained
the non-zero high half while `cmplwi` and `rlwinm` selected low-32 case 8.
XenonRecomp emitted `switch (register.u64)`, so the valid transfer to
`0x824CCB58` instead entered `__builtin_unreachable()` and caused the native
access violation in `sub_824CC7F8`.

Configured switches now emit `switch (register.u32)`. The focused codegen test
checks the emitted width and reproduces the exact
`0x0000000100000008 -> case 8` dispatch. Regenerated binary disassembly loads
the selector with `movl` before its native jump table; generated compilation,
runtime link, and all 15 regressions pass.

## External configured-switch target handling

**Patch purpose:** represent verified external targets of configured `bctr`
switch tables with the existing XenonRecomp inter-function tail-transfer
mechanism, while preserving diagnostics for unknown targets.

**Pinned upstream commit:** `ddd128bcca99fe8bfbb99bea583c972351fa6ace`

**Upstream file modified:** `external/XenonRecomp/XenonRecomp/recompiler.cpp`

### Root cause

`Recompiler::Recompile` handled a configured `PPC_INST_BCTR` label outside the
current function by emitting an error comment and `return;` unconditionally.
Elsewhere in the same method, a direct PPC `b` to an exact function symbol uses
`printFunctionCall(address); return;`. Therefore the configured-table path
failed to reuse the established cross-function branch representation.

The analysis pass also scanned direct `bl` targets before its sequential walk.
It could create an inferred function entry inside a manual configured range,
despite that range being an explicit ownership assertion. This re-fragmented
the two verified local regions and made their switch labels appear external.

### Semantics

For a configured table label:

1. A label inside the current function range remains `goto loc_<address>`.
2. A label outside that range is emitted as `printFunctionCall(label); return;`
   only for an exact `Symbol_Function` entry.
3. Any other label retains the original diagnostic and fail-safe `return;`.

This uses the existing `PPC_FUNC_IMPL` ABI, which passes the same
`PPCContext& ctx` and `base`. It does not modify `ctx.lr`; the immediate C++
return makes the non-linking PPC transfer a tail transfer rather than a normal
call/return sequence.

The analysis safeguard suppresses only automatic direct-call discovery whose
target lies inside a configured function range. It neither changes the binary
nor accepts unknown switch labels.

### Tests

`tests/register_helpers/external_switch_codegen_test.cpp` is a synthetic,
four-byte PPC `bctr` harness with no game data. It verifies:

1. a local label remains a `goto`;
2. an exact external function entry is invoked;
3. that transfer retains `ctx, base` and returns from the current C++ frame;
4. an unknown external label retains its diagnostic and is not called; and
5. existing local generation remains unchanged.

The target is registered with CTest as `external_switch_codegen`.

### The Darkness configuration evidence

`config/darkness_recomp_switch_correction.toml` contains only verified changes:

- `0x822699A0`, size `0x740`, the complete local owner through
  `0x8226A0E0`;
- `0x82758E50`, size `0x3F8`, the parser owner through `0x82759248`; and
- `0x82275008`, size `0x8`, the verified tail-table entry to `0x82274A70`.

The original `config/darkness_switch_tables.toml` remains preserved.

### Known limitation

The code generator intentionally does not create callable entries for arbitrary
interior labels. A configured external label must be an exact verified function
symbol; otherwise it remains diagnosed. This avoids silently treating unknown
data or a misclassified basic block as executable code.

## Game-specific switch overlay configuration

**Patch purpose:** allow a recompiler configuration to add or replace a small,
verified switch table without editing or duplicating the preserved XenonAnalyse
output.

**Upstream file modified:** `external/XenonRecomp/XenonRecomp/recompiler_config.cpp`.

`main.switches` accepts the same `base`, `r`, and `labels` schema as the
external table file. Entries load after that file and use `insert_or_assign`.
The first use is The Darkness parser table at `0x8222B5D0`; see
`CONTENT_STARTUP_ACCESS_VIOLATION.md` for raw-XEX evidence. This is data-driven
and leaves `config/darkness_switch_tables.toml` intact.

## VMX `vslh` support

**Patch purpose:** add code generation for the already decoded VMX Vector Shift
Left Integer Halfword instruction.

**Upstream files modified:**
`external/XenonRecomp/XenonRecomp/recompiler.cpp` and
`external/XenonRecomp/XenonUtils/ppc_context.h`.

The decoder already maps VX opcode 324 to `PPC_INST_VSLH`; no matching case
existed in `Recompiler::Recompile`, so it reached the generic unsupported path.
The patch emits an alias-safe scalar `PPC_VSLH` helper. It applies each source
halfword's corresponding `vB` low-four-bit count and does not affect condition
register state. See `PPC_INSTRUCTION_SEMANTICS.md` for specification and
representation evidence.

## PPC batch 1 VMX support

**Patch purpose:** implement six verified decoded VMX forms without changing
the XEX, game configuration, or generated C++ by hand.

**Upstream files modified:**
`external/XenonRecomp/XenonRecomp/recompiler.cpp` and
`external/XenonRecomp/XenonUtils/ppc_context.h`.

`vsrah`, `vspltish`, `vandc`, `vmaxsh`, and `vminsh` now emit alias-safe
helpers. The helpers are scalar intentionally: this avoids dependence on host
intrinsic signed-shift and aliasing behavior while preserving one guest lane
per corresponding host-order lane. `vsel128` joins the existing `vsel` case;
the local decoder establishes identical four-operand ordering and selection
semantics despite its Xenon VX128 register encoding.

`vsubshs`, `vpkswss`, and `vpkswus` are implemented with a right-aligned VSCR
field in `PPCContext`, persistent SAT updates, and decoded `mfvscr`/`mtvscr`
transfers. The helpers compute into temporaries, preserve alias safety, and set
SAT only when a clamp actually occurs.

## Runtime-only liveness instrumentation

**Patch purpose:** expose bounded function-entry, indirect-dispatch, and two
verified post-content control-field reads without editing generated C++ by hand
or changing guest semantics.

**Upstream files modified:** `XenonRecomp/recompiler.cpp` and
`XenonUtils/ppc_context.h`.

Each generated function emits `PPC_RUNTIME_FUNCTION_ENTER`; its default macro
is a no-op. Runtime builds force-include `runtime_function_trace.h`, which
records the per-thread function/import rings. Indirect calls use an equivalent
runtime override that validates `PPC_LOOKUP_FUNC` and retains the existing
transfer. Missing targets now fail explicitly rather than dereferencing null.

The only memory-read hooks are the independently disassembled `lwz` sites
`0x821F0FB8` and `0x821F0FD4` in `sub_821F0EB0`. They are no-ops in the normal
generated-code compile and read-only diagnostics in the runtime. See
`POST_CONTENT_LIVENESS.md` for their binary evidence and result.

## Xenon `db16cyc` cooperative spin hint

**Patch purpose:** preserve the decoded Xenon 16-cycle delay hint as an explicit
runtime hook instead of deleting it during code generation.

**Upstream files modified:** `XenonRecomp/recompiler.cpp` and
`XenonUtils/ppc_context.h`.

The Darkness guest thread 19 dynamically reached a polling loop in
`sub_828B3DC8` whose `ld`/compare/back-edge sequence contains `db16cyc` at
`0x828B3EAC` (raw instruction `0x7FFFFB78`). XenonRecomp previously emitted no
statement, leaving a host thread unable to observe a runtime teardown request
until the polled guest word changed. The generator now emits
`PPC_RUNTIME_DB16CYC()`. Its standalone default remains side-effect-free; the
title runtime overrides it with an x86 pause plus a private host stop check.
This neither changes PPC registers nor fabricates guest memory or completion.

## Guest vector-store runtime hook

**Patch purpose:** make generated `stvx` and `stvx128` stores observable by the
runtime-owned shared-memory coherency bridge without editing generated C++.

**Upstream files modified:** `XenonRecomp/recompiler.cpp` and
`XenonUtils/ppc_context.h`.

The previous generator emitted `simde_mm_store_si128(base + address, ...)`
directly. In the embedded external-memory configuration this bypassed the same
`RuntimeNotifyGuestPhysicalWrite` contract used by scalar stores and host file
reads. Live evidence proved that already-consumed UI index and atlas pages are
rewritten during the title prompt, so the missing notification was a real
coherency defect.

The generator now emits `PPC_STORE_V128(address, value)`. Its standalone default
preserves the previous shuffled 16-byte store exactly. The Darkness runtime
override performs that store and notifies ReXGlue only when the effective
address aliases guest physical memory, using the exact 16-byte half-open range.
No global invalidation, generated-code edit, or synthetic completion is used.

`vector_store_codegen_test` verifies that synthetic `stvx` output uses the hook
and contains no direct guest-memory vector store. Regeneration measured 18,239
hook calls and zero remaining direct vector stores. Runtime and ReXGlue builds
pass, and the full focused suite passes 12/12.
