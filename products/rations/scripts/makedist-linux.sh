#!/usr/bin/env bash
# Build NAMp Rations for 64-bit Linux and package the release tarball into dist/.
#
# TWO PRODUCTS, ONE TARBALL.
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
# not PACKAGED any more: the shipped Linux application is now NAMp Rack, which hosts this same amp
# along with other people's plug-ins around it, and shipping two JACK applications whose windows
# look alike would make a user pick between them with nothing to go on. The desktop entry and the
# four icons went with it - a launcher for a binary that is not in the archive is worse than none.
#
# WINDOWS IS A SEPARATE RELEASE and ships the plug-in only; see scripts/makedist-windows.sh.
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
# directory, the CMake source root, the shared packaging machinery and the top-level LICENCE are
# the repository's, while the resources, NOTICE and README are this product's. Naming the product
# directory "REPO" is what hid three separate breakages here, so it is named for what it is.
PRODUCT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$PRODUCT/../.." && pwd)"
BUILD="${NAMP_BUILD_DIR:-${RATIONS_BUILD_DIR:-$REPO/build}}"
ARCH="$(uname -m)"

# shellcheck source=../../../scripts/dist-common.sh
. "$REPO/scripts/dist-common.sh"

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

VERSION="$(namp_dist_version "$PRODUCT/CMakeLists.txt")"

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/pkg"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

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
# entire safety net, and it is the same list makedist-windows.sh checks. The SVG in the list is
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

# --- licence, attribution, installer ----------------------------------------
cp "$PRODUCT/NOTICE" "$REPO/LICENSE" "$PRODUCT/README.md" "$PKGDIR/"

{
  cat <<'EOF'
#!/usr/bin/env bash
# Install (or remove) NAMp Rations for the current user. Nothing here needs root, and nothing is
# installed outside your home directory.
#
#   NAMp-rations.vst3   -> ~/.vst3   (the plug-in, for a DAW)
#   rations.lv2         -> ~/.lv2    (the same plug-in, as LV2)
#
# There is no menu entry, because there is no application here to launch: this archive is the
# plug-in in two formats. The standalone application is NAMp Rack, which is its own download.
EOF
  namp_install_preamble plugin
  cat <<'EOF'

if [ "${1:-}" = "--uninstall" ]; then
  rm -rf "$VST3_DIR/NAMp-rations.vst3"
  rm -rf "$LV2_DIR/rations.lv2"
  echo "NAMp Rations removed."
  echo "Your settings are NOT removed: anything under ~/.config/NAMp-rations belongs to you."
  echo "Delete it by hand if you want it gone."
  exit 0
fi

mkdir -p "$VST3_DIR" "$LV2_DIR"
rm -rf "$VST3_DIR/NAMp-rations.vst3"
cp -r "$HERE/NAMp-rations.vst3" "$VST3_DIR/"
# A copy under /usr/lib/lv2 or /usr/local/lib/lv2 would SHADOW this one, and the only symptom is
# a host running a version you did not install. Say so rather than leaving it to be discovered.
rm -rf "$LV2_DIR/rations.lv2"
cp -r "$HERE/rations.lv2" "$LV2_DIR/"
for SHADOW in /usr/local/lib/lv2/rations.lv2 /usr/lib/lv2/rations.lv2; do
  if [ -e "$SHADOW" ]; then
    echo "Note: $SHADOW exists and will be used INSTEAD of the copy just installed."
    echo "      Remove it (it needs root) if you want this one."
  fi
done

echo "Installed:"
echo "  plug-in     $VST3_DIR/NAMp-rations.vst3"
echo "  lv2         $LV2_DIR/rations.lv2"
echo
echo "Rescan plug-ins in your DAW to pick up the VST3 or the LV2 - they are the same amp,"
echo "so install whichever your host prefers and ignore the other."
echo
echo "To remove it again:  ./install.sh --uninstall"
EOF
} > "$PKGDIR/install.sh"
chmod +x "$PKGDIR/install.sh"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rations ${VERSION} - a four-channel Neural Amp Modeler amp head for Linux

