#!/usr/bin/env bash
# NAMp Rations — generate the application icon for the Linux standalone (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# The icon is the amp head wearing its own wordmark, drawn in the panel's own face:
# resources/img/icon-base.png is a bare head — no wordmark, because the panel's wordmark is
# composed at run time and does not exist as one picture anywhere — and this script stacks the
# NAMp badge over "Rations" onto its faceplate, exactly as src/rationsview.cpp draws them, then
# cuts the icon sizes from the result.
#
# THE STACK IS THE PANEL'S, IN THE PANEL'S PROPORTIONS. The badge sits above the word and is the
# wider of the two, and the ratio between them here is read off geometry.h rather than invented:
# kBadgeW is 161 units against the word's ~135 at kTitleSize, so the word is TEXT_WIDTH_OF_BADGE
# per cent of the badge's width. What the icon does NOT copy is the panel's tuck — there the cap
# top rises slightly INTO the badge's box to bind the two together, which at 48 px would smear
# them into one mark, so here they are separated by a small positive gap.
#
# NOTHING HERE IS A GUESSED COORDINATE. The faceplate is found by looking for the gold piping that
# frames it, both elements are measured at their own ink rather than at their canvas or their font
# box, the point size is solved from the font's own metrics against that panel, and the result is
# checked to sit inside the panel with a margin AND to still be readable at 48 px before it is
# written. If the base art is ever redrawn, the stack follows it; if it is redrawn into something
# this cannot read, the script fails loudly instead of printing over the piping.
#
# Only the standalone needs this. The VST3 bundle carries no icon — a plug-in is drawn by its host —
# and icon-base.png is source art like the other *-base.png files here: it is not in the bundle's
# resource list and is not shipped. namp-badge.png IS in the bundle, because the editor draws it.
set -euo pipefail
cd "$(dirname "$0")"

REPO="$(cd .. && pwd)"
BASE="$REPO/resources/img/icon-base.png"
BADGE="$REPO/resources/img/namp-badge.png"
FONT="$REPO/resources/fonts/Michroma-Regular.ttf"
OUT="$REPO/packaging/icons"
TEXT="${RATIONS_ICON_TEXT:-Rations}"

# THE SPLIT IS THE ICON'S OWN, and that is a measurement rather than a preference. On the panel the
# badge is 161 units wide against the word's ~135 at kTitleSize (src/geometry.h), a block of about
# 2.6:1 - and this faceplate is 3.7:1, so a block in the panel's proportions is limited by HEIGHT
# here and comes out small. Held to the panel's ratio the word lands 3.2 px tall on the 48 px icon,
# under the 4 px floor below and under the 4.25 px the previous icon achieved. So the height is
# split directly instead: the badge stays the larger mark, and the word gets enough of the panel to
# still be a word at the smallest size. Widths follow from each element's own ink aspect.
BADGE_HEIGHT_SHARE=52 # of the usable faceplate height
GAP_SHARE=7           # between the badge's ink and the word's cap top; the rest is the word
MIN_MARGIN=4          # px of faceplate that must remain clear on every side, at the base art's scale

for f in "$BASE" "$BADGE" "$FONT"; do
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
    sys.exit("make_icon.sh: no gold piping found in the base art - is this a NAMp Rations head?")

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

# --- lay the stack out on the faceplate ------------------------------------
# Everything is measured at its INK. The badge PNG carries a transparent margin (make_assets.sh
# borders it before the resize) and a font's label box carries ascender and descender space the
# word "Rations" does not use, so sizing either one by its canvas would leave the stack floating
# inside a box of nothing and would make the two elements disagree about what "the same width"
# means. -trim on the label and an alpha bounding box on the badge give the real marks.
BASE_W="$(magick "$BASE" -format '%w' info:)"
BASE_H="$(magick "$BASE" -format '%h' info:)"
read -r BADGE_INK_W BADGE_INK_H BADGE_INK_X BADGE_INK_Y < <(
  magick "$BADGE" -alpha extract -threshold 5% -format '%@\n' info: | sed 's/[x+]/ /g')
read -r REF_W REF_H < <(magick -font "$FONT" -pointsize 100 -background none -fill white \
  label:"$TEXT" -trim +repage -format '%w %h\n' info:)

