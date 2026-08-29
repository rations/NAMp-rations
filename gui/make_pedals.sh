#!/usr/bin/env bash
# Rations — build the pedalboard's raster assets (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# Run once; the produced PNGs in resources/img are the committed assets the
# bundle ships. Nothing is generated at run time.
#
#   ./gui/make_pedals.sh
#
# Source art (resources/img/pedals, the author's own): pedal-boost-base.png and
# its four siblings, plus chrome-switch-on-off.png. As everywhere else in this
# pipeline the *-base.png files are the untrimmed exports and are deliberately
# NOT in the bundle's resource list — CMakeLists.txt ships the trimmed results.
#
# The five enclosures are BLANK: no knobs, no LED, no lettering. Everything on
# the face is drawn at run time over the blit, exactly the way the amp head is
# drawn over head.png, so a pedal's legends are Michroma at the current scale
# rather than pixels baked in at one size. resources/img/pedals also holds
# pedal-boost-mock.png and pedalboard-mock.png, which are DESIGN REFERENCES and
# not assets: the control positions in src/geometry.h were measured off the
# first of them by differencing it against the blank base.
#
# Not built here, and not needed: the knob and the two LEDs. The pedals reuse
# resources/img/dial.png, led_on.png and led_off.png — the very same files.
# resources/img/pedals held byte-identical copies of all three; they were
# deleted rather than shipped twice, because a second copy is a second thing to
# keep in step with make_assets.sh.
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

command -v magick >/dev/null || { echo "make_pedals: ImageMagick 7 (magick) not found" >&2; exit 1; }
mkdir -p "$OUT_DIR"

PEDAL_ART="$ART_DIR/pedals"

need() { [ -f "$1" ] || { echo "make_pedals: missing source art $1" >&2; exit 1; }; }

# Trim one source export to its own bounds and check the result against the size
# geometry.sh declares. The trim is what DECIDES the aspect every pedal is laid
# out against, so a re-export that moves a bbox must be noticed here rather than
# silently shifting every control on that pedal's face.
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
        echo "make_pedals: $what trims to ${bw}x${bh}, but geometry.sh says ${want_w}x${want_h}." >&2
        echo "             Update the sizes here and the matching constants in src/geometry.h." >&2
        exit 1
    fi
}

# ---- pedal-<name>.png: the five blank enclosures --------------------------
# All five are required to trim to the SAME size. That is not tidiness: one set
# of control positions in src/geometry.h is applied to all five faces, so a
# pedal whose body sat two pixels left of its siblings would draw its knobs two
# pixels off centre and nothing else would report it.
for name in $PEDAL_NAMES; do
    trim_checked "$PEDAL_ART/pedal-$name-base.png" "$OUT_DIR/pedal-$name.png" \
        "$PEDAL_SRC_W" "$PEDAL_SRC_H" "$name enclosure"
done

# ---- pedal_switch.png: the chrome footswitch cap --------------------------
# Same treatment, and for the same reason, as gui/make_knob.sh gives the dial:
# the cap sits off-centre on its export with a soft drop shadow below it, so a
# blit centred on the canvas would sit visibly off its own pivot and the shadow
# would draw as a hard straight cut where the crop clipped it.
#
# The mask is applied with -compose DstIn (multiply alpha), never CopyOpacity
# (replace alpha), so the cap's own antialiased rim survives; and DstIn takes
# the circle from the SOURCE'S ALPHA, hence `xc:none` plus a filled circle
# rather than the black/white greyscale a CopyOpacity mask would want.
# -draw "circle cx,cy px,py" takes a point ON the circle, not a radius.
SWITCH_SRC="$PEDAL_ART/chrome-switch-on-off.png"
need "$SWITCH_SRC"
half=$((PSWITCH_SIDE / 2))

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

magick -size ${PSWITCH_SIDE}x${PSWITCH_SIDE} xc:none -fill white \
    -draw "circle $half,$half $half,$((half - PSWITCH_MASK_R))" "PNG32:$tmp/mask.png"

magick "$SWITCH_SRC" \
    -crop ${PSWITCH_SIDE}x${PSWITCH_SIDE}+$((PSWITCH_CX - half))+$((PSWITCH_CY - half)) +repage \
    "$tmp/mask.png" -compose DstIn -composite \
    -resize ${PSWITCH_PX}x${PSWITCH_PX} \
    -strip -depth 8 "PNG32:$OUT_DIR/pedal_switch.png"

# ---- pedalboard-layout.png: a REFERENCE, not an asset ---------------------
# The board itself is drawn, not blitted: a dark ground, the two row legends and
# the patch cables between the jacks, with the five enclosures composited over
# it. So there is no board bitmap to ship, and the file written here exists only
# so the intended arrangement can be looked at. It is regenerated from the same
# geometry.sh numbers the editor lays out from, which is what makes it worth
# having: if the layout constants move, this moves with them.
#
# It supersedes pedals/pedalboard-mock.png, which is kept as the author's own
# source. That mock has the two rows at different scales, uneven row spacing,
# and the bottom row's outer jacks clipped by the canvas edge — none of which
# reaches the product, because the product never blits it.
LAYOUT_SCALE=2
lw=$((PEDAL_PAGE_W * LAYOUT_SCALE)); lh=$((PEDAL_PAGE_H * LAYOUT_SCALE))
pw=$((PEDAL_W * LAYOUT_SCALE));      ph=$((PEDAL_H * LAYOUT_SCALE))
FONT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/resources/fonts/Michroma-Regular.ttf"

place() { # name row-top-y centre-x  -> one -draw'able composite argument set
    printf ' %s -geometry %dx%d+%d+%d -composite' \
        "$OUT_DIR/pedal-$2.png" "$pw" "$ph" \
        "$(( ($3 - PEDAL_W / 2) * LAYOUT_SCALE ))" "$(( $4 * LAYOUT_SCALE ))"
}

# PRE: two pedals, centred as a pair. POST: three, centred as a triple.
args=""
args+="$(place x boost   $PEDAL_PRE_CX0  $PEDAL_ROW1_Y)"
args+="$(place x chorus  $PEDAL_PRE_CX1  $PEDAL_ROW1_Y)"
args+="$(place x flanger $PEDAL_POST_CX0 $PEDAL_ROW2_Y)"
args+="$(place x delay   $PEDAL_POST_CX1 $PEDAL_ROW2_Y)"
args+="$(place x reverb  $PEDAL_POST_CX2 $PEDAL_ROW2_Y)"

# shellcheck disable=SC2086 -- args is a deliberately word-split argument list
magick -size ${lw}x${lh} xc:"#101010" $args \
    -font "$FONT" -fill "#9a9a9a" -pointsize $((11 * LAYOUT_SCALE)) \
    -annotate +$((24 * LAYOUT_SCALE))+$(( (PEDAL_ROW1_Y - 8) * LAYOUT_SCALE )) 'PRE' \
    -annotate +$((24 * LAYOUT_SCALE))+$(( (PEDAL_ROW2_Y - 8) * LAYOUT_SCALE )) 'POST' \
    -strip -depth 8 "PNG32:$PEDAL_ART/pedalboard-layout.png"

for name in $PEDAL_NAMES; do
    magick identify "$OUT_DIR/pedal-$name.png"
done
magick identify "$PEDAL_ART/pedalboard-layout.png"
magick identify "$OUT_DIR/pedal_switch.png"
echo "Wrote five enclosures at ${PEDAL_SRC_W}x${PEDAL_SRC_H} (drawn at ${PEDAL_W}x${PEDAL_H})"
echo "and pedal_switch.png, square and centred on the cap."
