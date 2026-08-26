#!/usr/bin/env bash
# Rations — generate amp-style mini bat-toggle art (ImageMagick 7).
# Copyright (c) 2026 rations. MIT licence (see LICENSE).
#
# Derived from the author's own make_footswitch.sh in a sibling stompbox
# project, which carries a GPL-3.0-or-later header only because the project
# around it links JUCE. That script contains no JUCE reference — it is pure
# ImageMagick — so the author has relicensed their own work MIT here; the
# rationale is recorded in NOTICE. What is inherited is the technique (shape
# mask -> CopyOpacity for a chrome body, offset blurred silhouette for a contact
# shadow, screen-composited specular, PNG32 to stop a neutral-grey layer
# demoting the canvas to Gray), not a stompbox footswitch.
#
# Produces two registered RGBA PNGs:
#   switch_up_ring.png    ball up
#   switch_down_ring.png  ball down
#
# GEOMETRY — the pivot is the lever's POINT, not a nut. The lever is a taper
# whose apex sits at the centre of the frame in BOTH frames and whose wide end
# carries a chrome ball; toggling swings the ball to the top or the bottom. The
# fixed part is therefore the apex, and the ball is the only thing that appears
# to travel, which is what makes the state readable at a glance. There is no
# chrome nut at the centre: at this size a second chrome circle sitting at the
# pivot competes with the ball for the eye and both stop reading.
#
# There is deliberately NO escutcheon plate either. A toggle draws at 28x46
# logical pixels next to a 56 px knob, and at that size a bordered plate
# collapses into a smudge — the chrome lever against the black tolex is the only
# part that stays legible.
#
# The frames are also deliberately NEUTRAL about meaning. Three toggles share
# this pair (Bypass, EQ, Gate) and "up" does not mean the same for all of them —
# bypass engaged is the plug-in off. Which frame a parameter value picks is the
# editor's decision, so no green accent is baked in here.
#
# The pivot shadow is drawn once into $tmp and composited into both frames, so
# the two register pixel-perfect when swapped at runtime — the same discipline
# the shared collar enforces in the original script.
set -euo pipefail
cd "$(dirname "$0")"
source ./geometry.sh

# EVERY coordinate below is in STORED-ASSET pixels (SWITCH_W x SWITCH_H), and
# `u` scales it into the supersampled drawing canvas. Keeping one unit system is
# not fussiness: hand-mixing stored and canvas units silently shrinks the whole
# switch by the supersample factor, which looks like a rendering bug rather than
# an arithmetic one.
SS=4
u() { echo $(( $1 * SS )); }

W=$(u "$SWITCH_W")
H=$(u "$SWITCH_H")
CX=$(u $((SWITCH_W / 2)))
CY=$(u $((SWITCH_H / 2)))

BALL_R=$(u 19)          # chrome ball on the moving end
APEX_HALF=$(u 3)        # lever half-width AT THE PIVOT — the point it swings on.
                        # Not 0: a true zero-width apex antialiases away to
                        # nothing at 1/4 scale and the lever looks detached.
NECK_HALF=$(u 11)       # lever half-width where it enters the ball; must stay
                        # under BALL_R so the neck is swallowed, not shouldered
THROW=$(u 60)           # pivot-to-ball-centre distance. BALL_R + THROW = 79 <
                        # SWITCH_H/2 = 92, so the ball clears the frame edge.
                        # Ball and throw are also balanced against each other:
                        # a bigger ball on a shorter lever stops reading as a
                        # toggle and starts reading as a pushpin.

# ---- Escutcheon ring -------------------------------------------------------
# A gold annulus around a black recess, with the lever pivoting inside it. This
# is the one place the design walks back its own "NO escutcheon plate" note
# above: what does not survive at this size is a bordered rectangular PLATE, but
# a ring is a circle, so it stays legible when a rectangle would smear.
#
# Its gold is $GOLD from geometry.sh — the same variable make_meter.sh strokes
# the level-meter bezels with, and sampled from the faceplate piping in the
# first place. Hardcoding a hex here would let the ring and the meters drift
# apart the moment either is retouched.
#
# Radii measured off the hand-drawn reference this replaces: an annulus about
# 3 px wide with its outer edge at ~24 px. The ring is centred on the PIVOT
# rather than where the reference had it (about 5 px higher), so the lever's
# apex sits dead centre in the recess and the ring lands on the same line the
# power lamp is aligned to.
RING_FILL_R=$(u 24)     # black recess the lever pivots in
RING_R=$(u 23)          # gold annulus, stroke centre
RING_W=$(u 3)           # annulus stroke width

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Contact shadow, so the ring sits ON the tolex rather than floating over it.
# Sized to the ring rather than the old bare pivot: a shadow smaller than the
# thing above it is invisible, and this one has to read from under a 24 px disc.
# PNG32 forced: this is the base layer of both composites.
magick -size ${W}x${H} xc:none \
  -fill black -draw "ellipse $CX,$((CY + $(u 4))) $((RING_FILL_R + $(u 3))),$((RING_FILL_R + $(u 2))) 0,360" \
  -channel A -evaluate multiply 0.5 +channel -blur 0x$(u 8) \
  "PNG32:$tmp/pivot_shadow.png"

