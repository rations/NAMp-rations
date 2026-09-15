#!/usr/bin/env bash
# Rations — generate the level-meter track (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# A recessed vertical slot with a gold hairline, matching the toggle escutcheon
# and the faceplate piping. This replaces the sibling plug-in's upstream
# MeterBackground.png, which is a flat mid-grey panel: correct against that
# plug-in's raisin-black backdrop, wrong against gold-on-black tolex.
#
# The editor still draws a flat rect when this file is missing, so the asset is
# an improvement on the fallback rather than a dependency of it.
#
# Uses the same layer idiom as make_switch.sh: shape mask -> CopyOpacity for the
# body, thick blurred stroke -> CopyOpacity for the inner shadow.
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

W=$METER_W    # 80
H=$METER_H    # 408
R=14          # corner radius

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Body: near-black, very slightly lighter at the top so the slot reads as lit
# from above rather than as a hole punched in the panel.
magick -size ${W}x${H} \
  -define gradient:center=40,0 -define gradient:radii=420,420 \
  radial-gradient:'#1b1a19'-'#070707' \
  \( -size ${W}x${H} xc:black -fill white \
     -draw "roundrectangle 2,2 $((W - 3)),$((H - 3)) $R,$R" \) \
  -alpha off -compose CopyOpacity -composite \
  "PNG32:$tmp/body.png"

magick -size ${W}x${H} xc:none \
  -fill none -stroke black -strokewidth 10 \
  -draw "roundrectangle 2,2 $((W - 3)),$((H - 3)) $R,$R" -blur 0x5 \
  \( -size ${W}x${H} xc:black -fill white \
     -draw "roundrectangle 2,2 $((W - 3)),$((H - 3)) $R,$R" \) \
  -alpha off -compose CopyOpacity -composite \
  "PNG32:$tmp/inner.png"

magick "$tmp/body.png" \
  "$tmp/inner.png" -compose over -composite \
  -fill none -stroke "$GOLD" -strokewidth 3 \
  -draw "roundrectangle 3,3 $((W - 4)),$((H - 4)) $((R - 1)),$((R - 1))" \
  -strip -depth 8 "PNG32:$OUT_DIR/meter_track.png"

magick identify "$OUT_DIR/meter_track.png"
echo "Wrote meter_track.png."
