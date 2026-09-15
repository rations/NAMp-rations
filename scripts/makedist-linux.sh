#!/usr/bin/env bash
# Build the NAMp release for 64-bit Linux: ONE package holding everything this project ships.
#
# ONE DOWNLOAD, THREE THINGS IN IT, AND ONE install.sh THAT INSTALLS THEM ALL.
#
#   plugin/NAMp-rations.vst3   the amp as a plug-in for your DAW
#   plugin/rations.lv2         the same plug-in again, as LV2
#   rack/namp-rack             the same amp as a standalone application, with a rack that hosts
#                              other people's plug-ins around it
#   pedals/Rations*.vst3       the five pedals, as ordinary plug-ins any host will find
#
# WHY ONE PACKAGE RATHER THAN THREE. These are one amp, released together, versioned together and
# built from one tree. Three downloads would ask a user to work out which ones they want before
# they have seen any of them, and would let a person end up running a 0.3.0 plug-in beside a 0.2.9
# standalone -- two builds of one amp, silently different. The version check below makes that
# unrepresentable, and it can only do so because there is one package to label.
#
# HOW IT IS ASSEMBLED, AND WHY THE ASSERTIONS ARE NOT HERE. Each product builds and gates itself
# in its own scripts/stage-linux.sh, into its own subdirectory of one staging tree. Those
# assertions are artefact-specific -- "the plug-in must NOT link JACK", "the standalone MUST",
# "the LV2 must be self-contained", "the rack must export nothing at all" -- and they stay one
# call per line beside the product that owns them, because a flags table is how one of them
# quietly stops being asked while the run still says PASS. What lives HERE is only what is
# genuinely shared: the version, the licence files, the install script, the tarball.
#
# ONE BUILD DIRECTORY PER PRODUCT, and that is structural: smtg_add_vst3plugin writes to
# ${CMAKE_BINARY_DIR}/VST3/Release, so the two configures cannot share one. That is why this
# script calls two stage scripts rather than running one build.
#
# WINDOWS IS THE SAME SHAPE and is scripts/makedist-windows.sh: one installer, the same three
# things. macOS is the plug-in alone, because there is no macOS build of the rack; it is
# products/rations/scripts/makedist-mac.sh.
#
# WHAT THIS GATES ON, AND WHAT IT DELIBERATELY DOES NOT. Everything the stage scripts measure
# works on any machine: no captures, no JACK server, no X display. The proofs that DO need those
# (rations_offline, ir-gate.sh, switch-gate.sh, namp_chaincheck, live audio at 128 frames) are run
# by hand against a rig that has them, and switch-gate.sh in particular restarts jackd, which is
# not a thing a packaging script may do to someone's session.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"

# shellcheck source=dist-common.sh
. "$REPO/scripts/dist-common.sh"

VERSION="$(namp_dist_release_version "$REPO")"

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/pkg"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

# WHERE THE STAGE SCRIPTS REPORT AN ABI OVERSHOOT. They run as separate processes, so the variable
# namp_dist_abi_baseline sets cannot reach namp_dist_mark down here; this file is how it does.
# Outside PKGDIR, so it is not packaged.
export NAMP_DIST_ABI_OVER_FILE="$STAGEDIR/abi-overshoot"
: > "$NAMP_DIST_ABI_OVER_FILE"

echo "== NAMp $VERSION, Linux $ARCH =="
echo
echo "-- NAMp Rations (the plug-in) --"
"$REPO/products/rations/scripts/stage-linux.sh" "$PKGDIR"
echo
echo "-- NAMp Rack (the standalone) and the pedals --"
"$REPO/products/rack/scripts/stage-linux.sh" "$PKGDIR"
echo

# EVERY PART ARRIVED. The two stage scripts each assert their own artefacts in detail; what
# neither can assert is that the OTHER one ran. A tarball missing a whole product is the one
# failure mode created by splitting the work in two, so it is checked where the two meet.
namp_dist_require_dirs "$PKGDIR" "the release package" \
  plugin/NAMp-rations.vst3 \
  plugin/rations.lv2 \
  pedals
