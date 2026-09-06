#!/usr/bin/env bash
# Build NAMp Rations for 64-bit Linux and package the release tarball into dist/.
#
# TWO PRODUCTS, ONE TARBALL, AND NEITHER NEEDS THE OTHER TO BE INSTALLED.
#
#   NAMp-rations.vst3        the plug-in, for a DAW. It links cairo, FreeType, fontconfig and libX11
#                       and NOTHING else - in particular it does not link JACK, which is checked
#                       below rather than assumed.
#   namp-rations-standalone  the amp without a DAW: the same bundle, hosted on JACK in a window of its
#                       own, with a MIDI port for the footswitch. It is a HOST - it loads
#                       NAMp-rations.vst3 rather than containing a second copy of it - so the two files
#                       in this archive are not independent of each other the way the parent
#                       project's are. That is deliberate: the editor finds its art through
#                       dladdr() relative to the loaded module, so a standalone that linked the
#                       plug-in in would be resolving resources by a different route from the one
#                       every user's DAW uses.
#
# WINDOWS IS A SEPARATE RELEASE and ships the plug-in only; see scripts/makedist-windows.sh.
#
# WHAT THIS GATES ON, AND WHAT IT DELIBERATELY DOES NOT. Everything here is measured on the built
# binaries - what they link, what they export, what is inside the bundle, and that the standalone
# runs far enough to print its usage. All of it works on any machine: no captures, no JACK server,
# no X display. The proofs that DO need those (rations_offline, scripts/ir-gate.sh,
# scripts/switch-gate.sh) are run by hand against a rig that has them, and switch-gate.sh in
# particular restarts jackd, which is not a thing a packaging script may do to someone's session.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${RATIONS_BUILD_DIR:-$REPO/build}"
ARCH="$(uname -m)"

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

