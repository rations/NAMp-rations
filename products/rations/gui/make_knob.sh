#!/usr/bin/env bash
# Rations — the dial art, drawn from nothing (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# Writes one 256x256 bitmap per dial style the editor offers:
#
#   dial.png           Black    knurled black grip, gold ring, spun black face, gold wedge
#   dial_gold.png      Gold     spun gold face, black knurled grip, black line
#   dial_pointer.png   Pointer  a black pointer knob with a gold edge, spine and hub
#   dial_pedal.png     (only where geometry.sh sets DIAL_PEDAL) the pointer cut for a
#                      coloured pedal face: no gold edge, since the enclosure outlines it
#
# The order is the order of the DialStyle enum in src/rationsids.h, and the file
# names are the image keys the editor asks for; keep the three in step by hand.
#
# WHY THESE ARE DRAWN RATHER THAN CROPPED FROM A PHOTOGRAPH, which is what this
# script used to do with dial-base.png. A dial is one bitmap rotated about its
# centre, so whatever is in the bitmap turns with it — and a photograph carries its
# lighting: a highlight on one side and shade on the other. Turned through 270
# degrees that reads as a knob that is darker on one side, and the side moves.
# Centring the crop fixed the orbit and could not fix that.
#
# So nothing here is lit from a direction. Every shade is either
#
#   * a function of RADIUS alone — a radial gradient pushed through a colour strip
#     (clut), which is the same from every angle by construction; or
#   * an N-fold pattern in ANGLE that is even about the pointer axis — knurling, and
#     the wedges of a spun face — so left and right of the pointer are mirror images.
#
# and the finished dial is averaged with its own mirror image and then CHECKED
# against it, pixel for pixel. A dial that is not exactly symmetric fails the build
# rather than shipping.
#
# A dial still has no drop shadow, for the reason it never had one: a shadow that
# rotates with the knob is worse than none.
#
# Each operator below was settled by running it and looking at the numbers, not by
# recollection — in particular: radial-gradient with gradient:radii is linear in
# radius and centred between the two middle pixels of an even canvas (its mirror
# image differs from it by nothing); -distort Polar puts the rectangle's LEFT edge
# straight up and runs anticlockwise; and a three-image -composite does not confine
# an overlay that has no alpha of its own, which is why over() builds the alpha.
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Drawn at four times the stored size and reduced, so every edge is antialiased
# by the reduction rather than by each operator's own idea of it.
S=$((DIAL_PX * 4))   # working size
H=$((S / 2))         # a radius of 1.0 in the tables below is H pixels
C="$((H - 1)).5"     # the centre in -draw coordinates, which count pixel CENTRES
# The body stops just short of the box, so its antialiased edge is never clipped.
# The ink still fills the box edge to edge at the drawn size, which src/geometry.h
# relies on when it spaces a dial from the switch under it.
EDGE=0.992

# Golds. GOLD is geometry.sh's own, sampled from the faceplate piping; the others
# are that hue taken lighter and darker for a bevel.
GOLD_HI="#F4DFA0"; GOLD_MID="#C99B4B"; GOLD_LO="#7C5622"; GOLD_DK="#3B280E"

px()  { awk -v v="$1" -v h="$H" 'BEGIN{printf "%.2f", v*h}'; }
# A point straight up from the centre at radius r, as a -draw y coordinate.
up()  { awk -v c="$C" -v r="$(px "$1")" 'BEGIN{print c-r}'; }

# ---- primitives --------------------------------------------------------------

# clut OUT "r:colour" ...   colour as a piecewise-linear function of 0..1.
# Two stops at the same position make a hard step.
clut() {
    local out="$1"; shift
    local L=4096 pr="" pc="" args=() st r c a b
    for st in "$@"; do
        r="${st%%:*}"; c="${st#*:}"
        if [ -n "$pr" ]; then
            a=$(awk -v v="$pr" -v L=$L 'BEGIN{printf "%d", v*L+0.5}')
            b=$(awk -v v="$r"  -v L=$L 'BEGIN{printf "%d", v*L+0.5}')
            if [ $((b - a)) -gt 0 ]; then
                args+=( \( -size "1x$((b - a))" "gradient:$pc-$c" -rotate -90 \) )
            fi
        fi
        pr="$r"; pc="$c"
    done
    magick "${args[@]}" +append -resize "${L}x1!" "$out"
}