# The ring itself, drawn ONCE and composited into both frames. That is what
# guarantees the fixed end is pixel-identical across the swap — the same
# discipline the shared nut had before it, and the reason the toggle does not
# appear to jump when clicked.
magick -size ${W}x${H} xc:none \
  -fill black -stroke none -draw "ellipse $CX,$CY $RING_FILL_R,$RING_FILL_R 0,360" \
  -fill none -stroke "$GOLD" -strokewidth $RING_W \
  -draw "ellipse $CX,$CY $RING_R,$RING_R 0,360" \
  "PNG32:$tmp/ring.png"

# ---- One frame -------------------------------------------------------------
# $1 = +1 for ball up / -1 for ball down, $2..$3 = chrome gradient stops,
# $4 = output.
bat() {
  local dir=$1 hi=$2 lo=$3 out=$4
  local tipY=$((CY - dir * THROW))
  local midY=$(( (tipY + CY) / 2 ))

  # Three masks, not one. The lever and the ball are shaded separately because a
  # taper and a sphere do not catch the light the same way — one gradient
  # stretched over both flattens the ball into a disc. The union mask exists only
  # for the cast shadow, which is a silhouette and does not care about shading.
  magick -size ${W}x${H} xc:none -fill white \
    -draw "polygon $((CX - APEX_HALF)),$CY $((CX + APEX_HALF)),$CY $((CX + NECK_HALF)),$tipY $((CX - NECK_HALF)),$tipY" \
    "PNG32:$tmp/lever_mask.png"

  magick -size ${W}x${H} xc:none -fill white \
    -draw "circle $CX,$tipY $CX,$((tipY + BALL_R))" \
    "PNG32:$tmp/ball_mask.png"

  magick -size ${W}x${H} xc:none -fill white \
    -draw "polygon $((CX - APEX_HALF)),$CY $((CX + APEX_HALF)),$CY $((CX + NECK_HALF)),$tipY $((CX - NECK_HALF)),$tipY" \
    -draw "circle $CX,$tipY $CX,$((tipY + BALL_R))" \
    "PNG32:$tmp/all_mask.png"

  # Lever: broad off-centre radial, so the left flank is the lit one.
  magick -size ${W}x${H} \
    -define gradient:center=$((CX - $(u 16))),$midY -define gradient:radii=$(u 110),$(u 110) \
    radial-gradient:"$hi"-"$lo" \
    "$tmp/lever_mask.png" -compose CopyOpacity -composite \
    "PNG32:$tmp/lever.png"

  # Ball: a tight radial centred UP-LEFT of the ball itself is what makes it read
  # as a sphere instead of a chrome coin. The dark stroke is drawn after the
  # CopyOpacity, so it lands half outside the mask and gives the ball a crisp
  # edge against the tolex.
  magick -size ${W}x${H} \
    -define gradient:center=$((CX - $(u 8))),$((tipY - $(u 8))) \
    -define gradient:radii=$(u 40),$(u 40) \
    radial-gradient:"$hi"-"$lo" \
    "$tmp/ball_mask.png" -compose CopyOpacity -composite \
    -fill none -stroke '#15171b' -strokewidth $(u 2) \
    -draw "circle $CX,$tipY $CX,$((tipY + BALL_R))" \
    "PNG32:$tmp/ball.png"

  # Cast shadow: the whole silhouette, offset down-right and blurred.
  magick "$tmp/all_mask.png" \
    -channel RGB -evaluate set 0 +channel \
    -channel A -evaluate multiply 0.5 +channel \
    -roll +$(u 5)+$(u 6) -blur 0x$(u 6) \
    "PNG32:$tmp/cast_shadow.png"

  # Specular: a streak down the lit flank of the lever plus a hotspot up-left on
  # the ball, both clipped to the silhouette with CopyOpacity so neither
  # lightens the tolex beside it.
  magick -size ${W}x${H} xc:black \
    -fill white \
    -draw "ellipse $((CX - $(u 4))),$midY $(u 3),$((THROW / 3)) 0,360" \
    -draw "ellipse $((CX - $(u 7))),$((tipY - $(u 7))) $(u 7),$(u 6) 0,360" \
    -blur 0x$(u 5) \
    "$tmp/all_mask.png" -compose CopyOpacity -composite \
    "PNG32:$tmp/spec.png"

  # Order matters twice over. The ring goes ON TOP of the cast shadow, so the
  # gold stays clean instead of being dimmed by the lever's own shadow falling
  # across it; and the lever goes on top of the ring, so it reads as pivoting
  # INSIDE the recess rather than being pasted over a decal. Ball over lever, so
  # the neck is hidden under the ball rather than butting against its outline.
  magick -size ${W}x${H} xc:none \
    "$tmp/pivot_shadow.png" -compose over -composite \
    "$tmp/cast_shadow.png"  -compose over -composite \
    "$tmp/ring.png"         -compose over -composite \
    "$tmp/lever.png"        -compose over -composite \
    "$tmp/ball.png"         -compose over -composite \
    \( "$tmp/spec.png" \) -compose screen -composite \
    -resize ${SWITCH_W}x${SWITCH_H} \
    -strip -depth 8 "PNG32:$out"
}

# Up: pointing towards the light, so the brighter of the two.
bat  1 '#f8fafc' '#4e5257' "$OUT_DIR/switch_up_ring.png"
# Down: identical geometry mirrored about the pivot, dimmer because the lever is
# now angled away from the light.
bat -1 '#cbcfd3' '#3c4045' "$OUT_DIR/switch_down_ring.png"

magick identify "$OUT_DIR/switch_up_ring.png" "$OUT_DIR/switch_down_ring.png"
echo "Wrote switch_up_ring.png and switch_down_ring.png (registered bat toggle frames)."
