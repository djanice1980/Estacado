# Architecture

Private input lives in ignored `game/original_dump`. Upstream tools live in `external`; ignored generated PPC C++ in `generated/ppc`; game-specific code in `src`; and evidence-driven compatibility components in `runtime`.

No runtime implementation exists before verified XEX analysis.
