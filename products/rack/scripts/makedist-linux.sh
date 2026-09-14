#!/usr/bin/env bash
# Build NAMp Rack for 64-bit Linux and package the release tarball into dist/.
#
# ONE PROGRAM, AND IT IS SELF-CONTAINED.
#
#   namp-rack           the amp head as a JACK application, with a rack that hosts other people's
#                       VST3 and LV2 plug-ins before and after it. The amp, its art and its fonts
#                       are linked IN -- there is no bundle beside it to find and nothing to
#                       install first -- and plug-in discovery re-execs this same binary in
#                       scan-child mode, so a bundle that crashes during a scan takes a throwaway
#                       process with it instead of the rack. That is why the archive holds one
#                       executable and a launcher rather than a program and its plug-in.
#
# WHAT IS NOT IN HERE. NAMp-Rack-Amp.vst3 is built by every configure, and it is NOT packaged:
# it exists so the SDK validator has something to validate, which is by a wide margin the cheapest
# gate on the amp. The amp as a plug-in for a DAW is NAMp Rations, which is its own release and
# ships both VST3 and LV2. Shipping a second, pedal-less plug-in that does a subset of what that
# one does would be asking a user to choose with nothing to go on.
#
# THE SAME AMP IS IN BOTH RELEASES, and that is the point of the repository rather than a
# duplication to apologise for: one tree, so a fix to the amp lands once.
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

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNAMP_PRODUCT=rack -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

VERSION="$(namp_dist_version "$PRODUCT/CMakeLists.txt")"

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/pkg"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

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
RACK_STRINGS="$STAGEDIR/namp-rack.strings"
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

# --- licence, attribution, installer ----------------------------------------
cp "$PRODUCT/NOTICE" "$REPO/LICENSE" "$PKGDIR/"

{
  cat <<'EOF'
#!/usr/bin/env bash
# Install (or remove) NAMp Rack for the current user. Nothing here needs root, and nothing is
# installed outside your home directory.
#
#   namp-rack            -> ~/.local/bin                              (the program)
#   namp-rack.desktop    -> ~/.local/share/applications               (the menu entry)
#   namp-rack-<size>.png -> ~/.local/share/icons/hicolor/<size>/apps  (its icon)
#
# The program is self-contained: the amp, its art and its fonts are inside the binary, so there is
# nothing else to install and nothing for it to find.
EOF
  namp_install_preamble desktop
  cat <<'EOF'

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$BIN_DIR/namp-rack"
EOF
  namp_install_icons_remove namp-rack
  cat <<'EOF'
  refresh
  echo "NAMp Rack removed."
  echo "Your settings are NOT removed: ~/.config/NAMp-Rack holds your saved racks, which"
  echo "captures were loaded and your audio device choice, and ~/.cache/NAMp-Rack holds the"
  echo "plug-in scan cache. Delete them by hand if you want them gone."
  exit 0
fi

mkdir -p "$BIN_DIR" "$APP_DIR"
install -m 755 "$HERE/namp-rack" "$BIN_DIR/namp-rack"
EOF
  namp_install_icons_install namp-rack
  cat <<'EOF'
refresh

echo "Installed:"
echo "  program     $BIN_DIR/namp-rack"
echo "  launcher    $APP_DIR/namp-rack.desktop"
echo
echo "Start a JACK server first, or run a PipeWire desktop, which provides one."
EOF
  namp_install_path_note namp-rack
} > "$PKGDIR/install.sh"
chmod +x "$PKGDIR/install.sh"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rack ${VERSION} - a four-channel Neural Amp Modeler amp head, and a rack
for your plug-ins, for Linux

This archive holds one program:

    namp-rack     the amp in its own window, on JACK, with a rack that hosts
                  other people's VST3 and LV2 plug-ins before and after it

It is self-contained. The amp, its art and its fonts are inside the binary, so
there is nothing to install first and nothing for it to go looking for.