# radial OUT "r:colour" ...   the whole canvas shaded by radius.
radial() {
    local out="$1"; shift
    clut "$tmp/_clut.png" "$@"
    magick -size ${S}x${S} -define gradient:radii=$H,$H radial-gradient:black-white \
        "$tmp/_clut.png" -clut "$out"
}

# disc OUT radius   a white antialiased disc on transparent, for cut().
# -draw "circle cx,cy px,py" takes a point ON the circle, not a radius.
disc() {
    magick -size ${S}x${S} xc:none -fill white -draw "circle $C,$C $C,$(up "$2")" "PNG32:$1"
}

# ring OUT r0 r1   a greyscale mask, white between the two radii.
ring() {
    magick -size ${S}x${S} xc:black \
        -fill white -draw "circle $C,$C $C,$(up "$3")" \
        -fill black -draw "circle $C,$C $C,$(up "$2")" "$1"
}

# teeth OUT n lo hi   an n-fold pattern in angle, greyscale lo..hi. A cosine with a
# crest on the pointer, so it is even about the pointer axis whatever n is.
teeth() {
    magick -size ${S}x${S} gradient: -rotate 90 -function Sinusoid "$2,90" \
        -function Polynomial "$(awk -v a="$3" -v b="$4" 'BEGIN{print b-a}'),$3" \
        -virtual-pixel HorizontalTile -distort Polar $H "$1"
}

# sheen OUT n gain pow [phase]   the bright wedges a lathe-turned face shows, n of
# them on black, to be screened over the face. Phase 90 puts a wedge on the pointer;
# 270 with n=2 puts the pair square ACROSS it instead — which is what the black
# face wants, because at the drawn size a wedge under the pointer reads as a second
# pointer.
sheen() {
    magick -size ${S}x${S} gradient: -rotate 90 -function Sinusoid "$2,${5:-90}" \
        -evaluate Pow "$4" -evaluate Multiply "$3" \
        -virtual-pixel HorizontalTile -distort Polar $H "$1"
}

# grooves OUT amount seed   fine concentric lathe rings, as a multiply layer. The
# noise runs along the radius only, so the rings are whole circles. Seeded, so the
# asset is the same bytes on every run.
grooves() {
    magick -size 2048x1 xc:gray50 -seed "$3" +noise Gaussian -colorspace Gray \
        -blur 0x0.6 -resize "4096x1!" -normalize \
        -function Polynomial "$2,$(awk -v a="$2" 'BEGIN{print 1-a}')" "$tmp/_g.png"
    magick -size ${S}x${S} -define gradient:radii=$H,$H radial-gradient:black-white \
        "$tmp/_g.png" -clut "$1"
}

# over BASE OVERLAY MASK   lay OVERLAY on BASE where the greyscale MASK is white.
over() {
    magick "$1" \( "$2" "$3" -alpha off -compose CopyOpacity -composite \) \
        -compose Over -composite "$1"
}

# cut BASE ALPHA OUT   give BASE the alpha of ALPHA. DstIn multiplies by the
# source's alpha, so ALPHA must carry its shape in its alpha channel.
cut() { magick "$1" -alpha set "$2" -compose DstIn -composite "PNG32:$3"; }

# bar OUT r0 r1 width colour   a pointer bar with rounded ends, up from the centre.
bar() {
    local w2; w2=$(awk -v w="$(px "$4")" 'BEGIN{print w/2}')
    magick -size ${S}x${S} xc:none -fill "$5" -draw "roundrectangle \
        $(awk -v c=$C -v w=$w2 'BEGIN{print c-w}'),$(up "$3") \
        $(awk -v c=$C -v w=$w2 'BEGIN{print c+w}'),$(up "$2") $w2,$w2" "PNG32:$1"
}

# inlay BASE POINTER   sink POINTER into BASE: a soft dark edge under it, then it.
inlay() {
    magick "$1" \( "$2" -channel A -morphology Dilate Disk:3 +channel -fill black -colorize 100 \
        -channel A -blur 0x3 -evaluate Multiply 0.8 +channel \) -compose Over -composite \
        "$2" -compose Over -composite "$1"
}