# The project() version, which is the first VERSION line in the top-level lists file. Read rather
# than duplicated, and read the SAME way makedist-windows.sh reads it, so the two releases cannot
# be tagged differently from one another.
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]][[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
  "$REPO/CMakeLists.txt" | head -1)"
if [ -z "$VERSION" ]; then
  echo "could not read the project version from CMakeLists.txt" >&2
  exit 1
fi

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/NAMp-rations-${VERSION}"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

# --- the plug-in ------------------------------------------------------------
BUNDLE="$BUILD/VST3/Release/NAMp-rations.vst3"
if [ ! -d "$BUNDLE" ]; then
  echo "VST3 bundle not found at $BUNDLE" >&2
  exit 1
fi
cp -r "$BUNDLE" "$PKGDIR/"
PKGBUNDLE="$PKGDIR/NAMp-rations.vst3"

# ONE ARCHITECTURE FOLDER, AND IT IS THE LINUX ONE.
#
# A VST3 bundle holds Contents/<arch>/ per platform, so a build tree that was once configured with
# the MinGW toolchain and later re-configured natively keeps its Contents/x86_64-win directory:
# CMake writes the new binary beside the old one instead of replacing it, and `cp -r` then carries
# a Windows DLL into the Linux tarball. It loads nowhere and is pure weight. This is the mirror of
# the prune in makedist-windows.sh, for the mirror-image mistake.
for _arch in "$PKGBUNDLE/Contents"/*/; do
  _arch="${_arch%/}"
  _name="$(basename "$_arch")"
  case "$_name" in
    Resources | "${ARCH}-linux") ;;
    *)
      echo "warning: removing $_name/ from the packaged bundle - it is not a Linux" >&2
      echo "  architecture folder and belongs to no Linux release. '$BUILD' was" >&2
      echo "  configured for another platform at some point; run 'rm -rf $BUILD'" >&2
      echo "  and re-run this script to stop seeing this." >&2
      rm -rf "$_arch"
      ;;
  esac
done

PLUGIN_SO="$PKGBUNDLE/Contents/${ARCH}-linux/NAMp-rations.so"
if [ ! -f "$PLUGIN_SO" ]; then
  echo "no NAMp-rations.so inside $PKGBUNDLE/Contents/${ARCH}-linux/" >&2
  echo "The bundle layout is wrong; no host can load this." >&2
  find "$PKGBUNDLE" -type f >&2
  exit 1
fi

# RESOURCES. This project embeds NO fallback art: src/gfx/resourcestore.h states that its built-in
# table is always empty here by design. So a bundle that reaches a user without Contents/Resources
# draws flat rectangles and says so only on stderr, which nobody reads. This assertion is the
# entire safety net, and it is the same list makedist-windows.sh checks. The SVG in the list is
# File.svg, which the IR and capture loader rows draw; it was Gear.svg until the settings control
# became a labelled button and stopped drawing the gear at all.
for _res in Contents/Resources/img/head.png \
            Contents/Resources/img/pedal-boost.png \
            Contents/Resources/img/File.svg \
            Contents/Resources/fonts/Michroma-Regular.ttf \
            Contents/Resources/fonts/Roboto-Regular.ttf; do
  if [ ! -f "$PKGBUNDLE/$_res" ]; then
    echo "the bundle is missing $_res; the editor would draw flat rectangles." >&2
    exit 1
  fi
done

strip --strip-unneeded "$PLUGIN_SO"

# --- the standalone ---------------------------------------------------------
# A shipped component, so a build that skipped it (JACK development files absent) must not quietly
# produce a half release - the tarball would then be the Windows release with a Linux binary in it.
STANDALONE="$BUILD/namp-rations-standalone"
if [ ! -f "$STANDALONE" ]; then
  echo "namp-rations-standalone was not built - install the JACK development files" >&2
  echo "(libjack-jackd2-dev, or libjack-dev) and re-run, or the release would" >&2
  echo "ship the plug-in only." >&2
  exit 1
fi
cp "$STANDALONE" "$PKGDIR/"
strip --strip-unneeded "$PKGDIR/namp-rations-standalone"

# --- gates ------------------------------------------------------------------
# THE PLUG-IN MUST NOT LINK THE AUDIO BACKEND. Only the standalone hosts JACK; a bundle that
# linked it would refuse to load on every machine without libjack installed, which is most of
# them, and would do it silently - the host simply reports no such plug-in.
if ldd "$PLUGIN_SO" | grep -qi 'libjack'; then
  echo "the VST3 bundle links libjack. Only namp-rations-standalone may." >&2
  ldd "$PLUGIN_SO" | grep -i jack >&2
  exit 1
fi

# ...AND THE STANDALONE MUST. The other direction is worth checking too: a standalone that somehow
# came out without it is one that cannot make a sound, and it would not say so until it ran.
if ! ldd "$PKGDIR/namp-rations-standalone" | grep -qi 'libjack'; then
  echo "namp-rations-standalone does not link libjack; it could not open an audio device." >&2
  exit 1
fi

# ENTRY POINTS. The three a VST3 host calls on Linux.
#
# Presence, not an exact set: unlike the Windows DLL this is an ELF shared object, where a handful
# of weak C++ template instantiations legitimately remain visible whatever -fvisibility=hidden and
# --exclude-libs,ALL do, so an "exactly three" rule here would fail on an innocent template.
for _entry in GetPluginFactory ModuleEntry ModuleExit; do
  if ! nm -D --defined-only "$PLUGIN_SO" | awk '$2 == "T" { print $3 }' | grep -qx "$_entry"; then
    echo "the VST3 bundle does not export $_entry; no host can load it." >&2
    exit 1
  fi
done

# NO STB_GNU_UNIQUE SYMBOLS. A unique symbol makes glibc's loader refuse to unload the library and
# binds it process-wide, so two NAM-derived plug-ins in one host collide - and the failure is an
# abort inside the host, not an error we could report. They appear when a static-local in an
# inline function or template escapes the visibility settings.
UNIQUE_SYMS="$(nm -D --defined-only "$PLUGIN_SO" | awk '$2 == "u" { print $3 }')"
if [ -n "$UNIQUE_SYMS" ]; then
  echo "the VST3 bundle exports STB_GNU_UNIQUE symbols:" >&2
  echo "$UNIQUE_SYMS" | head -10 | sed 's/^/  /' >&2
  echo "A host loading a second NAM plug-in would abort. Check -fvisibility=hidden" >&2
  echo "and -Wl,--exclude-libs,ALL in CMakeLists.txt." >&2
  exit 1
fi

# THE STANDALONE RUNS. --help touches the argument parser and the settings-path logic and returns
# 0, which is as far as anything can be driven without a display and a JACK server.
STANDALONE_HELP="$("$PKGDIR/namp-rations-standalone" --help 2>&1 || true)"
if ! printf '%s' "$STANDALONE_HELP" | grep -q "usage: namp-rations-standalone"; then
  echo "namp-rations-standalone --help did not print its usage:" >&2
  printf '%s\n' "$STANDALONE_HELP" | head -10 >&2
  exit 1
fi

# --- licence, attribution, launcher -----------------------------------------
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

if [ ! -f "$REPO/packaging/icons/namp-rations-256.png" ]; then
  echo "the application icons are missing - run gui/make_icon.sh" >&2
  exit 1
fi
mkdir -p "$PKGDIR/desktop"
cp "$REPO/packaging/namp-rations.desktop" "$PKGDIR/desktop/"
cp "$REPO"/packaging/icons/namp-rations-*.png "$PKGDIR/desktop/"

cat > "$PKGDIR/install.sh" <<'EOF'
#!/usr/bin/env bash
# Install (or remove) NAMp Rations for the current user. Nothing here needs root, and nothing is
# installed outside your home directory.
#
#   NAMp-rations.vst3         -> ~/.vst3                                   (the plug-in, for a DAW)
#   namp-rations-standalone   -> ~/.local/bin                              (the amp, on JACK)
#   namp-rations.desktop      -> ~/.local/share/applications               (the launcher entry)
#   namp-rations-<size>.png   -> ~/.local/share/icons/hicolor/<size>/apps  (its icon)
#
# The standalone LOADS the plug-in rather than containing it, and ~/.vst3 is one of the places it
# looks, so installing the two together is what makes the menu entry work from anywhere.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VST3_DIR="$HOME/.vst3"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_ROOT="$HOME/.local/share/icons/hicolor"

refresh() {
  command -v update-desktop-database >/dev/null && update-desktop-database "$APP_DIR" 2>/dev/null || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -f -t "$ICON_ROOT" 2>/dev/null || true
}

if [ "${1:-}" = "--uninstall" ]; then
  rm -rf "$VST3_DIR/NAMp-rations.vst3"
  rm -f "$BIN_DIR/namp-rations-standalone"
  rm -f "$APP_DIR/namp-rations.desktop"
  for SIZE in 256 128 64 48; do
    rm -f "$ICON_ROOT/${SIZE}x${SIZE}/apps/namp-rations.png"
  done
  refresh
  echo "NAMp Rations removed."
  echo "Your settings are NOT removed: ~/.config/NAMp-rations/standalone.state holds which"
  echo "captures the standalone was playing. Delete it by hand if you want it gone."
  exit 0
fi

mkdir -p "$VST3_DIR" "$BIN_DIR" "$APP_DIR"
rm -rf "$VST3_DIR/NAMp-rations.vst3"
cp -r "$HERE/NAMp-rations.vst3" "$VST3_DIR/"
install -m 755 "$HERE/namp-rations-standalone" "$BIN_DIR/namp-rations-standalone"
install -m 644 "$HERE/desktop/namp-rations.desktop" "$APP_DIR/namp-rations.desktop"
for SIZE in 256 128 64 48; do
  mkdir -p "$ICON_ROOT/${SIZE}x${SIZE}/apps"
  install -m 644 "$HERE/desktop/namp-rations-${SIZE}.png" "$ICON_ROOT/${SIZE}x${SIZE}/apps/namp-rations.png"
done
refresh

echo "Installed:"
echo "  plug-in     $VST3_DIR/NAMp-rations.vst3"
echo "  standalone  $BIN_DIR/namp-rations-standalone"
echo "  launcher    $APP_DIR/namp-rations.desktop"
echo
echo "Rescan plug-ins in your DAW to pick up the VST3."
case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) echo "Note: $BIN_DIR is not on your PATH, so 'namp-rations-standalone' will not be"
     echo "found by name from a shell. The menu entry works either way." ;;
esac
echo
echo "To remove it again:  ./install.sh --uninstall"
EOF
chmod +x "$PKGDIR/install.sh"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rations ${VERSION} - a four-channel Neural Amp Modeler amp head for Linux

This archive holds two things, and they are the same amp twice:

    NAMp-rations.vst3        the plug-in, for your DAW
    namp-rations-standalone  the amp on its own, on JACK, with its own window

The standalone is a HOST rather than a second copy: it loads
NAMp-rations.vst3. Keep the two together, or install both with the script
below.

Install
-------
    ./install.sh

Everything goes under your home directory and nothing needs root:

    ~/.vst3/NAMp-rations.vst3                   the plug-in
    ~/.local/bin/namp-rations-standalone        the standalone
    ~/.local/share/applications/           a menu entry, with an icon

Then rescan plug-ins in your DAW. To remove it all again: ./install.sh --uninstall

You can also just run it where you extracted it, with no installation at all:

    ./namp-rations-standalone

and copy NAMp-rations.vst3 into ~/.vst3 by hand if you only want the plug-in.
The standalone looks for the bundle in \$RATIONS_VST3, then beside itself, then
~/.vst3, then /usr/local/lib/vst3 and /usr/lib/vst3, and you can also pass the
path to it:

    ./namp-rations-standalone /path/to/NAMp-rations.vst3

The standalone
--------------
It is a JACK application. Start a JACK server first (qjackctl, or e.g.
"jackd -R -d alsa -r 48000 -p 256"), or run a PipeWire desktop, which provides
one. With no server it still opens, so you can set your captures up, but it
makes no sound and says so.

It registers these ports:

    NAMp-rations:in       your guitar
    NAMp-rations:out_l    \\ the amp, in stereo from the pedalboard's
    NAMp-rations:out_r    / flanger onwards
    NAMp-rations:midi_in  a MIDI footswitch

The audio ports are connected to the first physical capture and playback ports
it finds. The MIDI port is left UNCONNECTED on purpose: which of your MIDI
devices is the footswitch is not something to guess at, and the wrong guess has
a keyboard changing amp channels. Connect it in your patchbay.

What it was playing - the four capture banks, the impulse responses, the
pedalboard, the MIDI bindings, every knob - is written to

    ~/.config/NAMp-rations/standalone.state

when you close it, and read back when you start it. Run it with --no-state to
skip both ends and come up empty.

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
stack and the noise gate. The icon left of the settings button is Slim, which trades model
size for CPU - it appears only when your captures can actually use it.

Two more pages, reached by the buttons at the bottom: a cabinet page that loads
one or two impulse responses with a blend between them, and a pedalboard of
five pedals - Boost and Chorus before the amp, Flanger, Delay and Reverb after
it.

The file picker is drawn inside the plug-in rather than being a GTK or Qt
dialog, so it looks like the rest of the panel and cannot clash with whatever
toolkit your DAW is built on.

Requirements
------------
The plug-in needs cairo, freetype2, fontconfig and libX11, which a desktop
Linux install already has. It does NOT link JACK.

The standalone additionally needs the JACK client library (libjack.so.0) and a
running JACK server. On Debian/Devuan/Ubuntu:

    sudo apt install jackd2          # pulls in libjack-jackd2-0

On a PipeWire desktop, "pipewire-jack" provides the same library and server.
Either way you almost certainly have this already if you run JACK at all --
libjack.so.0 ships with the server, not separately.

Nothing here needs a -dev package; those are only for building from source.

There is no 32-bit build. The Windows release is a separate download and ships
the plug-in only.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution.
EOF

mkdir -p "$REPO/dist"
TARBALL="$REPO/dist/NAMp-rations-${VERSION}-linux-${ARCH}.tar.gz"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$STAGEDIR" "NAMp-rations-${VERSION}"

echo ""
echo "Packaged: $TARBALL"
echo ""
echo "Contents:"
tar -tzf "$TARBALL"