namp_dist_require_files "$PKGDIR" "the release package" \
  rack/namp-rack \
  rack/desktop/namp-rack.desktop \
  pedals/NOTICE

# --- licence, attribution ----------------------------------------------------
# The root NOTICE covers all three products; per-product copies were merged into it so that a
# section applying to only one says so, rather than two documents drifting apart. The pedals'
# own NOTICE travels with their binaries as well, because this archive redistributes them and
# their attribution is theirs rather than this project's; the stage script put it there.
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

# --- the install script ------------------------------------------------------
{
  cat <<'EOF'
#!/usr/bin/env bash
# Install (or remove) NAMp for the current user -- all of it, in one go. Nothing here needs root,
# and nothing is installed outside your home directory.
#
#   plugin/NAMp-rations.vst3  -> ~/.vst3                                 (the plug-in, for a DAW)
#   plugin/rations.lv2        -> ~/.lv2                                  (the same plug-in as LV2)
#   pedals/Rations*.vst3      -> ~/.vst3                                 (the five pedals)
#   rack/namp-rack            -> ~/.local/bin                            (the standalone)
#   rack/desktop/*.desktop    -> ~/.local/share/applications             (its menu entry)
#   rack/desktop/*.png        -> ~/.local/share/icons/hicolor/<size>/apps (its icon)
#
# The standalone is self-contained: the amp, its art and its fonts are inside the binary, so there
# is nothing else for it to find. The pedals go to ~/.vst3 because that is one of the directories
# the standalone's own scan looks in, so they appear in its plug-in list on the next rescan -- and
# so does every other VST3 you already have there. They are ordinary plug-ins: your DAW will find
# them too, and nothing about them is special to this program.
EOF
  namp_install_preamble plugin desktop
  cat <<'EOF'

if [ "${1:-}" = "--uninstall" ]; then
  rm -rf "$VST3_DIR/NAMp-rations.vst3"
  rm -rf "$LV2_DIR/rations.lv2"
  rm -f "$BIN_DIR/namp-rack"
  for _p in "$HERE"/pedals/*.vst3; do
    [ -d "$_p" ] || continue
    rm -rf "$VST3_DIR/$(basename "$_p")"
  done
EOF
  namp_install_icons_remove namp-rack
  cat <<'EOF'
  refresh
  echo "NAMp removed."
  echo
  echo "Your settings are NOT removed, because they are yours:"
  echo "  ~/.config/NAMp-rations   the plug-in's settings"
  echo "  ~/.config/NAMp-Rack      the standalone's saved racks, captures and device choice"
  echo "  ~/.cache/NAMp-Rack       its plug-in scan cache (safe to delete at any time)"
  echo "Delete them by hand if you want them gone."
  exit 0
fi

mkdir -p "$VST3_DIR" "$LV2_DIR" "$BIN_DIR" "$APP_DIR"

# THE PLUG-IN. Replaced rather than merged: a .vst3 is a DIRECTORY, so copying over an older one
# would leave whatever the old version had and the new one does not, and the result is a bundle
# that is neither release.
rm -rf "$VST3_DIR/NAMp-rations.vst3"
cp -r "$HERE/plugin/NAMp-rations.vst3" "$VST3_DIR/"

# THE LV2. A copy under /usr/lib/lv2 or /usr/local/lib/lv2 would SHADOW this one, and the only
# symptom is a host running a version you did not install. Say so rather than leaving it to be
# discovered.
rm -rf "$LV2_DIR/rations.lv2"
cp -r "$HERE/plugin/rations.lv2" "$LV2_DIR/"
for SHADOW in /usr/local/lib/lv2/rations.lv2 /usr/lib/lv2/rations.lv2; do
  if [ -e "$SHADOW" ]; then
    echo "Note: $SHADOW exists and will be used INSTEAD of the copy just installed."
    echo "      Remove it (it needs root) if you want this one."
  fi
done

# THE PEDALS, the same way and for the same reason.
PEDAL_N=0
for _p in "$HERE"/pedals/*.vst3; do
  [ -d "$_p" ] || continue
  rm -rf "$VST3_DIR/$(basename "$_p")"
  cp -r "$_p" "$VST3_DIR/"
  PEDAL_N=$((PEDAL_N + 1))
done

# THE STANDALONE.
install -m 755 "$HERE/rack/namp-rack" "$BIN_DIR/namp-rack"
EOF
  namp_install_icons_install namp-rack rack/desktop
  cat <<'EOF'
refresh

echo "Installed:"
echo "  plug-in     $VST3_DIR/NAMp-rations.vst3"
echo "  lv2         $LV2_DIR/rations.lv2"
echo "  pedals      $VST3_DIR  ($PEDAL_N plug-ins)"
echo "  standalone  $BIN_DIR/namp-rack"
echo "  launcher    $APP_DIR/namp-rack.desktop"
echo
echo "In a DAW: rescan plug-ins. The VST3 and the LV2 are the same amp in two wrappers, so"
echo "use whichever your host prefers and ignore the other - there is nothing to choose"
echo "between them in sound or in features."
echo
echo "Standalone: start a JACK server first, or run a PipeWire desktop, which provides one."
EOF
  namp_install_path_note namp-rack
} > "$PKGDIR/install.sh"
chmod +x "$PKGDIR/install.sh"

# --- the notes ---------------------------------------------------------------
cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp ${VERSION} - a four-channel Neural Amp Modeler amp head, for Linux

One amp, three ways to run it, and one install script that installs all of them.

    plugin/NAMp-rations.vst3   the amp as a plug-in, for your DAW
    plugin/rations.lv2         the same plug-in again, as LV2
    rack/namp-rack             the same amp standalone, in its own window on
                               JACK, with a rack that hosts other people's VST3
                               and LV2 plug-ins before and after it
    pedals/                    five pedals, as ordinary plug-ins

The plug-in and the standalone are the SAME amp from the SAME source, released
together and versioned together. The difference is the surround: the plug-in has
a five-pedal pedalboard built into its panel, and the standalone leaves that out
because it hosts your own plug-ins instead - including the five in pedals/, which
reach it by exactly the route everybody else's plug-in takes.

Install
-------
    ./install.sh

Everything goes under your home directory and nothing needs root:

    ~/.vst3/NAMp-rations.vst3          the plug-in
    ~/.lv2/rations.lv2                 the LV2 build
    ~/.vst3/Rations*.vst3              the five pedals
    ~/.local/bin/namp-rack             the standalone
    ~/.local/share/applications/       its menu entry, with an icon

Then rescan plug-ins in your DAW. To remove it all again:

    ./install.sh --uninstall

You do not have to install anything to try the standalone - it runs where you
extracted it:

    ./rack/namp-rack

and you can copy any of the bundles into place by hand instead; there is nothing
else to install and no files outside the directories listed above.

Captures
--------
NAMp ships NO captures - it plays yours, and it wants four sets of them.

In the plug-in, click "Captures, MIDI, Settings", top right. In the standalone,
open the setup page. Either way, point each channel's loader at a DIRECTORY of
.nam files, or at a single .nam. That folder's name becomes the channel's name on
the front panel, and you can type over it if you would rather call it something
else.

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

A channel with nothing loaded is silent rather than broken; a fresh instance has
four of them. Captures must be feed-forward (WaveNet or ConvNet); an LSTM capture
is refused rather than silently accepted.

The four channels
-----------------
Exactly one channel sounds at a time. Click its bat switch, or learn a MIDI
footswitch to it, and the change is instant and silent even mid-note - which is
the whole reason this project exists.

The settings page also carries a trim per channel (for when a high-gain channel
reads louder than a clean one at the same measured loudness), the MIDI learn
rows, and the output section: Raw / Normalized / Calibrated, plus input
calibration. Normalized is the default.

MIDI learn: a learned CC or Program Change answers on ANY MIDI channel, because
both arrive at a VST3 plug-in as parameter changes and the channel is already
gone by then. Only a learned NOTE can be pinned to one MIDI channel.

The rest of the panel
---------------------
Shared Threshold / Bass / Middle / Treble, Input and Output, each reading its
value under the dial. BYPASS, EQ and GATE switch out the whole chain, the tone
stack and the noise gate. The icon beside the settings button is Slim, which
trades model size for CPU - it appears only when your captures can actually use
it. A cabinet section loads one or two impulse responses with a blend between
them.

The file picker is drawn inside the panel rather than being a GTK or Qt dialog,
so it looks like the rest of the program and cannot clash with whatever toolkit
your DAW or desktop is built on.

The five pedals
---------------
    RationsBoost      a Tube Screamer-style overdrive
    RationsChorus     two modulated taps per channel
    RationsFlanger    swept comb with feedback
    RationsDelay      tempo-syncable, optional ping-pong
    RationsReverb     a Freeverb-lineage room

install.sh puts them in ~/.vst3, so they turn up in the standalone's plug-in list
after a rescan, and in your DAW's as well. They are also built into the plug-in's
own pedalboard page, so you do not need them installed to use that - the separate
bundles are for the standalone and for any other host.

Running the standalone
----------------------
It is a JACK application. Start a JACK server first (qjackctl, or e.g.
"jackd -R -d alsa -r 48000 -p 256"), or run a PipeWire desktop, which provides
one. With no server it still opens, so you can set your captures and your rack
up, but it makes no sound and says so.

It registers these ports:

    namp-rack:in       your guitar
    namp-rack:out_l    \\ the amp, in stereo from the cabinet onwards
    namp-rack:out_r    /
    namp-rack:midi_in  a MIDI footswitch

The audio ports are connected to the first physical capture and playback ports it
finds. The MIDI port is left UNCONNECTED on purpose: which of your MIDI devices
is the footswitch is not something to guess at, and the wrong guess has a
keyboard changing amp channels. Connect it in your patchbay.

Plug-ins load before the amp and after it, in the order you put them. The pre-amp
section is mono and the post-amp section is stereo, because that is where the amp
makes it stereo. Scanning happens in a separate process on purpose: a plug-in that
crashes while it is being examined takes that process with it and is recorded as
bad rather than retried, so one broken bundle on your disk cannot stop the rack
from starting. Adding and removing plug-ins while audio is running is safe and is
not heard.

Requirements
------------
The plug-in, the LV2 and the pedals need cairo, freetype2, fontconfig and libX11,
which a desktop Linux install already has. None of them links JACK.

The standalone needs those plus lilv and suil (for hosting LV2 plug-ins) and the
JACK client library, with a running JACK server. On Debian/Devuan/Ubuntu:

    sudo apt install jackd2 liblilv-0-0 libsuil-0-0

On a PipeWire desktop, "pipewire-jack" provides the JACK library and server.

Nothing here needs a -dev package; those are only for building from source.

There is no 32-bit build. The Windows release is a separate download and holds
the same three things.

Licence
-------
MIT, all of it. See LICENSE, and NOTICE for third-party attribution; the pedals
carry their own in pedals/NOTICE, covering components they use and this one does
not.

One binary this project ships is NOT MIT, and it is not in this archive: the
WINDOWS standalone, which hosts ASIO and is therefore GPLv3. Nothing on Linux
contains ASIO code. If that matters to you, the Windows download explains it in
full.
EOF

PKGNAME="NAMp-${VERSION}$(namp_dist_mark)-linux-${ARCH}"
mv "$PKGDIR" "$STAGEDIR/$PKGNAME"
# The epoch makes the TARBALL reproducible, not just the binaries in it -- see namp_dist_tarball.
namp_dist_tarball "$STAGEDIR" "$PKGNAME" "$REPO/dist" "$(namp_dist_epoch "$REPO")"
