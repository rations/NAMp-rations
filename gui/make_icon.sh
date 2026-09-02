#!/usr/bin/env bash
# Rations — generate the application icon for the Linux standalone (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# The icon is the amp head with its name printed on the faceplate, drawn in the panel's own face:
# resources/img/icon-base.png is a bare head — no wordmark, because the wordmark on the real panel
# is Michroma drawn at run time and does not exist as a picture anywhere — and this script prints
# "Rations Amp" onto it in white Michroma and cuts the icon sizes from the result.
#
# NOTHING HERE IS A GUESSED COORDINATE. The faceplate is found by looking for the gold piping that
# frames it, the point size is solved from the font's own metrics against that panel's width, and
# the result is checked to sit inside the panel with a margin before it is written. If the base art
# is ever redrawn, the text follows it; if it is redrawn into something this cannot read, the script
# fails loudly instead of printing the name over the piping.
#
# Only the standalone needs this. The VST3 bundle carries no icon — a plug-in is drawn by its host —
# and icon-base.png is source art like the other *-base.png files here: it is not in the bundle's
# resource list and is not shipped.
set -euo pipefail
cd "$(dirname "$0")"

REPO="$(cd .. && pwd)"
BASE="$REPO/resources/img/icon-base.png"
FONT="$REPO/resources/fonts/Michroma-Regular.ttf"
OUT="$REPO/packaging/icons"
TEXT="${RATIONS_ICON_TEXT:-Rations Amp}"

# The text fills this much of the faceplate. Width is what constrains it on a panel this shape;
# the height cap is there so a shorter name cannot grow into the piping above and below.
WIDTH_FRACTION=88
HEIGHT_FRACTION=60
MIN_MARGIN=4 # px of faceplate that must remain clear on every side, at the base art's own scale

for f in "$BASE" "$FONT"; do
  [ -f "$f" ] || { echo "make_icon.sh: $f is missing" >&2; exit 1; }
