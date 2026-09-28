# Register helper resolution

Source input: the verified local `default.xex` (XEX2, SHA-256 recorded in `docs/GAME_INPUT.md`). The addresses below come from a code-section scan performed through the pinned XenonUtils XEX loader, not from another title. Each documented signature was found exactly once in `.text`.

## `__restgprlr_14`

ADDRESS: `0x82899A40`  
REFERENCES: `restgprlr_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: `ld r14..r31,-0x98..-0x10(r1)`; `lwz r12,-8(r1)`; `mtlr r12`; `blr`.  
EXPECTED PURPOSE: Restore non-volatile GPRs and LR from the current stack frame.  
EVIDENCE: Exact pinned-XenonRecomp signature `E9 C1 FF 68`, one `.text` match, and the full register/LR sequence in `logs/register_helpers_disassembly.log`.  
CONFIDENCE: VERIFIED

## `__savegprlr_14`

ADDRESS: `0x828999F0`  
REFERENCES: `savegprlr_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: `std r14..r31,-0x98..-0x10(r1)`; `stw r12,-8(r1)`; `blr`.  
EXPECTED PURPOSE: Save non-volatile GPRs and LR value supplied in `r12`.  
EVIDENCE: Exact pinned-XenonRecomp signature `F9 C1 FF 68`, one `.text` match, and the complete save sequence.  
CONFIDENCE: VERIFIED

## `__restfpr_14`

ADDRESS: `0x8289A1DC`  
REFERENCES: `restfpr_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: `lfd f14..f31,-0x90..-0x08(r12)`; `blr`.  
EXPECTED PURPOSE: Restore non-volatile floating-point registers from the frame pointer in `r12`.  
EVIDENCE: Exact pinned-XenonRecomp signature `C9 CC FF 70`, one `.text` match, and 18 ordered loads ending in `blr`.  
CONFIDENCE: VERIFIED

## `__savefpr_14`

ADDRESS: `0x8289A190`  
REFERENCES: `savefpr_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: `stfd f14..f31,-0x90..-0x08(r12)`; `blr`.  
EXPECTED PURPOSE: Save non-volatile floating-point registers through `r12`.  
EVIDENCE: Exact pinned-XenonRecomp signature `D9 CC FF 70`, one `.text` match, and 18 ordered stores ending in `blr`.  
CONFIDENCE: VERIFIED

## `__restvmx_14`

ADDRESS: `0x829B8F08`  
REFERENCES: `restvmx_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: Repeated `li r11,-0x120..-0x10`; `lvx v14..v31,r11,r12`; `blr`.  
EXPECTED PURPOSE: Restore VMX registers `v14` through `v31`.  
EVIDENCE: Exact pinned-XenonRecomp eight-byte signature `39 60 FE E0 7D CB 60 CE`, one `.text` match, and ordered VMX loads.  
CONFIDENCE: VERIFIED

## `__savevmx_14`

ADDRESS: `0x829B8C70`  
REFERENCES: `savevmx_14_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: Repeated `li r11,-0x120..-0x10`; `stvx v14..v31,r11,r12`; `blr`.  
EXPECTED PURPOSE: Save VMX registers `v14` through `v31`.  
EVIDENCE: Exact pinned-XenonRecomp eight-byte signature `39 60 FE E0 7D CB 61 CE`, one `.text` match, and ordered VMX stores.  
CONFIDENCE: VERIFIED

## `__restvmx_64`

ADDRESS: `0x829B8F9C`  
REFERENCES: `restvmx_64_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: Repeated `li r11,-0x400...`; `lvx128 v64...`, beginning with `v64`; continuation and terminal branch are preserved in the scanner log.  
EXPECTED PURPOSE: Restore the Xenon extended VMX register bank beginning at `v64`.  
EVIDENCE: Exact pinned-XenonRecomp eight-byte signature `39 60 FC 00 10 0B 60 CB`, one `.text` match, and ordered `lvx128` operations.  
CONFIDENCE: VERIFIED

## `__savevmx_64`

ADDRESS: `0x829B8D04`  
REFERENCES: `savevmx_64_address` in the verified map and recompiler configuration.  
CURRENT CLASSIFICATION: VERIFIED  
DISASSEMBLY: Repeated `li r11,-0x400...`; `stvx128 v64...`, beginning with `v64`; continuation and terminal branch are preserved in the scanner log.  
EXPECTED PURPOSE: Save the Xenon extended VMX register bank beginning at `v64`.  
EVIDENCE: Exact pinned-XenonRecomp eight-byte signature `39 60 FC 00 10 0B 61 CB`, one `.text` match, and ordered `stvx128` operations.  
CONFIDENCE: VERIFIED

## Mapping policy

`config/darkness_register_helpers.toml` contains only these eight VERIFIED mappings. XenonRecomp consumes the same values from `[main]` in `config/darkness_recomp.toml`; no XenonRecomp source modification is required.