# finish WORK NAME   mirror-average, reduce to the stored size, and prove the result.
# Averaged with its mirror AFTER the reduction as well as before: the resize
# filter's own rounding is enough to leave a handful of pixels one step apart.
finish() {
    local out="$OUT_DIR/$2.png"
    magick "$1" \( +clone -flop \) -evaluate-sequence Mean \
        -resize ${DIAL_PX}x${DIAL_PX} \( +clone -flop \) -evaluate-sequence Mean \
        -strip -depth 8 "PNG32:$out"
    local size diff
    size="$(magick identify -format "%wx%h" "$out")"
    if [ "$size" != "${DIAL_PX}x${DIAL_PX}" ]; then
        echo "make_knob: $2.png is $size, but geometry.sh says ${DIAL_PX}x${DIAL_PX}." >&2
        exit 1
    fi
    diff="$(magick "$out" \( +clone -flop \) -metric AE -compare -format '%[distortion]' info:)"
    if [ "$diff" != "0" ]; then
        echo "make_knob: $2.png differs from its own mirror image in $diff pixels." >&2
        echo "           A dial that is not symmetric about its pointer looks lopsided as it turns." >&2
        exit 1
    fi
    magick identify "$out"
}

# ---- Black: the dial this plug-in always had, drawn symmetric -------------------
# Knurled black grip, gold ring, spun black face, gold wedge at mid-travel.
dial_black() {
    radial "$tmp/b.png" \
        0:#262626 0.45:#181818 0.68:#0E0E0E 0.715:#000000 \
        0.715:$GOLD_DK 0.735:$GOLD_LO 0.765:$GOLD_HI 0.79:$GOLD_MID 0.82:#A67A35 0.84:$GOLD_DK \
        0.84:#000000 0.86:#1C1C1C 0.925:#4A4A4A 0.97:#1A1A1A $EDGE:#050505 1:#050505
    teeth "$tmp/t.png" 72 0.30 1
    ring "$tmp/m.png" 0.85 1
    magick "$tmp/b.png" "$tmp/t.png" -compose Multiply -composite "$tmp/o.png"
    over "$tmp/b.png" "$tmp/o.png" "$tmp/m.png"
    sheen "$tmp/s.png" 2 0.24 5 270
    grooves "$tmp/g.png" 0.30 11
    ring "$tmp/m.png" 0 0.712
    magick "$tmp/b.png" "$tmp/s.png" -compose Screen -composite \
        "$tmp/g.png" -compose Multiply -composite "$tmp/o.png"
    over "$tmp/b.png" "$tmp/o.png" "$tmp/m.png"
    # The wedge is wide and bright on purpose: at the 56 units a dial is drawn at,
    # a narrow one is lost against the sheen.
    magick -size ${S}x${S} xc:none -fill "$GOLD_HI" -draw "polygon $(awk -v c=$C -v h=$H \
        'BEGIN{printf "%.1f,%.1f %.1f,%.1f %.1f,%.1f", c-0.09*h,c-0.70*h, c+0.09*h,c-0.70*h, c,c-0.26*h}')" \
        "PNG32:$tmp/p.png"
    inlay "$tmp/b.png" "$tmp/p.png"
    disc "$tmp/a.png" $EDGE
    cut "$tmp/b.png" "$tmp/a.png" "$tmp/w.png"
    finish "$tmp/w.png" dial
}

# ---- Gold: spun gold face, black knurled grip, black line --------------------
dial_gold() {
    radial "$tmp/b.png" \
        0:#F2D58A 0.35:#E2BA66 0.66:#CDA052 0.715:$GOLD_HI 0.755:#9A7030 0.78:$GOLD_DK \
        0.78:#000000 0.805:#1A1A1A 0.90:#474747 0.965:#1A1A1A $EDGE:#050505 1:#050505
    # A gold face is already bright, so a screened wedge alone barely shows on it:
    # the two-fold shade underneath is what gives the face its turned look.
    sheen "$tmp/s.png" 2 0.30 4
    teeth "$tmp/t.png" 2 0.66 1
    grooves "$tmp/g.png" 0.20 23
    ring "$tmp/m.png" 0 0.70
    magick "$tmp/b.png" "$tmp/t.png" -compose Multiply -composite \
        "$tmp/s.png" -compose Screen -composite \
        "$tmp/g.png" -compose Multiply -composite "$tmp/o.png"
    over "$tmp/b.png" "$tmp/o.png" "$tmp/m.png"
    teeth "$tmp/t.png" 64 0.30 1
    ring "$tmp/m.png" 0.795 1
    magick "$tmp/b.png" "$tmp/t.png" -compose Multiply -composite "$tmp/o.png"
    over "$tmp/b.png" "$tmp/o.png" "$tmp/m.png"
    bar "$tmp/p.png" 0.26 0.69 0.07 "#0B0B0B"
    magick "$tmp/b.png" "$tmp/p.png" -compose Over -composite "$tmp/b.png"
    disc "$tmp/a.png" $EDGE
    cut "$tmp/b.png" "$tmp/a.png" "$tmp/w.png"
    finish "$tmp/w.png" dial_gold
}

