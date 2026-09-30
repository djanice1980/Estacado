#!/usr/bin/env bash
# Runs the package (build/package) under Steam's Proton Experimental with its
# own prefix in the toolchain folder.
# Usage: linux/run-proton.sh [TheDarknessSettings.exe|TheDarkness.exe] [args...]
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
tc="${ESTACADO_TOOLCHAIN:-$repo/build/linux-toolchain}"
pkg="${ESTACADO_PACKAGE:-$repo/build/package}"
steam="$HOME/.steam/steam"
proton="${PROTON:-$steam/steamapps/common/Proton - Experimental/proton}"
[ -x "$proton" ] || { echo "Proton Experimental not found (install it in Steam, or set PROTON)."; exit 1; }
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam"
export STEAM_COMPAT_DATA_PATH="$tc/proton-prefix"
mkdir -p "$STEAM_COMPAT_DATA_PATH"
exe="${1:-TheDarknessSettings.exe}"
shift || true
cd "$pkg"
exec "$proton" run "$pkg/$exe" "$@"
