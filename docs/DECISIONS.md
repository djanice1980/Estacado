# Decisions

| Date | Decision | Rationale |
| --- | --- | --- |
| 2026-08-20 | Keep original game media in place. | No game dump was moved, edited, or copied; private media is ignored by Git. |
| 2026-08-20 | Use official XenonRecomp main at a recorded commit. | The local source snapshot lacks Git metadata. |
| 2026-08-20 | Defer XenosRecomp and ReXGlue acquisition. | No analysis evidence justifies adoption. |
| 2026-08-20 | Block builds until CMake and Ninja are installed. | CMake is required upstream and Ninja is the selected generator. |
| 2026-08-20 | Use x64 Clang-cl for all native tool builds. | The default LLVM directory targets x86 and failed CMake's linker probe. |
| 2026-08-20 | Retain the completed extraction in its actual private directory. | The extractor option-order error selected a root-level output directory; moving multi-gigabyte copyrighted data is unnecessary and risks data loss. |
| 2026-08-23 | First-pass recompilation uses no optimization or game-specific patches. | This establishes a reproducible baseline before any correctness work. |
| 2026-08-23 | Compile generated C++ as an object-only target. | It verifies every translation unit without claiming runtime, import, or linker integration. |
