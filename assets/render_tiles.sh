#!/usr/bin/env bash
# Render the app tiles from their SVG sources, and prove the result is installable.
#
# The Vita's package installer refuses a VPK whose tile PNGs are not 8-bit palette images —
# the install dies with 0x8010113D and the package says nothing about why. So this script is
# the only sanctioned way to make a tile: it renders, hands the result to
# scripts/convert_png.py, which decides the format and reads back what it wrote, and then
# verifies every committed tile. Neither step can leave a TrueColor PNG in the tree.
#
# Usage:
#   assets/render_tiles.sh           render from assets/*.svg, then verify every tile
#   assets/render_tiles.sh --check   verify every tile PNG only (run it after touching any)
#   QUANTIZER=pngquant assets/render_tiles.sh   use pngquant instead of Pillow
#
# Needs rsvg-convert (brew install librsvg) and uv. The client's
# sce_sys/livearea/contents/bg0.png has no SVG behind it yet (TASK-009): it is committed art,
# so it is verified and can be re-quantized, but it is not rendered here.
set -euo pipefail
cd "$(dirname "$0")/.."

QUANTIZER=${QUANTIZER:-pillow}
TILE_PNGS=(
  sce_sys/icon0.png
  sce_sys/livearea/contents/bg0.png
  sce_sys_sshd/icon0.png
  sce_sys_sshd/livearea/contents/bg0.png
)

for tool in rsvg-convert uv; do
  command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool (brew install librsvg; brew install uv)" >&2; exit 1; }
done

if [ "${1:-}" = "--check" ]; then
  exec uv run scripts/convert_png.py --check "${TILE_PNGS[@]}"
fi

# <svg source> <committed png> <width> <height>
TILES=(
  "assets/sshd-icon0.svg  sce_sys_sshd/icon0.png                 128 128"
  "assets/sshd-bg0.svg    sce_sys_sshd/livearea/contents/bg0.png 960 544"
)

WORK=$(mktemp -d -t tiles)
trap 'rm -rf "$WORK"' EXIT

for spec in "${TILES[@]}"; do
  set -- $spec
  src=$1 out=$2 w=$3 h=$4
  # rsvg-convert renders 24-bit RGB, which is the format that gets shipped by mistake; the
  # converter is what turns it into something the installer will take.
  rsvg-convert -w "$w" -h "$h" "$src" -o "$WORK/tile.png"
  uv run scripts/convert_png.py --quantizer "$QUANTIZER" --force --out "$out" "$WORK/tile.png"
done

uv run scripts/convert_png.py --check "${TILE_PNGS[@]}"