This archive holds two things, and they are the same amp twice:

    NAMp-rations.vst3   the plug-in, for your DAW
    rations.lv2         the same plug-in as LV2, if your host prefers that

They are the same DSP and the same panel in two wrappers, so install whichever
your host handles best and ignore the other; there is nothing to choose between
them in sound or in features.

If you want the amp WITHOUT a DAW - its own window, on JACK, and a rack that
hosts other people's VST3 and LV2 plug-ins around it - that is NAMp Rack, and
it is a separate download.

Install
-------
    ./install.sh

Everything goes under your home directory and nothing needs root:

    ~/.vst3/NAMp-rations.vst3    the plug-in
    ~/.lv2/rations.lv2           the LV2 build

Then rescan plug-ins in your DAW. To remove it all again: ./install.sh --uninstall

You can also just copy either one into place by hand; there is nothing else to
install and no files outside those two directories.

Captures
--------
NAMp Rations ships NO captures - it plays yours, and it wants four sets
of them.

Click the "Captures, MIDI, Settings" button, top right, to open the settings
page. The top section has one loader per channel: point each at a DIRECTORY of
.nam files, or at a single .nam. That folder's name becomes the channel's name
on the front panel, and you can type over it if you would rather call it
something else.

Each channel's dial then sweeps that whole bank continuously - no reload, no
click, no dialog. One click of the mouse wheel on a channel dial is exactly one
capture. Rest on one and you are playing that capture exactly; in between, you
are hearing the two either side blended.

Captures are ordered by the number in the filename (the LAST run of digits, so
"GAIN 2" comes before "GAIN 10"), then anything ending in MAX, then anything
with no number at all, alphabetically. So capture your amp at each mark of its
own gain control and put that mark last in the name:

    MyAmp - crunch - GAIN 1.nam
    MyAmp - crunch - GAIN 2.nam
    ...
    MyAmp - crunch - GAIN MAX.nam

and that channel's dial is that amp's gain control, at the amp's own spacing.

A channel with nothing loaded is silent rather than broken; a fresh instance
has four of them. Captures must be feed-forward (WaveNet or ConvNet); an LSTM
capture is refused rather than silently accepted.

The four channels
-----------------
Exactly one channel sounds at a time. Click its bat switch, or learn a MIDI
footswitch to it on the settings page - the switch is instant and silent even
mid-note, which is the whole reason this plug-in exists.

The settings page also carries a trim per channel (for when a high-gain channel
reads louder than a clean one at the same measured loudness), the MIDI learn
rows - four for the channels, five for the pedals - and the output section:
Raw / Normalized / Calibrated, plus input calibration. Normalized is the
default.

MIDI learn: a learned CC or Program Change answers on ANY MIDI channel, because
both arrive at a VST3 plug-in as parameter changes and the channel is already
gone by then. Only a learned NOTE can be pinned to one MIDI channel.

The rest of the panel
---------------------
Shared Threshold / Bass / Middle / Treble, Input and Output, each reading its
value under the dial. BYPASS, EQ and GATE switch out the whole chain, the tone
stack and the noise gate. The icon left of the settings button is Slim, which
trades model size for CPU - it appears only when your captures can actually use
it.

Two more pages, reached by the buttons at the bottom: a cabinet page that loads
one or two impulse responses with a blend between them, and a pedalboard of
five pedals - Boost and Chorus before the amp, Flanger, Delay and Reverb after
it.

The file picker is drawn inside the plug-in rather than being a GTK or Qt
dialog, so it looks like the rest of the panel and cannot clash with whatever
toolkit your DAW is built on.

Requirements
------------
Both formats need cairo, freetype2, fontconfig and libX11, which a desktop
Linux install already has. Neither links JACK.

Nothing here needs a -dev package; those are only for building from source.

There is no 32-bit build. The Windows release is a separate download and ships
the VST3 only.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution.
EOF

PKGNAME="NAMp-rations-${VERSION}$(namp_dist_mark)-linux-${ARCH}"
mv "$PKGDIR" "$STAGEDIR/$PKGNAME"
namp_dist_tarball "$STAGEDIR" "$PKGNAME" "$PRODUCT/dist"
