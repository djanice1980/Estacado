# Unsupported PowerPC instruction triage

Current inventory is reproduced from `logs/frsqrte_probe48_recomp.log`. The 80
remaining diagnostics are four instruction forms. The reachability labels for
those remaining forms come from `docs/BOOT_CPU_REACHABILITY.csv` and cover
resolved direct calls from `_xstart` only.

| Opcode / form | Occurrences | Function count | First example | Reachability | Semantics / support status | Proposed implementation | Test strategy |
| --- | ---: | ---: | --- | --- | --- | --- | --- |
| `vctuxs` | 67 | 4 | `0x829798C4` | STATICALLY_NOT_REACHED | VMX float-to-unsigned conversion with scale/rounding semantics; unsupported. | Blocked pending authoritative rounding, range, and VSCR/FPSCR behavior. | Boundary, NaN, rounding-mode, and scale vectors. |
| `dcbst` | 6 | 3 | `0x82420D1C` | STATICALLY_NOT_REACHED | Data-cache block store; unsupported. | Blocked pending host memory/coherency policy. It will not be silently dropped. | Multi-thread visibility/order test. |
| `vcfpuxws128` | 5 | 1 | `0x82979F6C` | STATICALLY_NOT_REACHED | Xenon VMX extended unsigned fixed-point conversion; unsupported. | Blocked pending Xenon-specific conversion evidence. | Exact lane/scale/rounding vectors. |
| `fnmadd` | 2 | 2 | `0x825E2FE4` | STATICALLY_NOT_REACHED | Fused negative multiply-add with guest FP state implications; unsupported. | Blocked pending FPSCR and exception/rounding handling. | NaN, signed-zero, exception, and rounding tests. |

## Completed during this queue

The supported, regenerated forms are `vpkuwum`, `vpkshss`, `vpkuhus`,
`vsubuhs`, `mulhd`, `cror`, `crorc`, and `frsqrte`. `frsqrte` removed 1,544
diagnostics after dynamic probe 47 reached two causal sites; the implementation
uses the verified estimate table and covers special values, FPSCR, enabled
exceptions, and record-form CR1 with Xbox-native vectors. See
`docs/FRSQRTE_ANALYSIS.md`.
