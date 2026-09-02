#!/usr/bin/env bash
# Rations — generate the application icon for the Linux standalone (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# The icon is RENDERED, not drawn. There is no badge asset in this tree and there is deliberately
# not going to be one: the wordmark is Michroma text drawn at run time, so the only place the
# faceplate exists as a picture is a render of it. panelrender draws exactly what the editor draws,
# which is why it is the source here — an icon hand-cut from the art would go stale the first time
# the panel moved.
#
# SHAPE, and it was settled by looking at 48-pixel candidates rather than by argument. The head is
# 2.81:1 and an icon is 1:1, so something has to give. The whole head letterboxed into a square is
# a thin smear with dead space above and below it, and a square crop of the dials says "a knob"
# rather than "Rations". What survives being shrunk is the WORDMARK band - the centre of the
# faceplate, gold rail above and below, the top of the channel dials - which still reads as the
# word at 64 px and as a gold-railed black plate at 48.
#
# The crop is written as FRACTIONS of the render rather than as pixels, so it stays centred on the
# wordmark if the head page ever changes size.
#
# Only the standalone needs this. The VST3 bundle carries no icon — a plug-in is drawn by its host.
set -euo pipefail
cd "$(dirname "$0")"

REPO="$(cd .. && pwd)"
BUILD="${RATIONS_BUILD_DIR:-$REPO/build}"
OUT="$REPO/packaging/icons"

PANELRENDER="$BUILD/panelrender"
if [ ! -x "$PANELRENDER" ]; then
  echo "make_icon.sh: no panelrender at $PANELRENDER" >&2
  echo "  cmake -S \"$REPO\" -B \"$BUILD\" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build \"$BUILD\"" >&2
  exit 1
fi
command -v magick >/dev/null || { echo "make_icon.sh: ImageMagick 7 (magick) not found" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Every page, because that is panelrender's contract; only the head is used.
"$PANELRENDER" "$tmp/page" "$REPO/resources" 1.0 >/dev/null
HEAD="$tmp/page-head.png"
[ -f "$HEAD" ] || { echo "make_icon.sh: panelrender produced no head page" >&2; exit 1; }

# The tile's ground is taken FROM the art rather than named here, so a repaint of the cabinet
# carries into the icon instead of leaving it the old colour.
GROUND="$(magick "$HEAD" -format '%[pixel:p{4,4}]' info:)"

HEAD_W="$(magick "$HEAD" -format '%w' info:)"
HEAD_H="$(magick "$HEAD" -format '%h' info:)"
CROP_W=$((HEAD_W * 42 / 100))
CROP_H=$((HEAD_H * 82 / 100))
CROP_X=$(((HEAD_W - CROP_W) / 2))
CROP_Y=$((HEAD_H * 11 / 100))

mkdir -p "$OUT"
for SIZE in 256 128 64 48; do
  INNER=$((SIZE * 92 / 100))
  RADIUS=$((SIZE * 12 / 100))
  magick \
    \( -size ${SIZE}x${SIZE} xc:none \
       -fill "$GROUND" -draw "roundrectangle 0,0 $((SIZE - 1)),$((SIZE - 1)) $RADIUS,$RADIUS" \) \
    \( "$HEAD" -crop ${CROP_W}x${CROP_H}+${CROP_X}+${CROP_Y} +repage -resize ${INNER}x \) \
    -gravity center -compose over -composite \
    "PNG32:$OUT/rations-${SIZE}.png"
done

echo "make_icon.sh: wrote $OUT/rations-{256,128,64,48}.png on $GROUND"
