# Building on Linux

These scripts build the Windows port on Linux and run it under Proton. They
don't make a native Linux version: the renderer is Direct3D 12, which Proton
runs through vkd3d-proton. The recompiler runs natively. ReXGlue and the
runtime are cross-compiled with clang-cl and lld-link against the MSVC CRT
and Windows SDK, which xwin downloads from Microsoft onto your machine.

Tested on CachyOS with clang 22, CMake 4, Ninja, PowerShell (pwsh), Wine 11
and Steam's Proton Experimental.

## What you need

- clang, lld and llvm (for `clang-cl`, `lld-link`, `llvm-rc`, `llvm-lib`,
  `llvm-mt`), cmake, ninja, python3, curl
- PowerShell 7 (`pwsh`), used for the import-trap generator and packaging
- Steam with Proton Experimental, to play
- Wine, only for the Windows installer
- your game: a folder with the extracted files (the one that contains
  `default.xex`, the supported version from the main README)

On Arch/CachyOS:

```bash
sudo pacman -S --needed clang lld llvm cmake ninja python curl wine
```

`pwsh` is in the AUR as `powershell-bin`.

## Build and play

```bash
linux/setup-toolchain.sh --accept-msvc-license
linux/build.sh "/path/to/game folder"
linux/run-proton.sh
```

- `setup-toolchain.sh` downloads xwin and the MSVC CRT and Windows SDK into
  `build/linux-toolchain`. The flag accepts the
  [Microsoft Visual Studio license terms](https://go.microsoft.com/fwlink/?LinkId=2086102).
- `build.sh` checks your `default.xex`, recompiles it, builds everything and
  writes a player package to `build/package`. The game folder is only needed
  the first time; later builds are incremental.
- `run-proton.sh` starts the launcher. `run-proton.sh TheDarkness.exe` starts
  the game directly.

To play from Steam instead: *Games → Add a Non-Steam Game*, pick
`build/package/TheDarknessSettings.exe`, then under *Properties →
Compatibility* force Proton Experimental. For a DualSense or other
non-Xbox controller, set *Properties → Controller* to *Enable Steam Input*.

`build/package` contains `portable.txt`, so settings, saves and logs stay in
that folder.

## Game in Games on Demand (GOD) format

The launcher reads a disc image or an extracted folder, but not a GOD
package. `svod_extract.py` extracts one. Pass the header file (the one with a
`.data` folder next to it):

```bash
python3 linux/svod_extract.py "545407EE/00007000/<header>" "/path/to/game folder"
```

## Windows installer

```bash
linux/setup-toolchain.sh --accept-msvc-license --installer
linux/build-installer.sh
```

This writes `build/installer/TheDarkness-Estacado-<version>-Setup.exe`, an
Inno Setup installer. It installs to Program Files and includes the Visual
C++ runtime. It doesn't include any game files. After installing, settings
and logs go to `%LOCALAPPDATA%\The Darkness` and saves to `Saved Games\The
Darkness`.

## CachyOS / Arch package

```bash
linux/build-arch-package.sh
sudo pacman -U build/arch/the-darkness-estacado-*.pkg.tar.zst
```

This builds a pacman package from `build/package`. It installs the game to
`/opt/the-darkness-estacado`, adds a `the-darkness` command and two menu
entries (The Darkness, The Darkness Settings), and comes off again with
`sudo pacman -R the-darkness-estacado`. It doesn't include any game files:
start **The Darkness Settings** (or `the-darkness --settings`), choose your
game folder or disc image and a preset, then play.

It runs under Proton in one of two ways (`the-darkness --help`):

- **umu** (the default when installed): `sudo pacman -S umu-launcher
  proton-cachyos`. No Steam needed; the first start downloads umu's Steam
  Linux Runtime (about 650 MB).
- **Steam**: Steam with Proton Experimental installed (Library > Tools).
  Used when umu isn't installed, or with `THE_DARKNESS_RUNTIME=steam`.

Each runtime keeps its own Wine prefix under
`~/.local/share/the-darkness-estacado/` (`prefix-umu`, `prefix-steam`) with
that runtime's settings and saves, so they don't carry over when you switch.

The packages and the installer contain the game's code in translated form
and no game data; see Legal in the main README.
