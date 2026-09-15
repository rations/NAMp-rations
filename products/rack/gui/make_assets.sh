#!/usr/bin/env bash
# Rations — build every raster asset the editor loads (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# Run once; the produced PNGs in resources/img are the committed assets the
# bundle ships. Nothing is generated at run time.
#
#   ./gui/make_assets.sh
#
# Source art (resources/img, the author's own): head-base.png, cabinet-base.png,
# dial-base.png, led-on-base.png, led-off-base.png. The *-base.png files are the
# untrimmed exports and are deliberately NOT in the bundle's resource list —
# CMakeLists.txt ships the trimmed and generated results only. Overridable with
# ART_DIR / OUT_DIR.
#
# There is no badge step: the "Rations" wordmark is drawn as text in Michroma at
# run time rather than exported as a raster layer.
#
# The vector icons are NOT built here — they are small MIT SVGs committed to
# resources/img and rasterised at run time by NanoSVG. Set NAM_UPSTREAM to a
# checkout of NeuralAmpModelerPlugin (github.com/sdatkinson/NeuralAmpModelerPlugin,
# MIT) to refresh the five that come from it. Folder.svg is the author's own, and
# SlimmableIcon.svg is upstream's shape recoloured to this panel's palette — a
# derived file, so it is NOT refreshed by the loop below and a copy from upstream
# would silently put the sibling plug-in's azure back on this faceplate.
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

command -v magick >/dev/null || { echo "make_assets: ImageMagick 7 (magick) not found" >&2; exit 1; }
mkdir -p "$OUT_DIR"

need() { [ -f "$1" ] || { echo "make_assets: missing source art $1" >&2; exit 1; }; }

# Trim one source export to its own bounds and check the result against the size
# geometry.sh declares. The trim is what DECIDES a canvas dimension, so a source
# re-export that moves the bbox must be noticed here rather than silently
# shifting every control that is placed against it.
trim_checked() {
    local src="$1" out="$2" want_w="$3" want_h="$4" what="$5"
    need "$src"
    magick "$src" -fuzz 6% -trim +repage -strip -depth 8 "PNG32:$out"
    # `read` returns 1 at EOF even after assigning, and identify emits no
    # trailing newline, so this must not be a bare `read` under `set -e`.
    local bbox bw bh
    bbox="$(magick identify -format "%w %h" "$out")"
    bw="${bbox% *}"; bh="${bbox#* }"
    if [ "$bw" != "$want_w" ] || [ "$bh" != "$want_h" ]; then
        echo "make_assets: $what trims to ${bw}x${bh}, but geometry.sh says ${want_w}x${want_h}." >&2
        echo "             Update the sizes here and the matching constants in src/geometry.h." >&2
        exit 1
    fi
}

# ---- head.png: the amp head, trimmed to its own bounds ---------------------
# The trim decides the logical canvas (WIN_W x WIN_H in geometry.sh).
trim_checked "$ART_DIR/head-base.png" "$OUT_DIR/head.png" "$HEAD_W" "$HEAD_H" "head art"

# ---- cabinet.png: the speaker cabinet, trimmed to its own bounds -----------
# Not resized here. The cabinet page scales it down at draw time, and how far
# depends on how much room the two IR loader rows take — a decision that belongs
# with the layout in src/geometry.h, not baked into a stored asset that would
# then have to be regenerated to move a row.
trim_checked "$ART_DIR/cabinet-base.png" "$OUT_DIR/cabinet.png" "$CAB_W" "$CAB_H" "cabinet art"

# ---- namp-badge.png: the NAMp wordmark above "Rations" on the head --------
# The head's title is a BADGE over text rather than text alone. The badge is the
# parent project's own gold NAMp mark, and it arrives with a real alpha channel
# and a very wide, very faint bloom around the ink -- a plain -trim keeps almost
# the whole 1536x1024 canvas, because that bloom never quite reaches zero.
#
# So the crop is taken from the alpha channel at a THRESHOLD rather than from
# -trim: everything at 5% opacity or more is ink or its hard shadow, and what is
# below that is bloom nobody can see on a dark faceplate. A small margin is put
# back so the shadow does not end on a hard edge. Derived rather than hard-coded,
# so replacing the source art re-derives the crop instead of silently cutting it
# in the wrong place.
#
# Stored at 512 wide against the 162 it is drawn at (src/geometry.h kBadgeW), so
# it stays clean at kScaleMax on a HiDPI display, where the static layer is
# composited at device resolution.
need "$ART_DIR/namp-badge-base.png"
# %@ is the bounding box of the non-zero region; %wx%h would be the canvas,
# which is the whole 1536x1024 and would crop nothing at all.
BADGE_BOX="$(magick "$ART_DIR/namp-badge-base.png" -alpha extract -threshold 5% \
             -format "%@" info:)"
if [ -z "$BADGE_BOX" ]; then
    echo "make_assets: could not find any ink in namp-badge-base.png" >&2
    exit 1
fi
magick "$ART_DIR/namp-badge-base.png" -crop "$BADGE_BOX" +repage \
       -bordercolor none -border 12 \
       -resize "${BADGE_W}x" -strip -depth 8 "PNG32:$OUT_DIR/namp-badge.png"
badge_size="$(magick identify -format "%w %h" "$OUT_DIR/namp-badge.png")"
badge_h="${badge_size#* }"
if [ "$badge_h" != "$BADGE_H" ]; then
    echo "make_assets: the badge stores as ${badge_size% *}x${badge_h}, but geometry.sh says" >&2
    echo "             ${BADGE_W}x${BADGE_H}. The source art's proportions have changed, so the" >&2
    echo "             drawn size in src/geometry.h (kBadgeW/kBadgeH) needs re-deriving too." >&2
    exit 1
fi

# ---- LEDs: already 128x128 and centred, so only re-encoded -----------------
need "$ART_DIR/led-on-base.png"
need "$ART_DIR/led-off-base.png"
magick "$ART_DIR/led-on-base.png"  -strip -depth 8 "PNG32:$OUT_DIR/led_on.png"
magick "$ART_DIR/led-off-base.png" -strip -depth 8 "PNG32:$OUT_DIR/led_off.png"

# ---- generated art --------------------------------------------------------
./make_knob.sh
./make_switch.sh
./make_meter.sh

# ---- vector icons (optional refresh) --------------------------------------
if [ -n "${NAM_UPSTREAM:-}" ]; then
    # Gear.svg is still refreshed with the set even though nothing draws it any
    # more: the settings control is a labelled button now, and keeping the
    # upstream icon set whole costs nothing and leaves it there if it is wanted.
    for i in Gear File Cross ArrowLeft ArrowRight; do
        src="$NAM_UPSTREAM/NeuralAmpModeler/resources/img/$i.svg"
        [ -f "$src" ] || { echo "make_assets: missing $src" >&2; exit 1; }
        cp "$src" "$OUT_DIR/$i.svg"
    done
    echo "Refreshed 5 upstream icons from \$NAM_UPSTREAM."
    echo "SlimmableIcon.svg is deliberately NOT among them: it is recoloured, not copied."
fi

echo
magick identify "$OUT_DIR"/*.png
echo
echo "Wrote $(ls -1 "$OUT_DIR" | wc -l) files to $OUT_DIR."