If you want the amp inside your DAW instead, as a plug-in, that is NAMp
Rations - the same amp, with a five-pedal pedalboard built in, as both VST3 and
LV2. It is a separate download.

Install
-------
    ./install.sh

Everything goes under your home directory and nothing needs root:

    ~/.local/bin/namp-rack                 the program
    ~/.local/share/applications/           a menu entry, with an icon

To remove it again: ./install.sh --uninstall

You can also just run it where you extracted it, with no installation at all:

    ./namp-rack

Running it
----------
It is a JACK application. Start a JACK server first (qjackctl, or e.g.
"jackd -R -d alsa -r 48000 -p 256"), or run a PipeWire desktop, which provides
one. With no server it still opens, so you can set your captures and your rack
up, but it makes no sound and says so.

It registers these ports:

    namp-rack:in       your guitar
    namp-rack:out_l    \\ the amp, in stereo from the cabinet onwards
    namp-rack:out_r    /
    namp-rack:midi_in  a MIDI footswitch

The audio ports are connected to the first physical capture and playback ports
it finds. The MIDI port is left UNCONNECTED on purpose: which of your MIDI
devices is the footswitch is not something to guess at, and the wrong guess has
a keyboard changing amp channels. Connect it in your patchbay.

Your saved racks, which captures were loaded, your audio device choice and your
MIDI bindings live in ~/.config/NAMp-Rack. The plug-in scan cache lives in
~/.cache/NAMp-Rack and can be deleted at any time; it is rebuilt by a rescan.

The rack
--------
Plug-ins load before the amp and after it, in the order you put them. The
pre-amp section is mono and the post-amp section is stereo, because that is
where the amp makes it stereo.

Scanning happens in a separate process on purpose: a plug-in that crashes while
it is being examined takes that process with it and is recorded as bad rather
than retried, so one broken bundle on your disk cannot stop the rack from
starting. A plug-in that misbehaves once it is RUNNING is a different matter -
it is on the audio thread and no host can prevent that - so the rack counts what
each one does and shows you which one it was.

Adding and removing plug-ins while audio is running is safe and is not heard.

Captures
--------
NAMp Rack ships NO captures - it plays yours, and it wants four sets of them.

Open the setup page and point each channel's loader at a DIRECTORY of .nam
files, or at a single .nam. That folder's name becomes the channel's name on the
front panel, and you can type over it if you would rather call it something else.

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
four of them. Captures must be feed-forward (WaveNet or ConvNet); an LSTM
capture is refused rather than silently accepted.

The four channels
-----------------
Exactly one channel sounds at a time. Click its bat switch, or learn a MIDI
footswitch to it on the setup page - the switch is instant and silent even
mid-note, which is the whole reason this program exists.

The rest of the panel: shared Threshold / Bass / Middle / Treble, Input and
Output, each reading its value under the dial. BYPASS, EQ and GATE switch out the
whole chain, the tone stack and the noise gate. The icon beside the setup button
is Slim, which trades model size for CPU - it appears only when your captures can
actually use it. The setup page also carries the cabinet section, which blends
one or two impulse responses.

The file picker is drawn inside the program rather than being a GTK or Qt dialog,
so it looks like the rest of the panel and cannot clash with whatever toolkit
your desktop is built on.

Requirements
------------
cairo, freetype2, fontconfig, libX11, lilv and suil, which a desktop Linux
install with a plug-in host on it already has, plus the JACK client library
(libjack.so.0) and a running JACK server. On Debian/Devuan/Ubuntu:

    sudo apt install jackd2 liblilv-0-0 libsuil-0-0

On a PipeWire desktop, "pipewire-jack" provides the JACK library and server.

Nothing here needs a -dev package; those are only for building from source.

There is no 32-bit build.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution.
EOF

PKGNAME="namp-rack-${VERSION}$(namp_dist_mark)-linux-${ARCH}"
mv "$PKGDIR" "$STAGEDIR/$PKGNAME"
namp_dist_tarball "$STAGEDIR" "$PKGNAME" "$PRODUCT/dist"
