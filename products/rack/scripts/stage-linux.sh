#!/usr/bin/env bash
# Build NAMp Rack and the five pedals for 64-bit Linux, gate them, and stage them for the release.
#
# THIS SCRIPT PRODUCES NO ARCHIVE. The release is ONE package holding all three things this
# project ships, and it is assembled by scripts/makedist-linux.sh at the repository root, which
# calls this and the plug-in's stage script in turn. See that script's header for why the split is
# here and not somewhere else.
#
# WHAT IT STAGES:
#
#   rack/namp-rack      the amp head as a JACK application, with a rack that hosts other people's
#                       VST3 and LV2 plug-ins before and after it. The amp, its art and its fonts
#                       are linked IN -- there is no bundle beside it to find and nothing to
#                       install first -- and plug-in discovery re-execs this same binary in
#                       scan-child mode, so a bundle that crashes during a scan takes a throwaway
#                       process with it instead of the rack. That is why the archive holds one
#                       executable and a launcher rather than a program and its plug-in.
#   rack/desktop/       the menu entry and its four icons.
#   pedals/             Boost, Chorus, Flanger, Delay and Reverb, built by this same build from a
#                       pinned submodule. They are staged at the TOP of the package rather than
#                       under rack/ because they are not the rack's: they are ORDINARY PLUG-INS
#                       that any host on the machine will find, and the install script puts them
#                       in ~/.vst3 beside NAMp Rations rather than anywhere the rack owns. That
#                       they are built by this build is a fact about the build, not about them.
#
# WHAT IS NOT IN HERE. NAMp-Rack-Amp.vst3 is built by every configure, and it is NOT packaged:
# it exists so the SDK validator has something to validate, which is by a wide margin the cheapest
# gate on the amp. The amp as a plug-in for a DAW is NAMp Rations, and it is in this same release,
# staged by the other script. Shipping a second, pedal-less plug-in that does a subset of what
# that one does would be asking a user to choose with nothing to go on.
#
# THE SAME AMP IS IN BOTH, and that is the point of the repository rather than a duplication to
# apologise for: one tree, so a fix to the amp lands once.
#
# WHAT THIS GATES ON. Everything is measured on the built binary -- what it links, what it
# exports, whether its art really is inside it, what runtime it asks its loader for, and that it
# runs far enough to print its usage. All of it works on any machine: no JACK server, no X
# display, no plug-ins installed. The proofs that need those (namp_chaincheck, namp_hostcheck,
# the editor-cycles and rack-stress runs, live audio at 128 frames) are run by hand against a rig
# that has them.
set -euo pipefail

PRODUCT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$PRODUCT/../.." && pwd)"
BUILD="${NAMP_RACK_BUILD_DIR:-$REPO/build-rack}"
ARCH="$(uname -m)"

# shellcheck source=../../../scripts/dist-common.sh
. "$REPO/scripts/dist-common.sh"

STAGE="${1:-}"
[ -n "$STAGE" ] && [ -d "$STAGE" ] ||
  namp_dist_die "usage: stage-linux.sh <stagedir>   (an existing directory to stage into)
This script stages; it packages nothing. Run scripts/makedist-linux.sh to build a release."

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNAMP_PRODUCT=rack -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

PKGDIR="$STAGE/rack"
PEDALDIR="$STAGE/pedals"
mkdir -p "$PKGDIR"

# --- the program ------------------------------------------------------------
RACK="$BUILD/namp-rack"
[ -f "$RACK" ] || namp_dist_die "namp-rack was not built at $RACK"
cp "$RACK" "$PKGDIR/"
PKGRACK="$PKGDIR/namp-rack"
strip --strip-unneeded "$PKGRACK"

# --- gates ------------------------------------------------------------------
# IT MUST LINK JACK. This IS the audio application; one that somehow came out without libjack
# could not make a sound, and it would not say so until it ran.
namp_dist_must_link "$PKGRACK" 'libjack' "namp-rack" \
  "It could not open an audio device."

# AND IT MUST LINK THE LV2 HOST LIBRARIES. Half of what this program is for is hosting LV2
# plug-ins, and that half is a CMake option: a configure that could not find lilv and suil
# produces a binary that builds, starts, plays the amp and silently finds no LV2 plug-in
# anywhere. Nothing about that failure looks like a failure, which is why it is asserted here
# rather than left to a user to report as "it doesn't see my plug-ins".
namp_dist_must_link "$PKGRACK" 'liblilv' "namp-rack" \
  "It was configured without the LV2 host libraries and would find no LV2 plug-in at all."
namp_dist_must_link "$PKGRACK" 'libsuil' "namp-rack" \
  "It was configured without suil and could show no LV2 plug-in's own editor."

