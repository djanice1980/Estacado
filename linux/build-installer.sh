#!/usr/bin/env bash
# Builds a Windows setup.exe from build/package with Inno Setup under Wine
# (linux/setup-toolchain.sh --installer). Output: build/installer.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
tc="${ESTACADO_TOOLCHAIN:-$repo/build/linux-toolchain}"
pkg="$repo/build/package"
out="$repo/build/installer"
iscc="$tc/installer/inno-prefix/drive_c/InnoSetup/ISCC.exe"
[ -f "$pkg/TheDarkness.exe" ] || { echo "No package: run linux/build.sh first."; exit 1; }
[ -f "$iscc" ] || { echo "No Inno Setup: run linux/setup-toolchain.sh --accept-msvc-license --installer"; exit 1; }
version="0.9.0+$(git -C "$repo" rev-parse --short HEAD).$(date +%Y%m%d)"
mkdir -p "$out"
winpath() { echo "Z:$(realpath "$1" | tr / '\\')"; }
export WINEPREFIX="$tc/installer/inno-prefix" WINEDEBUG=-all
wine "$iscc" /Q \
  "/DPackageDir=$(winpath "$pkg")" \
  "/DRedistDir=$(winpath "$tc/installer/redist")" \
  "/DOutputDir=$(winpath "$out")" \
  "/DAppVersion=$version" \
  "$(winpath "$repo/linux/installer/estacado.iss")"
ls -la "$out"
