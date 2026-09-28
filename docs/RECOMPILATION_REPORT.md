# Static recompilation report

## VERIFIED

- XenonRecomp command: `XenonRecomp.exe config/darkness_recomp.toml external/XenonRecomp/XenonUtils/ppc_context.h`.
- Input XEX: the verified local `default.xex` documented in `GAME_INPUT.md`.
- Switch input: unchanged `config/darkness_switch_tables.toml` with 675 `[[switch]]` sections.
- Initial first-pass exit code: 0; initial output: 131 files, 178,190,115 bytes, and 32,471 functions.
- Verified-helper pass exit code: 0; output: 132 files, 179,471,719 bytes, and 32,636 `PPC_FUNC_IMPL` definitions.
- Verified-helper structure: 128 `ppc_recomp.N.cpp` translation units, `ppc_func_mapping.cpp`, and three generated headers.
- Verified-helper compile: all 128 generated C++ translation units plus the mapping source compiled as x64 Clang-cl object code with zero compiler errors.
- Link/runtime integration: intentionally not attempted.

## RECOMPILER DIAGNOSTICS

- Initial first pass: 2,443 `ERROR:` lines: eight unspecified register-helper addresses and 2,435 switch cases targeting outside their detected function boundaries.
- Verified-helper pass: zero unspecified-helper diagnostics and 2,435 switch cases targeting outside their detected function boundaries.
- Both passes: 7,604 unsupported-instruction diagnostics spanning 40 distinct mnemonics; the most frequent are `vslh` (2,354), `frsqrte` (1,544), `vsrah` (979), `vsubshs` (901), and `vspltish` (602).
- 30 vector compare RC-bit diagnostics where no comparison was generated.

Verified helper mappings and the disassembly evidence are recorded in `docs/REGISTER_HELPERS.md` and `config/darkness_register_helpers.toml`. The remaining diagnostics did not stop code generation or C++ compilation; their execution effect remains UNKNOWN because runtime work has not begun.