# IT MUST EXPORT NOTHING AT ALL, and this is the sharpest check in the file.
#
# This binary contains the NAM core AND the SDK's plug-in-side classes, and it dlopens other
# people's plug-ins into its own process -- plug-ins built against the same SDK, carrying the same
# symbol names. Two copies of a Steinberg symbol in one process is a crash nobody can debug, with
# our stack nowhere in the backtrace. -Wl,--exclude-libs,ALL is what stops it, and the check is
# that the dynamic symbol table is EMPTY: not "small", not "only ours", empty. An executable has
# no reason to export anything, so unlike the plug-in's export list there is no innocent template
# instantiation to make room for.
namp_dist_exports_exactly "$PKGRACK" "namp-rack"
namp_dist_no_unique "$PKGRACK" "namp-rack"

# THE ART IS REALLY INSIDE IT. The rack carries no Contents/Resources to check, because its art
# and fonts are compiled in as byte arrays -- so the equivalent assertion is that the embedded
# table's own keys are in the binary. cmake/embedresources.cmake writes each resource's relative
# path as a string literal beside its bytes, and those literals survive --strip-unneeded, so
# finding them proves the table was generated and linked rather than quietly emptied by a
# resource list that stopped matching the files on disk.
#
# Without this, a build with an empty table draws flat rectangles and a fallback font, says so
# only on stderr, and passes every other check here.
#
# The literals are extracted ONCE into a file rather than re-run per resource down a pipe into
# `grep -q`. That is not only six times faster: grep -q closes the pipe as soon as it matches,
# strings takes SIGPIPE, and under `set -o pipefail` the successful case becomes a FAILING
# pipeline. Found by this check reporting a missing head.png that was demonstrably present.
RACK_STRINGS="$(mktemp)"
trap 'rm -f "$RACK_STRINGS"' EXIT
strings "$PKGRACK" > "$RACK_STRINGS"
for _res in img/head.png img/cabinet.png img/dial.png img/File.svg \
            fonts/Michroma-Regular.ttf fonts/Roboto-Regular.ttf; do
  grep -qx "$_res" "$RACK_STRINGS" ||
    namp_dist_die "namp-rack does not carry $_res; its art was not linked in and the editor
would draw flat rectangles. Check the embedded-resource list in CMakeLists.txt."
done

# IT RUNS. --help touches the argument parser and the settings-path logic and returns 0, which is
# as far as anything can be driven without a display and a JACK server.
RACK_HELP="$("$PKGRACK" --help 2>&1 || true)"
if ! printf '%s' "$RACK_HELP" | grep -q "usage: namp-rack"; then
  echo "namp-rack --help did not print its usage:" >&2
  printf '%s\n' "$RACK_HELP" | head -10 >&2
  exit 1
fi

# THE RUNTIME CEILING, against the declared release baseline.
namp_dist_abi_baseline "namp-rack" "$PKGRACK"

# --- the launcher -----------------------------------------------------------
# A shipped component like the program itself: a menu entry whose icon is missing shows a generic
# cog, and a missing entry means the program is installed and invisible. Neither says anything on
# stderr, so both are asserted rather than assumed.
namp_dist_require_files "$PRODUCT/packaging" "the launcher" \
  namp-rack.desktop \
  icons/namp-rack-256.png icons/namp-rack-128.png \
  icons/namp-rack-64.png icons/namp-rack-48.png

mkdir -p "$PKGDIR/desktop"
cp "$PRODUCT/packaging/namp-rack.desktop" "$PKGDIR/desktop/"
cp "$PRODUCT"/packaging/icons/namp-rack-*.png "$PKGDIR/desktop/"

# --- the pedals -------------------------------------------------------------
# DECISION R7. Five plug-ins built by this build from a pinned submodule and shipped beside the
# rack, which loads them through the same catalogue, the same out-of-process scan and the same
# chain as anybody else's. They get no privileged path in, which is the point: the path every
# other plug-in takes is the one the author exercises every time the program is run.
#
# THE NAMES COME FROM THE BUILD, not from a list restated here, and specifically from the CACHE
# rather than from the CMakeLists text. CMakeLists.txt declares NAMPRACK_PEDAL_BUNDLES beside the
# add_subdirectory() that produces them; reading it back out of CMakeCache.txt means this list is
# what the configure that produced these binaries actually decided, not what the source says it
# would decide. A sixth pedal upstream is then one edit away from shipping rather than three, and
# a list that quietly stopped matching -- which is how a release starts shipping four of five --
# cannot happen here. (Parsing the CMakeLists with sed was tried first and got this wrong on its
# first run: the set() is indented, and an anchored pattern matched nothing.)
PEDAL_BUNDLES="$(sed -n 's/^NAMPRACK_PEDAL_BUNDLES:INTERNAL=//p' "$BUILD/CMakeCache.txt" |
                 tr ';' ' ')"
