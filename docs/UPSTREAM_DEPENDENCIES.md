# Upstream dependencies

Checked on 2026-08-31. `external/XenonRecomp` is a nested upstream Git checkout, not committed by bootstrap.

| Component | Upstream | Pinned main commit | License | Build requirements / status |
| --- | --- | --- | --- | --- |
| XenonRecomp / XenonAnalyse | https://github.com/hedge-dev/XenonRecomp | `ddd128bcca99fe8bfbb99bea583c972351fa6ace` | MIT | CMake 3.20+, Clang 18+, submodules. Source is acquired and the pinned tools/tests build locally with CMake/Ninja. |
| XenosRecomp | https://github.com/hedge-dev/XenosRecomp | `990d03b28a27b50277ee5d8d942e1c5f873869d1` | MIT | CMake 3.20+, C++17, submodules. Shader inputs are now identified in `System/Xenon/ProgramCache.xpc`; not acquired or built pending a verified cache parser and compatibility test. |
| ReXGlue SDK | https://github.com/rexglue/rexglue-sdk | `cb58065c793429aa92895d778af58d12e9d26d8f` | BSD 3-Clause | Acquired, pinned, integrated, and clean-built with optimized `clang-cl` Release. Exact-commit metadata and deterministic DLL relinks/reconfiguration are verified; The Darkness adapters retain one authoritative guest-memory model. |
| extract-xiso | https://github.com/XboxDev/extract-xiso | `b72e5b60d598ec6df80534cda19cdcd4361aa18c` | Modified BSD-style author license in `extract-xiso.c` | Builds with CMake/Ninja; verified locally as extract-xiso v2.7.1 and used only on the local ISO. |

The existing `tools/XenonRecomp-main` is an unpacked snapshot without Git metadata. It is untouched and not canonical.

The M6A architecture decision and exact evidence are recorded in
`docs/GRAPHICS_FRONTIER.md`. Dependency acquisition remains a separate next
milestone; this discovery pass did not begin renderer implementation.
