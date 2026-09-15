#!/usr/bin/env bash
# Rations — square, centred knob asset for clean rotary rotation.
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# Adapted from the author's own make_knob.sh in a sibling stompbox project. That
# copy carries a GPL-3.0-or-later header only because the project around it links
# JUCE; this script contains no JUCE reference, includes nothing and derives from
# nothing GPL, so the author has relicensed their own work MIT here. The
# rationale is recorded in NOTICE.
#
# Why this step is needed at all: dial-base.png is a 1536x1024 canvas with the
# knob sitting off-centre (body centre (770, 488), not (768, 512)) and a soft
# drop shadow trailing below it. Rotating that bitmap about its canvas centre
# makes the knob orbit instead of spin, and drags the shadow around with it.
#
# So: crop a square centred on the knob BODY, then multiply the alpha by a
# circular mask. The mask is what removes the drop shadow -- clipping it with the
# crop alone would leave a straight cut across a half-opaque gradient, which
# reads as a hard line under rotation. A knob has no shadow as a result, which is
# the right trade: a shadow that rotates with the knob is worse than none.
#
# The body's own antialiased edge must survive, so the mask is applied with
# -compose DstIn (multiply alpha), never CopyOpacity (replace alpha).
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

SRC="$ART_DIR/dial-base.png"
[ -f "$SRC" ] || { echo "make_knob: missing $SRC" >&2; exit 1; }

# Knob body extents in the source, measured with an alpha>200 bounding box:
#   x 442..1098 (centre 770), y 162..814 (centre 488), so radius <= 328.
CX=770; CY=488
SIDE=664                 # 2*332: an inscribed circle of r=332 never clips
HALF=$((SIDE / 2))
MASK_R=331               # >= 328 (content) and < 332 (half-side)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Antialiased circular mask. DstIn multiplies the destination's alpha by the
# SOURCE'S ALPHA, so the mask must carry the circle in its alpha channel -- hence
# xc:none plus a filled circle, not the black/white greyscale a CopyOpacity mask
# would use. (And no -alpha off anywhere: as a sequence operator it would strip
# the alpha off the dial as well, leaving an opaque square.)
# -draw "circle cx,cy px,py" takes a point ON the circle, not a radius.
magick -size ${SIDE}x${SIDE} xc:none -fill white \
  -draw "circle $HALF,$HALF $HALF,$((HALF - MASK_R))" "PNG32:$tmp/mask.png"

magick "$SRC" \
  -crop ${SIDE}x${SIDE}+$((CX - HALF))+$((CY - HALF)) +repage \
  "$tmp/mask.png" -compose DstIn -composite \
  -resize ${DIAL_PX}x${DIAL_PX} \
  -strip -depth 8 "PNG32:$OUT_DIR/dial.png"

magick identify "$OUT_DIR/dial.png"
echo "Wrote dial.png (square, centred on the knob body, shadow removed)."
