# Master geometry and palette for the Rations editor art.
#
# Sourced by make_assets.sh and its helpers; the same numbers are mirrored as
# constants in src/geometry.h (keep the two in sync by hand).
#
# Every value here is a LOGICAL pixel of the editor canvas. The canvas is one
# unit system: the editor applies a single cairo_scale(s, s) at compose time and
# divides mouse coordinates by s before hit-testing, so no scale factor is ever
# baked into a constant.
#
# The layout numbers below are not invented — they are measured off the panel
# art (head-base.png) so the controls land inside the gold piping rather than
# near it. Re-derive them with:
#
#   magick head-base.png -fuzz 6% -trim +repage head.png        # -> 1133x403
#   # then find the gold piping: pixels with r>130, g>95, b<0.55r
#   #   rows  54..58 and 336..340   -> piping top / bottom
#   #   cols  51..54 and 1078..1081 -> piping left / right
#
# giving the inner faceplate rectangle used for all placement below. That scan
# was re-run against this project's own art rather than inherited on faith, and
# it reproduces those four bands exactly.

# ---- Canvas -----------------------------------------------------------------
# Exactly the trimmed size of the amp-head art (head + drop shadow). Choosing
# the art's own size means the default window is a pixel-exact 1:1 blit with no
# resampling at all; anything else would soften the panel at its default size.
WIN_W=1133
WIN_H=403

# Host resizing range, as a multiple of the canvas. The art is the resolution
# ceiling: the head is 1133 px wide in the source, so above 1.0 the panel is
# genuinely upscaled and 1.5 is where a photographic faceplate stops holding up.
SCALE_MIN=0.66
SCALE_MAX=1.50

# ---- Inner faceplate (inside the gold piping), measured as above -------------
FACE_L=59
FACE_T=62
FACE_R=1074   # exclusive
FACE_B=333    # exclusive
FACE_CX=566   # (FACE_L + FACE_R) / 2

# ---- Palette (0xRRGGBB; mirrored in src/gfx/palette.h) ----------------------
# Sampled from the art rather than picked: FACE/GOLD are the faceplate and the
# piping in head-base.png, TEXT is what the mock's dial labels actually are.
BG_COLOR="#121011"      # letterbox around the head, and the cabinet page's ground
FACE_COLOR="#1E1C1D"    # faceplate fallback if head.png fails to load
GOLD="#B88B4C"          # piping / hairline borders, sampled from the art
TEXT_COLOR="#FFFFFF"    # labels
DIM_COLOR="#9A9490"     # empty / disabled text
ACCENT="#3FD05A"        # meter fill, browser selection, active states
ACCENT_BRIGHT="#7FE89A" # bright tick at the top of the meter fill
PEAK_COLOR="#FF3B30"    # meter peak marker

# ---- Stored asset sizes -----------------------------------------------------
# Assets are stored above their 1x on-screen size so the pre-scaled cache has
# something to downsample from at SCALE_MAX. head.png is the exception: the
# source art has no more detail to give, so it is stored 1:1 and is the reason
# SCALE_MAX is 1.5 rather than 2.
HEAD_W=1133; HEAD_H=403
# The speaker cabinet, stored at its own trimmed size. It is drawn far smaller
# than this (see CAB_* in src/geometry.h): the editor canvas is the amp head's
# shape, 2.81:1, and a 4x12 cabinet is 1.70:1, so the cabinet page is height-
# limited and the stored art is a long way above its draw size.
CAB_W=1483; CAB_H=872
DIAL_PX=256                   # square; drawn at 56x56, extra headroom for rotation
LED_PX=128                    # source size, drawn at 18x18
SWITCH_W=112; SWITCH_H=184    # 4x of the 28x46 draw size
METER_W=80;  METER_H=408      # 4x of the 20x102 draw size

# ---- Pedalboard -------------------------------------------------------------
# Every pedal-*-base.png is the SAME 494x740 export and all five trim to the SAME
# 468x691, so one layout serves all five enclosures and a re-export that moves any
# of them is caught by make_pedals.sh.
#
# 468x691, not the 470x693 an alpha bounding box reports: `-fuzz 6% -trim` is what
# actually produces the asset and it takes one more pixel of antialiasing off each
# edge. The number here has to be the one the pipeline yields, or the check below
# fails on its own output. Control positions measured on the 494x740 export are
# therefore referenced to +13+21, not +12+20.
#
# Stored at the art's own trimmed size and downscaled at draw time, which is the
# cabinet's arrangement rather than the head's: the head is stored at its draw
# size for a 1:1 blit because it IS the window, while a pedal is drawn at 190x280
# (a 2.46x downscale) and would be soft if it were stored that small and then
# scaled back up at kScaleMax. ImageCache::getScaled caches the result per size.
# ---- the head's title badge ------------------------------------------------
# The NAMp wordmark that sits above "Rations" on the head page. BADGE_W/H is the
# STORED size; the drawn size is kBadgeW/kBadgeH in src/geometry.h, which is a
# third of it. make_assets.sh checks the stored height against BADGE_H, so a
# change to the source art's proportions is a loud failure rather than a badge
# that quietly draws squashed.
BADGE_W=512; BADGE_H=152

PEDAL_SRC_W=468; PEDAL_SRC_H=691
PEDAL_W=190;     PEDAL_H=281   # logical draw size; mirrored in src/geometry.h
PEDAL_NAMES="boost chorus flanger delay reverb"

# The board's own canvas, and the grid the five sit on. Two rows: PRE carries two
# pedals centred as a pair, POST carries three centred as a triple, both about the
# page centre, with the same 22-unit gap between neighbours and a 24-unit margin
# at each edge. 3*190 + 2*22 + 2*24 = 662 across; the header band the back button
# lives in (50) + 20 + 281 + 30 + 281 + 19 = 681 down. Mirrored in src/geometry.h.
PEDAL_PAGE_W=662; PEDAL_PAGE_H=681
PEDAL_GAP=22
PEDAL_ROW1_Y=70; PEDAL_ROW2_Y=380
PEDAL_PRE_CX0=225;  PEDAL_PRE_CX1=437
PEDAL_POST_CX0=119; PEDAL_POST_CX1=331; PEDAL_POST_CX2=543

# The footswitch cap, given the same treatment as the dial: cropped square about
# its own centre and alpha-masked to a circle, so it can be blitted on its pivot
# without a straight cut through its drop shadow. Measured on the source with an
# alpha>100 bounding box: content 107x108 at +17+12, so the cap centre is
# (70.5, 66) and its content radius is at most 54.
PSWITCH_CX=70; PSWITCH_CY=66
PSWITCH_SIDE=116               # 2*58: an inscribed circle of r=58 never clips
PSWITCH_MASK_R=56              # >= 54 (content) and < 58 (half-side)
PSWITCH_PX=128                 # stored square, drawn at 44x44

# There is no badge asset. The "Rations" wordmark is drawn as text in Michroma
# at run time, the way the author's other plug-in draws its own name.

# ---- Source art and output --------------------------------------------------
# Both live in resources/img: the *-base.png files are the untrimmed source
# exports and are NOT shipped (CMakeLists.txt lists what the bundle copies), and
# everything else in that directory is a build product of this pipeline or a
# committed SVG. Overridable so the pipeline can be pointed at a re-exported set.
: "${ART_DIR:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/resources/img}"
: "${OUT_DIR:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/resources/img}"
