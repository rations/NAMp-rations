#!/usr/bin/env bash
# Build NAMp Rations for 64-bit Linux, gate the result, and stage it for the release.
#
# THIS SCRIPT PRODUCES NO ARCHIVE. The release is ONE package holding all three things this
# project ships -- the plug-in, the standalone and the pedals -- and it is assembled by
# scripts/makedist-linux.sh at the repository root, which calls this and the rack's stage script
# in turn. Splitting it this way keeps the thing that must not be shared unshared: every
# assertion below is about a specific artefact, spelled out one call per line, and lives beside
# the product that owns it. What IS shared -- the install script, the tarball, the licence files,
# the version -- is written once at the root instead of twice here.
#
# WHAT IT STAGES, into <stagedir>/plugin/:
#
#   NAMp-rations.vst3   the plug-in, for a DAW. It links cairo, FreeType, fontconfig and libX11
#                       and NOTHING else - in particular it does not link JACK, which is checked
#                       below rather than assumed.
#   rations.lv2         the same plug-in again, as LV2, for a host that wants that format. It is
#                       not a second plug-in and not a second copy of the DSP: both shared objects
#                       inside it host the very same RationsProcessor and RationsEditorView the
#                       VST3 bundle carries, wrapped for LV2's four callbacks (lv2/rationslv2.h
#                       says why, and what it cost). It is self-contained - it carries its own art
#                       and needs the VST3 bundle no more than any other host's format does.
#
# WHERE THE STANDALONE WENT. namp-rations-standalone is still BUILT, still JACK-guarded, and is
# still what switch-gate.sh and rations_jackcheck drive; it is the only rig that catches a
# host-side parameter-echo bug, so losing it would lose a test nothing else performs. It is simply
# not PACKAGED any more: the standalone in the release is NAMp Rack, which is this same amp in its
# own window with a rack around it, and shipping two JACK applications whose windows look alike
# would make a user pick between them with nothing to go on.
#
# WHAT THIS GATES ON, AND WHAT IT DELIBERATELY DOES NOT. Everything here is measured on the built
# binaries - what they link, what they export, what is inside the bundle, and what runtime they ask
# their loader for. All of it works on any machine: no captures, no JACK server, no X display. The
# proofs that DO need those (rations_offline, scripts/ir-gate.sh, scripts/switch-gate.sh) are run
# by hand against a rig that has them, and switch-gate.sh in particular restarts jackd, which is
# not a thing a packaging script may do to someone's session.
set -euo pipefail

# PRODUCT is this product's own directory; REPO is the REPOSITORY root, and they have been
# different places since the two products moved under products/. This script needs both: the build
# directory, the CMake source root and the shared packaging machinery are the repository's, while
# the resources and the lists file that carries the version are this product's. Naming the product
# directory "REPO" is what hid three separate breakages here, so it is named for what it is.
PRODUCT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$PRODUCT/../.." && pwd)"
BUILD="${NAMP_BUILD_DIR:-${RATIONS_BUILD_DIR:-$REPO/build}}"
ARCH="$(uname -m)"

# shellcheck source=../../../scripts/dist-common.sh
. "$REPO/scripts/dist-common.sh"

STAGE="${1:-}"
[ -n "$STAGE" ] && [ -d "$STAGE" ] ||
  namp_dist_die "usage: stage-linux.sh <stagedir>   (an existing directory to stage into)
This script stages; it packages nothing. Run scripts/makedist-linux.sh to build a release."

# ONE PRODUCT PER BUILD DIRECTORY, and that is structural rather than tidiness:
# smtg_add_vst3plugin writes to ${CMAKE_BINARY_DIR}/VST3/Release, so two products configured into
# one binary directory would write two bundles to one path.
cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

PKGDIR="$STAGE/plugin"
mkdir -p "$PKGDIR"

# --- the plug-in ------------------------------------------------------------
BUNDLE="$BUILD/VST3/Release/NAMp-rations.vst3"
[ -d "$BUNDLE" ] || namp_dist_die "VST3 bundle not found at $BUNDLE"
cp -r "$BUNDLE" "$PKGDIR/"
PKGBUNDLE="$PKGDIR/NAMp-rations.vst3"

namp_dist_prune_bundle_arches "$PKGBUNDLE" "$ARCH"

PLUGIN_SO="$PKGBUNDLE/Contents/${ARCH}-linux/NAMp-rations.so"
if [ ! -f "$PLUGIN_SO" ]; then
  echo "no NAMp-rations.so inside $PKGBUNDLE/Contents/${ARCH}-linux/" >&2
  echo "The bundle layout is wrong; no host can load this." >&2
  find "$PKGBUNDLE" -type f >&2
  exit 1
fi