done
command -v magick >/dev/null || { echo "make_icon.sh: ImageMagick 7 (magick) not found" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# --- find the faceplate -----------------------------------------------------
# The head is a black cab with gold piping: an outer edge, and an inner rectangle framing the
# faceplate. Scanning the middle row and the middle column for gold finds both, and the INNERMOST
# pair on each axis is the faceplate the name goes on.
magick "$BASE" -depth 8 rgb:- > "$tmp/base.rgb"
read -r PANEL_X PANEL_Y PANEL_W PANEL_H TOLEX < <(
  BASE_RGB="$tmp/base.rgb" BASE_W="$(magick "$BASE" -format '%w' info:)" \
  BASE_H="$(magick "$BASE" -format '%h' info:)" python3 - <<'PYEOF'
import os, sys

W = int(os.environ["BASE_W"])
H = int(os.environ["BASE_H"])
data = open(os.environ["BASE_RGB"], "rb").read()

def px(x, y):
    i = (y * W + x) * 3
    return data[i], data[i + 1], data[i + 2]

# "Gold" as the piping actually is, rather than as a named colour: much more red than blue, and
# bright enough not to be the tolex weave or the grille cloth.
def gold(p):
    r, g, b = p
    return r > 150 and g > 120 and b < 160 and r > b + 40

def runs(values):
    out, start = [], None
    for v in values:
        if start is None:
            start = prev = v
        elif v == prev + 1:
            prev = v
        else:
            out.append((start, prev))
            start = prev = v
    if start is not None:
        out.append((start, prev))
    return out

mid_y, mid_x = H // 2, W // 2
h_runs = runs([x for x in range(W) if gold(px(x, mid_y))])
v_runs = runs([y for y in range(H) if gold(px(mid_x, y))])
if len(h_runs) < 2 or len(v_runs) < 2:
    sys.exit("make_icon.sh: no gold piping found in the base art - is this a Rations head?")

# The innermost run on each side: the last one before the middle, and the first one after it.
left = max(r for r in h_runs if r[1] < mid_x)[1]
right = min((r for r in h_runs if r[0] > mid_x), key=lambda r: r[0])[0]
top = max(r for r in v_runs if r[1] < mid_y)[1]
bottom = min((r for r in v_runs if r[0] > mid_y), key=lambda r: r[0])[0]

x, y = left + 1, top + 1
w, h = right - left - 1, bottom - top - 1
if w < 40 or h < 20:
    sys.exit(f"make_icon.sh: the faceplate came out {w}x{h}, which cannot be right")

# The tolex, for the tile behind the icon: sampled between the two pipings on the left.
tolex = px(max(0, left - 6), mid_y)
print(x, y, w, h, "rgb(%d,%d,%d)" % tolex)
PYEOF
)
echo "make_icon.sh: faceplate ${PANEL_W}x${PANEL_H} at +${PANEL_X}+${PANEL_Y}, tolex $TOLEX"

# --- size the name to the faceplate ----------------------------------------
# Font metrics scale linearly with the point size, so one measurement at 100 solves it exactly.
BASE_W="$(magick "$BASE" -format '%w' info:)"
BASE_H="$(magick "$BASE" -format '%h' info:)"
read -r REF_W REF_H < <(magick -font "$FONT" -pointsize 100 label:"$TEXT" \
  -format '%w %h\n' info:)
POINTSIZE="$(python3 -c "
ref_w, ref_h = $REF_W, $REF_H
by_w = 100.0 * $WIDTH_FRACTION / 100.0 * $PANEL_W / ref_w
by_h = 100.0 * $HEIGHT_FRACTION / 100.0 * $PANEL_H / ref_h
print(round(min(by_w, by_h), 1))
")"
read -r TEXT_W TEXT_H < <(magick -font "$FONT" -pointsize "$POINTSIZE" label:"$TEXT" \
  -format '%w %h\n' info:)

# THE AUDIT, and it checks the thing that can actually go wrong. Overflowing the faceplate cannot:
# the point size is solved to fit, so a name too long for the panel comes out SMALL rather than
# over the piping - which is the real failure, because the smallest icon is 48 px and the head is
# scaled to its width there. So what is checked is how tall the name ends up on that icon. The
# margin check is kept beside it for the pixel or two that rounding can add.
#
# 4 px is where a word stops being a word and becomes a grey smudge. "Rations Amp" lands at 6.6.
MIN_INK_PX=4
SMALLEST_ICON=48
python3 -c "
scale = $SMALLEST_ICON / float($BASE_W)
ink = $TEXT_H * scale
if $TEXT_W > $PANEL_W - 2 * $MIN_MARGIN or $TEXT_H > $PANEL_H - 2 * $MIN_MARGIN:
    raise SystemExit(
        'make_icon.sh: \"$TEXT\" lays out ${TEXT_W}x${TEXT_H} at ${POINTSIZE}pt, which does not '
        'leave ${MIN_MARGIN}px inside a ${PANEL_W}x${PANEL_H} faceplate')
if ink < $MIN_INK_PX:
    raise SystemExit(
        'make_icon.sh: \"$TEXT\" only fits the faceplate at ${POINTSIZE}pt, which is %.1f px tall '
        'on the ${SMALLEST_ICON}px icon and unreadable there (%d px is the floor). Use a shorter '
        'name.' % (ink, $MIN_INK_PX))
print('make_icon.sh: \"$TEXT\" at ${POINTSIZE}pt is ${TEXT_W}x${TEXT_H}, %.1f px tall on the '
      '${SMALLEST_ICON}px icon' % ink)
"

# Centred on the FACEPLATE, which is not quite the centre of the picture: the cab has more below
# the panel than above it.
OFFSET_X="$(python3 -c "print(int(round(($PANEL_X + $PANEL_W / 2.0) - $BASE_W / 2.0)))")"
OFFSET_Y="$(python3 -c "print(int(round(($PANEL_Y + $PANEL_H / 2.0) - $BASE_H / 2.0)))")"

magick "$BASE" -font "$FONT" -pointsize "$POINTSIZE" -fill white \
  -gravity center -annotate "+${OFFSET_X}+${OFFSET_Y}" "$TEXT" \
  "PNG32:$tmp/head.png"

# --- cut the icon sizes -----------------------------------------------------
# The head is far wider than it is tall and an icon is square, so it sits on a rounded tile of the
# cab's own tolex rather than floating on transparency, where at 48 px it would be a thin smear.
#
# FULL BLEED ACROSS THE TILE, decided by looking at 48-pixel candidates rather than by taste: the
# head inset to 92% of the width lost enough of the name to matter at 64 px and at 48 px, and the
# tile's corners are pure tolex either way because the head occupies about a third of the height.
mkdir -p "$OUT"
for SIZE in 256 128 64 48; do
  INNER=$SIZE
  RADIUS=$((SIZE * 12 / 100))
  magick \
    \( -size ${SIZE}x${SIZE} xc:none \
       -fill "$TOLEX" -draw "roundrectangle 0,0 $((SIZE - 1)),$((SIZE - 1)) $RADIUS,$RADIUS" \) \
    \( "$tmp/head.png" -resize ${INNER}x \) \
    -gravity center -compose over -composite \
    "PNG32:$OUT/rations-${SIZE}.png"
done

echo "make_icon.sh: wrote $OUT/rations-{256,128,64,48}.png"