# Solve the whole stack at once: the usable height is split three ways, each element's width comes
# from its own ink aspect, and if either then overruns the panel's width they are ALL scaled by the
# same factor - so the stack shrinks to fit rather than rearranging itself, and a wider word can
# never quietly squeeze the badge.
read -r BADGE_W BADGE_H POINTSIZE TEXT_W TEXT_H GAP STACK_H < <(python3 -c "
badge_aspect = $BADGE_INK_W / float($BADGE_INK_H)
text_aspect  = $REF_W / float($REF_H)
avail_w = $PANEL_W - 2 * $MIN_MARGIN
avail_h = $PANEL_H - 2 * $MIN_MARGIN

badge_h = avail_h * $BADGE_HEIGHT_SHARE / 100.0
gap     = avail_h * $GAP_SHARE / 100.0
text_h  = avail_h - badge_h - gap
badge_w = badge_h * badge_aspect
text_w  = text_h * text_aspect

k = min(1.0, avail_w / badge_w, avail_w / text_w)
badge_w, badge_h, text_w, text_h, gap = (v * k for v in (badge_w, badge_h, text_w, text_h, gap))

print(int(round(badge_w)), int(round(badge_h)),
      round(100.0 * text_w / $REF_W, 1), int(round(text_w)), int(round(text_h)),
      int(round(gap)), int(round(badge_h + gap + text_h)))
")
echo "make_icon.sh: badge ${BADGE_W}x${BADGE_H}, \"$TEXT\" ${TEXT_W}x${TEXT_H} at ${POINTSIZE}pt," \
     "gap ${GAP}, stack ${STACK_H} tall in a ${PANEL_W}x${PANEL_H} faceplate"

# THE AUDIT, and it checks the thing that can actually go wrong. Overflowing the faceplate cannot:
# the stack is solved to fit, so a wordmark too big for the panel comes out SMALL rather than over
# the piping - which is the real failure, because the smallest icon is 48 px and the head is scaled
# to its width there. So what is checked is how tall each element ends up on that icon. The margin
# check is kept beside it for the pixel or two that rounding can add.
#
# 4 px is where a word stops being a word and becomes a grey smudge, and it is measured at INK - not
# at the font's box, which is what the previous version of this script compared against and which
# is half again as tall. Re-measured at ink, the icon this replaces put "Rations Amp" at 4.25 px, so
# the floor is where it always was rather than lowered to let this design through. Both elements are
# checked, not just the smaller: the badge carries lettering of its own, and it is the new one.
MIN_INK_PX=4
SMALLEST_ICON=48
python3 -c "
scale = $SMALLEST_ICON / float($BASE_W)
text_ink = $TEXT_H * scale
badge_ink = $BADGE_H * scale
widest = max($BADGE_W, $TEXT_W)
if widest > $PANEL_W - 2 * $MIN_MARGIN or $STACK_H > $PANEL_H - 2 * $MIN_MARGIN:
    raise SystemExit(
        'make_icon.sh: the stack lays out %dx${STACK_H}, which does not leave ${MIN_MARGIN}px '
        'inside a ${PANEL_W}x${PANEL_H} faceplate' % widest)
for name, ink in (('the badge', badge_ink), ('\"$TEXT\"', text_ink)):
    if ink < $MIN_INK_PX:
        raise SystemExit(
            'make_icon.sh: %s is only %.1f px tall on the ${SMALLEST_ICON}px icon and unreadable '
            'there (%d px is the floor)' % (name, ink, $MIN_INK_PX))
print('make_icon.sh: on the ${SMALLEST_ICON}px icon the badge is %.1f px tall and \"$TEXT\" is '
      '%.1f px' % (badge_ink, text_ink))
"

# --- compose ----------------------------------------------------------------
# Absolute pixel offsets and two -composite calls rather than -gravity/-annotate: annotate places
# text by the gravity's own rule about baselines and boxes, which is one more thing to be sure of,
# and the word has already been measured at its ink. Both pieces are trimmed to that ink first, so
# an offset means what it says.
STACK_X="$(python3 -c "print(int(round($PANEL_X + ($PANEL_W - $BADGE_W) / 2.0)))")"
STACK_Y="$(python3 -c "print(int(round($PANEL_Y + ($PANEL_H - $STACK_H) / 2.0)))")"
TEXT_X="$(python3 -c "print(int(round($PANEL_X + ($PANEL_W - $TEXT_W) / 2.0)))")"
TEXT_Y="$(python3 -c "print($STACK_Y + $BADGE_H + $GAP)")"

magick "$BADGE" -crop "${BADGE_INK_W}x${BADGE_INK_H}+${BADGE_INK_X}+${BADGE_INK_Y}" +repage \
  -resize "${BADGE_W}x${BADGE_H}!" "PNG32:$tmp/badge.png"
magick -font "$FONT" -pointsize "$POINTSIZE" -background none -fill white label:"$TEXT" \
  -trim +repage -resize "${TEXT_W}x${TEXT_H}!" "PNG32:$tmp/text.png"

magick "$BASE" \
  "$tmp/badge.png" -geometry "+${STACK_X}+${STACK_Y}" -composite \
  "$tmp/text.png" -geometry "+${TEXT_X}+${TEXT_Y}" -composite \
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
    "PNG32:$OUT/namp-rations-${SIZE}.png"
done

echo "make_icon.sh: wrote $OUT/namp-rations-{256,128,64,48}.png"