# RESOURCES. This project embeds NO fallback art: core/gfx/resourcestore.h states that its built-in
# table is always empty here by design. So a bundle that reaches a user without Contents/Resources
# draws flat rectangles and says so only on stderr, which nobody reads. This assertion is the
# entire safety net, and it is the same list stage-windows.sh checks. The SVG in the list is
# File.svg, which the IR and capture loader rows draw; it was Gear.svg until the settings control
# became a labelled button and stopped drawing the gear at all.
namp_dist_require_files "$PKGBUNDLE" "the bundle" \
  Contents/Resources/img/head.png \
  Contents/Resources/img/pedal-boost.png \
  Contents/Resources/img/File.svg \
  Contents/Resources/fonts/Michroma-Regular.ttf \
  Contents/Resources/fonts/Roboto-Regular.ttf

strip --strip-unneeded "$PLUGIN_SO"

# --- the LV2 bundle ---------------------------------------------------------
# A shipped component like the plug-in, so a build that skipped it is refused rather than quietly
# producing a ONE-product tarball that claims to hold two. (It claimed to hold three until the
# standalone stopped being packaged; the check is restated for the new count rather than relaxed,
# because it is exactly the check that catches this class of edit.)
LV2BUNDLE="$BUILD/lv2/rations.lv2"
if [ ! -d "$LV2BUNDLE" ]; then
  echo "rations.lv2 was not built - install the LV2 development files (lv2-dev)" >&2
  echo "and re-run, or the release would ship the VST3 only." >&2
  exit 1
fi
cp -r "$LV2BUNDLE" "$PKGDIR/"
PKGLV2="$PKGDIR/rations.lv2"
strip --strip-unneeded "$PKGLV2/rations.so" "$PKGLV2/rations_ui.so"

# --- gates ------------------------------------------------------------------
# THE PLUG-IN MUST NOT LINK THE AUDIO BACKEND. Nothing in this archive hosts JACK any more; a
# bundle that linked it would refuse to load on every machine without libjack installed, which is
# most of them, and would do it silently - the host simply reports no such plug-in.
namp_dist_must_not_link "$PLUGIN_SO" 'libjack' "the VST3 bundle" \
  "Nothing in this archive may; the standalone that once did is no longer packaged."

# ENTRY POINTS. The three a VST3 host calls on Linux, by presence rather than as an exact set.
namp_dist_exports_all "$PLUGIN_SO" "the VST3 bundle" GetPluginFactory ModuleEntry ModuleExit
namp_dist_no_unique "$PLUGIN_SO" "the VST3 bundle"

# THE LV2 BUNDLE'S SHAPE. The same questions the VST3 is asked, plus two of its own.
#
# EXACTLY ONE EXPORT EACH, and here "exactly" is affordable where it was not for the VST3: these
# are built from our own sources with -fvisibility=hidden and --exclude-libs,ALL and carry no
# module entry point, so anything else in the dynamic table is a symbol a second plug-in in the
# same host process can be merged with. This tree ships the VST3 and the LV2 of the SAME plug-in
# on Linux, so a host with both installed is exactly that collision path.
namp_dist_exports_exactly "$PKGLV2/rations.so"    "rations.lv2/rations.so"    lv2_descriptor
namp_dist_exports_exactly "$PKGLV2/rations_ui.so" "rations.lv2/rations_ui.so" lv2ui_descriptor
namp_dist_no_unique "$PKGLV2/rations.so"    "rations.lv2/rations.so"
namp_dist_no_unique "$PKGLV2/rations_ui.so" "rations.lv2/rations_ui.so"

# NEITHER HALF MAY LINK JACK, and the DSP half may not link the editor's libraries either: it
# draws nothing, and a headless host has to be able to run it with no X server present at all.
namp_dist_must_not_link "$PKGLV2/rations.so" 'libjack' "rations.lv2/rations.so" \
  "Nothing in this archive may."
namp_dist_must_not_link "$PKGLV2/rations.so" 'libX11|libcairo' "rations.lv2/rations.so" \
  "The DSP half draws nothing."

# THE BUNDLE'S CONTENTS. Its Turtle and the five load-bearing resources, which the UI loads from
# the bundle_path a host hands it rather than from any computed location.
namp_dist_require_files "$PKGLV2" "rations.lv2" \
  manifest.ttl rations.ttl \
  fonts/Michroma-Regular.ttf fonts/Roboto-Regular.ttf \
  img/head.png img/cabinet.png img/dial.png img/File.svg

# THE RUNTIME CEILING. Every shipped ELF, against the declared release baseline.
namp_dist_abi_baseline "NAMp-rations.vst3"        "$PLUGIN_SO"
namp_dist_abi_baseline "rations.lv2/rations.so"    "$PKGLV2/rations.so"
namp_dist_abi_baseline "rations.lv2/rations_ui.so" "$PKGLV2/rations_ui.so"

echo "staged the plug-in:"
echo "  plugin/NAMp-rations.vst3"
echo "  plugin/rations.lv2"
