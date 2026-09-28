# Steam Deck / Proton compatibility audit

Status: **PHASE 0 FOUNDATION / LIVE PROTON VALIDATION PENDING**

The supported path under investigation is the existing optimized Windows
build running through Proton and VKD3D-Proton. A native Linux renderer is not
justified while the D3D12 title path remains the authoritative validated path.

## Offline-verified foundations

- The generated runtime is a 64-bit Windows executable and ReXGlue presents
  through D3D12. Steam Deck must therefore use Proton for this phase.
- The runtime's required `/arch:AVX` contract is compatible with Steam Deck's
  Zen 2 CPU, but remains a declared host requirement.
- Controller input uses the Windows XInput 1.4 contract, preserving the same
  guest packet, button, stick, trigger, and vibration semantics validated on
  Windows. Proton controller mapping still requires live validation.
- The title continues to see its verified 1280x720 widescreen mode. The new
  `steam_deck_800p_compatibility.toml` preset selects a 1280x800 host surface
  and contains the complete 16:9 image without stretching or cropping.
- The preset deliberately leaves `frame_limit = 0`. It does not advertise a
  30, 40, or 60 FPS Deck tier before title-side frame production and actual
  device performance are measured.
- PC configuration, mod configuration, and original extracted content remain
  separately selected. The filesystem override system is deterministic and
  default-disabled.
- Portable save/profile/achievement data still defaults beside the executable
  under `runtime_data`. `--user-data-root <path>` now permits a launcher to
  choose a Proton-prefix or other managed writable directory without changing
  the current portable default. All guest save operations remain confined to
  the selected root.
- Graphics shader/pipeline cache storage prefers `%LOCALAPPDATA%`, which Proton
  maps inside its prefix, and retains the title/XEX/settings identity checks.
- V40 provides `--print-paths` and `--list-presets` as read-only launcher
  actions. They report the exact Proton-visible XEX, PC/mod configuration,
  writable/content roots, and seven packaged presets without initializing the
  guest, GPU, audio, or input. Default XEX discovery is executable-anchored and
  no longer changes with Proton's launch working directory.
- `--preset steam_deck_800p_compatibility` selects that packaged preset by
  contained name. Unknown names, path components, and conflicts with an
  explicit configuration fail before guest startup; no current-directory copy
  or launcher-side path assembly is required.
- V75 resolves verified ASCII Xbox path components case-insensitively beneath
  canonical extracted-content, loose-overlay, and portable-mount roots. Direct
  host spellings remain the fast path; a casing miss requires one unique
  fallback match, and writable creation may append only one missing final leaf
  beneath an already resolved portable parent.

## Unverified Proton-specific gates

The following are not claimed complete without a real Steam Deck/Proton run:

1. D3D12/VKD3D-Proton feature support, swapchain flags, VSync, VRR, and
   fullscreen behavior.
2. WinMM `waveOut` device cadence, audible channel layout, and suspend/resume
   recovery.
3. XInput hot-plug, Steam Input mapping, vibration, glyph expectations, and
   controller-only navigation.
4. Real mounted-corpus behavior through Wine's filesystem translation. The
   offline resolver removes the known host-casing dependency, but the complete
   extracted tree, overlays, and portable saves still require a Proton run.
5. Device suspend/resume. Guest monotonic clocks, audio queues, GPU resources,
   and the persistent cache must be checked across a real sleep cycle.
6. CPU, GPU, VRAM, memory, power, battery, and traversal-stutter measurements.

## First consolidated Deck validation

After the Windows Phase 1 candidate is live-validated, use one Proton session
with the contained 800p preset and a dedicated writable user-data root. Record
startup/module identity, intro/audio cadence, menu/controller behavior, first
gameplay, present intervals, PSO/cache events, saves, suspend/resume, and one
checkpoint reload. Only those measurements may justify named 30/40/60 FPS
quality tiers.