[ -n "$PEDAL_BUNDLES" ] ||
  namp_dist_die "the build declared no NAMPRACK_PEDAL_BUNDLES, so it was configured with
-DNAMPRACK_BUILD_PEDALS=OFF. This release ships the pedals: reconfigure with it ON, or delete this
block deliberately -- silently shipping none is worse than failing here."

VALIDATOR="$BUILD/bin/Release/validator"
[ -x "$VALIDATOR" ] ||
  namp_dist_die "no SDK validator at $VALIDATOR. It is built by this same configure; a build tree
without it is one that was configured differently from the one this script expects."

mkdir -p "$PEDALDIR"
PEDAL_COUNT=0
for _pedal in $PEDAL_BUNDLES; do
  _src="$BUILD/VST3/Release/${_pedal}.vst3"
  [ -d "$_src" ] || namp_dist_die "${_pedal}.vst3 was not built at $_src.
Configure with -DNAMPRACK_BUILD_PEDALS=ON, or check the rations-pedals submodule is checked out:
  git submodule update --init rations-pedals"
  cp -r "$_src" "$PEDALDIR/"
  _pkg="$PEDALDIR/${_pedal}.vst3"
  namp_dist_prune_bundle_arches "$_pkg" "$ARCH"
  _so="$_pkg/Contents/${ARCH}-linux/${_pedal}.so"
  [ -f "$_so" ] || namp_dist_die "${_pedal}.vst3 holds no ${ARCH}-linux binary."
  strip --strip-unneeded "$_so"

  # FIVE COPIES MUST STAY FIVE COPIES. All five are built from the SAME sources, and GCC gives a
  # function-local static inside an inline function STB_GNU_UNIQUE binding, which glibc resolves
  # through a table shared by the whole link-map namespace -- RTLD_LOCAL does not scope it. Loaded
  # together into this rack, which is the ordinary case rather than an exotic one, they would be
  # sharing one another's statics. Hidden visibility and --exclude-libs,ALL are what prevent it,
  # and this is the assertion that they were actually applied to the binary that ships.
  namp_dist_no_unique "$_so" "${_pedal}.so"

  # THE ART IS IN THE BUNDLE. A pedal whose Resources did not come along draws flat rectangles,
  # and says nothing on stderr while it does.
  for _res in "Contents/Resources/img/pedal-$(printf '%s' "$_pedal" | sed 's/^Rations//' | tr 'A-Z' 'a-z').png" \
              Contents/Resources/fonts/Michroma-Regular.ttf \
              Contents/Resources/fonts/Roboto-Regular.ttf; do
    [ -f "$_pkg/$_res" ] || namp_dist_die "${_pedal}.vst3 is missing $_res."
  done

  namp_dist_abi_baseline "${_pedal}.so" "$_so"

  # THE SDK'S OWN VALIDATOR, ON THE STAGED AND STRIPPED BUNDLE rather than on the one in the build
  # tree. That distinction has caught things in this repository before: what ships is a copy that
  # has been pruned of other architectures and stripped, and it is the copy worth validating.
  # Cheap -- it is already built beside the rack -- and it is the broadest single statement anyone
  # can make about a VST3 without a DAW.
  _val_out="$("$VALIDATOR" "$_pkg" 2>&1 || true)"
  printf '%s' "$_val_out" | grep -qE '^Result: [0-9]+ tests passed, 0 tests failed' ||
    namp_dist_die "the SDK validator did not pass against ${_pedal}.vst3:
$(printf '%s' "$_val_out" | tail -20)"
  printf '  %-18s %s\n' "${_pedal}.vst3" "$(printf '%s' "$_val_out" | grep -E '^Result:')"

  PEDAL_COUNT=$((PEDAL_COUNT + 1))
done
[ "$PEDAL_COUNT" = "5" ] ||
  namp_dist_die "packaged $PEDAL_COUNT pedals, expected 5. A release that ships four of five
pedals looks complete and is not."

# The pedals' own NOTICE travels with them, because this archive redistributes their binaries and
# therefore the third-party components they vendor. Their LICENCE is this project's licence, but
# their attribution is not this project's attribution.
cp "$REPO/rations-pedals/NOTICE" "$PEDALDIR/NOTICE"

echo "staged the standalone and the pedals:"
echo "  rack/namp-rack"
echo "  rack/desktop/   (1 launcher, 4 icons)"
echo "  pedals/         ($PEDAL_COUNT plug-ins)"
