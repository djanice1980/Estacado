#!/usr/bin/env bash
# Builds a CachyOS/Arch package (.pkg.tar.zst) from build/package
# (linux/build.sh). Output: build/arch. Install: sudo pacman -U <file>.
# The package installs to /opt/the-darkness-estacado with a `the-darkness`
# command and menu entries; see linux/arch/the-darkness for the runtimes.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
pkg="$repo/build/package"
out="$repo/build/arch"
[ -f "$pkg/TheDarkness.exe" ] || { echo "No package: run linux/build.sh first."; exit 1; }
command -v makepkg > /dev/null || { echo "makepkg not found (pacman's makepkg)."; exit 1; }

# Version: the changelog's release, the build date and this commit (pkgver
# has no '-').
release="$(grep -m1 -oE '^## [0-9]+\.[0-9]+\.[0-9]+' "$repo/CHANGELOG.md" | cut -c4-)"
pkgver="${release:-0.0.0}.$(date +%Y%m%d).g$(git -C "$repo" rev-parse --short HEAD)"
rm -rf "$out"
mkdir -p "$out/stage/estacado"

# The player files only: no game link, saves, settings, logs, caches or the
# portable marker (an installed copy keeps those in its Wine prefix).
tar -C "$pkg" -cf - \
  --exclude=./game --exclude=./logs --exclude=./runtime_data \
  --exclude='./vkd3d-proton.cache*' --exclude=./TheDarkness.pc.toml \
  --exclude=./TheDarkness.mods.toml --exclude=./TheDarkness.game.toml \
  --exclude=./portable.txt --exclude=./launcher_debug.txt --exclude=./bench.pc.toml \
  --exclude=./shot_script.txt . | tar -C "$out/stage/estacado" -xf -
tar -C "$out/stage" -cf "$out/estacado-$pkgver.tar" estacado
rm -rf "$out/stage"

cp "$repo/linux/arch/the-darkness" "$repo/linux/arch/the-darkness.desktop" \
   "$repo/linux/arch/the-darkness-settings.desktop" "$out/"
sed "s/@PKGVER@/$pkgver/" "$repo/linux/arch/PKGBUILD" > "$out/PKGBUILD"
sums=$(cd "$out" && for f in "estacado-$pkgver.tar" the-darkness the-darkness.desktop \
       the-darkness-settings.desktop; do printf "'%s' " "$(sha256sum "$f" | cut -d' ' -f1)"; done)
sed -i "s/@SHA256SUMS@/${sums% }/" "$out/PKGBUILD"

(cd "$out" && makepkg -f --noconfirm > makepkg.log 2>&1) || { tail -20 "$out/makepkg.log"; exit 1; }
ls -la "$out"/*.pkg.tar.zst