# ---- the pointer knob, in two cuts -------------------------------------------
# Not a disc: a round hub with a long tapered nose and a short tail, mirror
# symmetric about the pointer, which is all a rotating bitmap needs.
#
# The body is shaded by DISTANCE FROM ITS OWN OUTLINE rather than by radius, so the
# bevel — and, on the head cut, the gold edge — is the same width all the way
# round, nose included. $2.. is that shade as a colour strip, outline first.
#
# The spine and hub are heavy for the shape. The first cut had them half this
# weight and it read well at 98 units and not at all at 40, where the spine fell
# under two pixels; nothing here is drawn larger than about 56.
pointer_knob() {
    local name="$1"; shift
    local outline
    outline="$(awk -v c=$C -v h=$H 'BEGIN{
        printf "polygon %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f",
        c-0.15*h,c-0.97*h, c+0.15*h,c-0.97*h, c+0.62*h,c-0.05*h, c+0.50*h,c+0.36*h,
        c+0.26*h,c+0.86*h, c-0.26*h,c+0.86*h, c-0.50*h,c+0.36*h, c-0.62*h,c-0.05*h}')"
    # The corners are rounded by blurring the hard shape and tightening it again.
    magick -size ${S}x${S} xc:black -fill white -draw "$outline" \
        -draw "circle $C,$C $C,$(up 0.70)" -blur 0x22 -level 46%,54% "$tmp/sil.png"
    # Distance in from the outline, saturating at 90 working pixels.
    magick "$tmp/sil.png" -threshold 50% -morphology Distance Euclidean:4,'90!' \
        -blur 0x5 "$tmp/dist.png"
    clut "$tmp/_shade.png" "$@"
    magick "$tmp/dist.png" "$tmp/_shade.png" -clut "$tmp/b.png"
    bar "$tmp/p.png" 0.36 0.91 0.105 "$GOLD_HI"
    inlay "$tmp/b.png" "$tmp/p.png"
    radial "$tmp/hub.png" 0:$GOLD_HI 0.14:$GOLD_MID 0.25:$GOLD_LO 0.28:$GOLD_DK \
        0.28:#000000 0.32:#000000 1:#000000
    ring "$tmp/m.png" 0 0.32
    over "$tmp/b.png" "$tmp/hub.png" "$tmp/m.png"
    magick "$tmp/b.png" \( "$tmp/sil.png" -alpha copy \) -compose DstIn -composite \
        "PNG32:$tmp/w.png"
    finish "$tmp/w.png" "$name"
}

# Pointer, for the amp head. The head's faceplate is black, so a black pointer
# with nothing round it is black on black: all that showed was the hub and the
# spine. This cut carries a gold edge round the whole silhouette and a brighter
# bevel inside it.
dial_pointer() {
    pointer_knob dial_pointer \
        0:$GOLD_DK 0.05:$GOLD_LO 0.13:$GOLD_HI 0.21:$GOLD_MID 0.27:$GOLD_DK \
        0.27:#000000 0.33:#1A1A1A 0.52:#9A9A9F 0.76:#3A3A3C 1:#242425
}

# The same knob for a pedal's face. No gold edge: a pedal enclosure is a saturated
# colour and the black body is its own outline against it.
dial_pedal() {
    pointer_knob dial_pedal \
        0:#050505 0.12:#222222 0.34:#77777B 0.60:#303032 1:#1E1E1F
}

dial_black
dial_gold
dial_pointer
if [ "${DIAL_PEDAL:-0}" = "1" ]; then
    dial_pedal
fi
echo "Wrote the dial art (drawn, mirror-checked, no directional lighting)."
