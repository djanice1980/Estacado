#!/usr/bin/env bash
# Builds the port on Linux: XenonRecomp natively, then ReXGlue and the runtime
# as Windows x64 binaries with clang-cl/lld-link against the xwin sysroot, then
# the player package in build/package (run it with linux/run-proton.sh).
# Equivalent of scripts/build-all.ps1 without the tests.
#
# Usage: linux/build.sh [game folder]   (the folder that contains default.xex;
# only needed the first time, or set ESTACADO_GAME)
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
tc="${ESTACADO_TOOLCHAIN:-$repo/build/linux-toolchain}"
export ESTACADO_XWIN_SYSROOT="$tc/sysroot"
toolchain_file="$repo/linux/clang-cl-msvc.cmake"
game_link="Darkness, The (USA, Europe) (En,Fr,De,Es,It)"
[ -d "$tc/sysroot/crt" ] || { echo "Run linux/setup-toolchain.sh first."; exit 1; }
export PATH="$tc/bin:$PATH"
cd "$repo"
mkdir -p logs generated/ppc

echo "== Game files"
game="${1:-${ESTACADO_GAME:-}}"
if [ -n "$game" ]; then
  ln -sfn "$(realpath "$game")" "$game_link"
fi
[ -f "$game_link/default.xex" ] || { echo "Pass the game folder (the one with default.xex)."; exit 1; }
game="$(realpath "$game_link")"
want=aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f
have=$(sha256sum "$game_link/default.xex" | cut -d' ' -f1)
[ "$have" = "$want" ] || { echo "default.xex is not the supported version ($have)"; exit 1; }

echo "== Recompiler (native)"
cmake -S external/XenonRecomp -B build/XenonRecomp-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > logs/xenonrecomp-configure.log
cmake --build build/XenonRecomp-linux > logs/xenonrecomp-build.log
build/XenonRecomp-linux/XenonAnalyse/XenonAnalyse "$game_link/default.xex" \
  config/darkness_switch_tables.toml > logs/XenonAnalyse.log 2>&1
build/XenonRecomp-linux/XenonRecomp/XenonRecomp config/darkness_recomp_switch_correction.toml \
  external/XenonRecomp/XenonUtils/ppc_context.h > logs/switch_correction_recomp.log 2>&1

echo "== Sysroot case-variant links"
python3 linux/case_symlinks.py "$tc/sysroot" runtime tools tests external

echo "== ReXGlue (cross)"
rex=external/ReXGlue
if [ ! -f "$rex/out/build/win-amd64/build.ninja" ]; then
  # The options of scripts/build-rexglue.ps1 and the win-amd64 preset. Pass
  # flags as *_FLAGS_INIT: *_FLAGS would replace the toolchain's sysroot.
  cmake -S "$rex" -B "$rex/out/build/win-amd64" -G "Ninja Multi-Config" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
    -DCMAKE_C_FLAGS_INIT="/clang:-march=x86-64-v3" \
    -DCMAKE_CXX_FLAGS_INIT="/clang:-march=x86-64-v3" \
    -DCMAKE_CXX_STANDARD=23 "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo" \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DREXGLUE_GPU_THINLTO=ON -DREXGLUE_GPU_PGO="${PGO:-USE}" \
    -DREXGLUE_GPU_PGO_PROFILE="$repo/config/pgo/rexgpu-v404.profdata" \
    -DREXGLUE_GPU_DIAGNOSTICS=OFF -DREXGLUE_USE_VULKAN=OFF -DREXGLUE_USE_D3D12=ON \
    -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_ENABLE_SANITIZERS=OFF \
    -DREXGLUE_OUTPUT_DIR="$repo/$rex/out/win-amd64" > logs/rexglue-configure.log
fi
cmake --build "$rex/out/build/win-amd64" --config Release > logs/rexglue-build.log 2>&1 \
  || { grep -E "error:|FAILED:" logs/rexglue-build.log | head -20; exit 1; }

echo "== Runtime (cross)"
cmake -S runtime -B build/runtime -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
  -DDARKNESS_LANGUAGE_PACK_URL= -DDARKNESS_LANGUAGE_PACK_SHA256= > logs/runtime-configure.log
cmake --build build/runtime > logs/runtime-build.log 2>&1 \
  || { grep -E "error:|FAILED:" logs/runtime-build.log | head -20; exit 1; }

echo "== Package"
rm -rf build/package.new
pwsh -NoProfile -File scripts/package.ps1 -Output build/package.new
ln -sfn "$game" build/package.new/game
# Played in place: keep settings, saves and logs in this folder
# (runtime/runtime_user_paths.h). The Windows installer leaves the marker out.
echo "Keep settings, saves and logs in this folder." > build/package.new/portable.txt
# Keep settings, saves, logs and caches from the previous package.
if [ -d build/package ]; then
  for keep in TheDarkness.pc.toml TheDarkness.mods.toml TheDarkness.game.toml runtime_data logs \
              vkd3d-proton.cache vkd3d-proton.cache.write; do
    [ -e "build/package/$keep" ] && mv "build/package/$keep" build/package.new/
  done
  rm -rf build/package
fi
mv build/package.new build/package
echo "Done: $repo/build/package  (play: linux/run-proton.sh)"
