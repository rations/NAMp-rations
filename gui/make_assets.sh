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
