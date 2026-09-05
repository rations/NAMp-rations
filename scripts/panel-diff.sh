#!/usr/bin/env bash
# Compare two renders of the editor's four pages, pixel for pixel.
#
# usage: panel-diff.sh <dirA> <prefixA> <dirB> <prefixB> [labelA labelB]
#
# Each directory holds <prefix>-head.png, -cabinet.png, -pedalboard.png and
# -settings.png, which is exactly what tools/panelrender writes.
#
# WHAT THIS IS FOR. It is the only automated check of a NON-LINUX editor's
# appearance, and the reason scripts/build-win-deps.sh and scripts/build-mac-deps.sh
# pin the same five dependencies to the same versions as this machine's system
# libraries. Comparing two renders of the same sources from the same resources/
# tree at the same scale exercises cairo, FreeType rasterisation, PNG decode,
# NanoSVG and the whole of src/gfx on the other platform.
#
# It has two callers with two different questions:
#
#   makedist-windows.sh  Linux against the MinGW cross build, under Wine.
#   .github/workflows/macos.yml  the arm64 slice against the x86_64 one. That
#                        pair asks a question the Windows one cannot: whether the
#                        two ISAs round the dial trigonometry the same way. It
#                        needs no baseline from anyone's machine, which is why it
#                        is the mac diff that runs on every push.
#
# THE THRESHOLDS ARE MEASURED, NOT CHOSEN, and they are the CALLER's to state --
# they arrive as $PANEL_MAX_PIXELS and $PANEL_MAX_DELTA rather than being fixed
# here, because a figure measured for one pair of platforms says nothing about
# another. Each caller records what it measured beside the number it set.
#
# MAX_DELTA IS THE SHARPER OF THE TWO GATES and the one to trust. A rounding
# difference is 1/255 by definition; a real rasterisation change puts down ink
# where there was none, which is a delta of tens or hundreds whatever the pixel
# count says.
set -euo pipefail

if [ $# -lt 4 ]; then
  sed -n '2,6p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 1
fi

DIR_A="$1"; PRE_A="$2"; DIR_B="$3"; PRE_B="$4"
LABEL_A="${5:-$PRE_A}"; LABEL_B="${6:-$PRE_B}"

: "${PANEL_MAX_PIXELS:?set PANEL_MAX_PIXELS (per page) before calling}"
: "${PANEL_MAX_DELTA:?set PANEL_MAX_DELTA (per channel) before calling}"

if ! command -v magick >/dev/null; then
  echo "ImageMagick (magick) not found; cannot decode the renders." >&2
  exit 1
fi

RAW="$(mktemp -d)"
trap 'rm -rf "$RAW"' EXIT

for _p in head cabinet pedalboard settings; do
  for _side in "a:$DIR_A/$PRE_A" "b:$DIR_B/$PRE_B"; do
    _tag="${_side%%:*}"; _stem="${_side#*:}"
    if [ ! -f "$_stem-$_p.png" ]; then
      echo "missing render: $_stem-$_p.png" >&2
      exit 1
    fi
    magick "$_stem-$_p.png" -depth 8 "rgb:$RAW/$_tag-$_p.raw"
  done
done

RAW="$RAW" LABEL_A="$LABEL_A" LABEL_B="$LABEL_B" \
PANEL_MAX_PIXELS="$PANEL_MAX_PIXELS" PANEL_MAX_DELTA="$PANEL_MAX_DELTA" python3 - <<'PYEOF'
import os, sys

d          = os.environ["RAW"]
max_pixels = int(os.environ["PANEL_MAX_PIXELS"])
max_delta  = int(os.environ["PANEL_MAX_DELTA"])
label_a    = os.environ["LABEL_A"]
label_b    = os.environ["LABEL_B"]

fail, tot_d, tot_p = [], 0, 0
for page in ("head", "cabinet", "pedalboard", "settings"):
    a = open(f"{d}/a-{page}.raw", "rb").read()
    b = open(f"{d}/b-{page}.raw", "rb").read()
    if len(a) != len(b):
        fail.append(f"{page}: the two renders are different SIZES "
                    f"({len(a)//3} vs {len(b)//3} px) - the layout itself diverged")
        continue
    npx, ndiff, worst = len(a) // 3, 0, 0
    for i in range(0, len(a), 3):
        if a[i:i+3] != b[i:i+3]:
            ndiff += 1
            w = max(abs(x - y) for x, y in zip(a[i:i+3], b[i:i+3]))
            if w > worst:
                worst = w
    tot_d += ndiff
    tot_p += npx
    print(f"  {page:<11} {ndiff:>5} of {npx:>8} px differ, worst channel delta {worst}/255")
    if ndiff > max_pixels:
        fail.append(f"{page}: {ndiff} differing pixels, cap is {max_pixels}")
    if worst > max_delta:
        fail.append(f"{page}: worst channel delta {worst}/255, cap is {max_delta} - "
                    f"that is ink moving, not rounding")
print(f"  {'TOTAL':<11} {tot_d:>5} of {tot_p:>8} px")

if fail:
    print(f"\nthe {label_b} editor does not draw what the {label_a} one draws:", file=sys.stderr)
    for f in fail:
        print(f"  {f}", file=sys.stderr)
    print("\nCheck that the two sides' cairo/freetype/pixman/libpng/zlib still match:", file=sys.stderr)
    print("scripts/build-win-deps.sh and scripts/build-mac-deps.sh pin them to this", file=sys.stderr)
    print("machine's system versions, and this comparison is why.", file=sys.stderr)
    sys.exit(1)
PYEOF
