#!/usr/bin/env bash
# Downloads what the Linux cross-build needs into build/linux-toolchain
# (or $ESTACADO_TOOLCHAIN):
#   - xwin 0.10.0 (checksum-verified) and, through it, the MSVC CRT and the
#     Windows SDK from Microsoft. This accepts the Microsoft Visual Studio
#     license terms (https://go.microsoft.com/fwlink/?LinkId=2086102), so it
#     only runs with --accept-msvc-license.
#   - with --installer: Inno Setup 6.7.3 (installed into its own Wine prefix)
#     and the Visual C++ runtime installer, for linux/build-installer.sh.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
tc="${ESTACADO_TOOLCHAIN:-$repo/build/linux-toolchain}"
accept=0
installer=0
for argument in "$@"; do
  case "$argument" in
    --accept-msvc-license) accept=1 ;;
    --installer) installer=1 ;;
    *) echo "usage: $0 --accept-msvc-license [--installer]"; exit 2 ;;
  esac
done
[ "$accept" = 1 ] || { echo "Read https://go.microsoft.com/fwlink/?LinkId=2086102, then run: $0 --accept-msvc-license"; exit 2; }
mkdir -p "$tc/bin"

xwin_version=0.10.0
xwin_dir="xwin-$xwin_version-x86_64-unknown-linux-musl"
if [ ! -x "$tc/$xwin_dir/xwin" ]; then
  url="https://github.com/Jake-Shadle/xwin/releases/download/$xwin_version/$xwin_dir.tar.gz"
  curl -sSL -o "$tc/$xwin_dir.tar.gz" "$url"
  expected=$(curl -sSL "$url.sha256" | cut -c1-64)
  echo "$expected  $tc/$xwin_dir.tar.gz" | sha256sum -c -
  tar -xzf "$tc/$xwin_dir.tar.gz" -C "$tc"
fi
if [ ! -d "$tc/sysroot/crt" ]; then
  "$tc/$xwin_dir/xwin" --accept-license --cache-dir "$tc/xwin-cache" splat --output "$tc/sysroot"
fi
# runtime/CMakeLists.txt runs generate_import_traps.ps1 through "powershell".
printf '#!/bin/sh\nexec pwsh "$@"\n' > "$tc/bin/powershell"
chmod +x "$tc/bin/powershell"

if [ "$installer" = 1 ]; then
  mkdir -p "$tc/installer/redist"
  inno="$tc/installer/innosetup-6.7.3.exe"
  if [ ! -f "$inno" ]; then
    curl -sSL -o "$inno" https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe
    echo "9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732  $inno" | sha256sum -c -
  fi
  [ -f "$tc/installer/redist/VC_redist.x64.exe" ] ||
    curl -sSL -o "$tc/installer/redist/VC_redist.x64.exe" https://aka.ms/vs/17/release/vc_redist.x64.exe
  if [ ! -f "$tc/installer/inno-prefix/drive_c/InnoSetup/ISCC.exe" ]; then
    WINEPREFIX="$tc/installer/inno-prefix" WINEDEBUG=-all wine "$inno" \
      /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER "/DIR=C:\\InnoSetup"
  fi
fi
echo "Toolchain ready in $tc"
