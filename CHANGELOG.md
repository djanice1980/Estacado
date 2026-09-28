# Changelog

## 0.9.0 (pre-release)

The first public pre-release. Tested on one Windows 11 PC with an NVIDIA RTX
graphics card; other hardware is untested, so reports are very welcome (see
[Reporting a problem](README.md#reporting-a-problem)).

- Native Windows port of *The Darkness* (Xbox 360, USA/Europe disc) by
  static recompilation, with a Direct3D 12 renderer.
- First start: the launcher takes your disc image or extracted game folder,
  checks that it is the supported version and prepares it.
- Frame rate: Original 30, 60, the display's refresh rate, a custom limit or
  uncapped, with scripted scenes, physics and audio at their original speed.
- Resolution: 1x/2x/3x internal scale or automatic, any output resolution,
  windowed or borderless.
- Anti-aliasing: SMAA (default) or FXAA; AMD FSR 1 or CAS when the image is
  fitted to the screen.
- Keyboard and mouse with native mouse look and rebindable keys; controller;
  field of view.
- Settings in the launcher and in an in-game overlay (F1); presets Enhanced,
  Performance, Original and Steam Deck.
- Motion blur on or off and HD texture packs.
- Languages: the game's five (English, French, German, Italian, Spanish).
- Widescreen for 21:9, 32:9 and 16:10 screens (experimental, off by default).
- Crash reports (log and minidump) in the `logs` folder.
