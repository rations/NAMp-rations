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
# There is no badge asset. The "Rations" wordmark is drawn as text in Michroma
# at run time, the way the author's other plug-in draws its own name.

# ---- Source art and output --------------------------------------------------
# Both live in resources/img: the *-base.png files are the untrimmed source
# exports and are NOT shipped (CMakeLists.txt lists what the bundle copies), and
# everything else in that directory is a build product of this pipeline or a
# committed SVG. Overridable so the pipeline can be pointed at a re-exported set.
: "${ART_DIR:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/resources/img}"
: "${OUT_DIR:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/resources/img}"
