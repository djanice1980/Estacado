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

The package and the installer contain the game's code in translated form.
Keep them for your own use.
