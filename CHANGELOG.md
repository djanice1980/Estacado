# Changelog

## 0.9.1 (pre-release)

- **Bloom at 2x/3x/4x internal resolution** no longer bands or streaks around
  lights: the glow is made at the console's resolution and enlarged smoothly,
  in both of the game's anti-aliasing modes. By djanice1980
  ([#11](https://github.com/invinceble55-wq/Estacado/issues/11),
  [#15](https://github.com/invinceble55-wq/Estacado/pull/15),
  [Estacado-ReXGlue #1](https://github.com/invinceble55-wq/Estacado-ReXGlue/pull/1);
  fixes [#1](https://github.com/invinceble55-wq/Estacado/issues/1)).
- **Pause menu colours at 2x and 4x** internal resolution: the greenish haze
  over the paused game is gone. The pause menu's colour-grading table was
  drawn at the scaled resolution and read at the console's grid, which only
  lines up at odd scales; it is now drawn at the console's resolution like
  the game's other grading tables
  ([#10](https://github.com/invinceble55-wq/Estacado/issues/10)).
- **No more yellow flashes in the Chapter 1 car ride:** 0.9.0 showed single
  over-exposed yellow frames there (35 to 45 in each 140-second recording of
  the ride at 2x internal resolution); 0.9.1 showed none in four recordings.
- **Controllers on any port:** a controller that Windows, Steam Input or a
  virtual-pad driver puts on a port other than the first now plays as the
  player with the profile, so the profile and saves load and keyboard and
  controller can be switched freely
  ([#8](https://github.com/invinceble55-wq/Estacado/issues/8)).
- **Saves and settings in Saved Games** (`Saved Games\Estacado`), so updates
  never lose progress. The first start in a 0.9.0 folder copies its saves and
  settings there, checks every file and keeps the originals; the launcher can
  import saves from another folder; `portable.txt` keeps everything in the
  game folder. Suggested by djanice1980
  ([#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- **Other releases of the game** run when they contain the same code (for
  example localised releases): the launcher compares the decrypted
  executable, not the file. Other versions get a plain message and a local
  report ([#9](https://github.com/invinceble55-wq/Estacado/issues/9)).
- The launcher names any file missing from an incomplete extraction, and the
  zip now stores its folders explicitly
  ([#2](https://github.com/invinceble55-wq/Estacado/issues/2)).
- Folders and Windows user names with letters outside the system's code page
  work for settings and saves.
- **Quit game** button in the in-game settings (F1), with a confirmation
  (djanice1980, [#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- **Key bindings name each button's action** in the game's default controller
  layout, for example *Y button: jump* and *A button: use*, so a key can be
  moved to the action you want
  ([#12](https://github.com/invinceble55-wq/Estacado/issues/12)).
- **Jump is on Space and use on E** by default, as in most PC games (0.9.0 had
  them the other way round). Settings from 0.9.0 get the new keys if those two
  were never changed; keys you chose yourself stay as they are.
- **Button prompts follow your keys:** while you play with keyboard and mouse,
  the game's button icons show the keys you bound (for example *E*, *Space*,
  *Shift*, *LMB*) and prompt texts read like "Press E"; the Xbox buttons come
  back as soon as you use a controller. *Button prompts* in the settings
  (Automatic, Xbox buttons, Keyboard keys) changes this, also during play.
- **Less stutter in new areas on the first play:** the release now carries a
  data-free list of the game's shaders and pipelines: shader hashes, pipeline
  render states and, per shader, which shader of the game's own
  `System\Xenon\ProgramCache.xpc` it is plus the vertex-fetch bindings the
  console's Direct3D patches in (no shader code). At startup the shaders are
  rebuilt from the player's copy, each checked against its hash, and every
  pipeline the list names is compiled in the background, with a small
  progress note. First start with cold caches, spawn to the Chinatown street:
  16 compile waits (170 ms) before, 1 (2 ms) now. Shaders the game creates
  that are not on the list are still found in memory and compiled before
  their first draw when possible
  ([#5](https://github.com/invinceble55-wq/Estacado/issues/5)).
- Developer thread snapshots are no longer written to `logs` at every start.

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
