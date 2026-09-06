#!/usr/bin/env bash
# Package NAMp Rations for macOS into dist/.
#
# ONE PRODUCT, NOT TWO. The Linux release ships the plug-in AND namp-rations-standalone;
# this one ships the plug-in only. standalone/ is X11, Linux::IRunLoop and JACK
# throughout, and a macOS version of it is a rewrite against NSApplication,
# CoreAudio and CoreMIDI rather than a port. Windows ships the plug-in only for the
# same reason.
#
# TWO WAYS TO RUN IT, because the release is built on two machines that are not
# this one:
#
#   makedist-mac.sh                       build here, package the native slice.
#                                         What someone sitting at a Mac runs.
#   makedist-mac.sh --bundle A --bundle B join two already-built slices with lipo
#                                         and package the result. What CI runs,
#                                         with one bundle from each runner.
#
# WHY THE JOIN IS A SEPARATE STEP AND NOT ONE CMAKE_OSX_ARCHITECTURES="arm64;x86_64"
# BUILD. A universal build produces two slices on one machine, and only one of them
# can be RUN there -- so panelrender, the validator and every other gate would
# silently cover one architecture and assume the other. Each slice is therefore
# built and gated on a host of its own architecture, and lipo only ever joins
# binaries that have already been proved. CMakeLists.txt refuses a multi-arch
# CMAKE_OSX_ARCHITECTURES outright so this cannot be shortcut by accident.
#
# WHAT THIS GATES ON. Everything is measured on the built bundle: what it links,
# what it exports, what is inside it, that both architectures are present, and
# that the signature verifies. The capture-dependent proofs are NOT run here and
# CANNOT be: they need .nam banks, and this plug-in ships none because the
# captures are not ours to distribute. docs/macos-testing.md is what stands in
# for them, and it is a person listening.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${RATIONS_BUILD_DIR:-$REPO/build}"

BUNDLES=()
while [ $# -gt 0 ]; do
  case "$1" in
    --bundle)
      [ $# -ge 2 ] || { echo "--bundle needs a path" >&2; exit 1; }
      BUNDLES+=("$2"); shift 2 ;;
    -h | --help)
      sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)
      echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

for _tool in lipo codesign xattr ditto otool nm strip; do
  command -v "$_tool" >/dev/null 2>&1 || {
    echo "$_tool not found; this script only runs on macOS." >&2; exit 1; }
done

# The project() version, which is the first VERSION line in the top-level lists
# file. Read rather than duplicated, and read the SAME way makedist-linux.sh and
# makedist-windows.sh read it, so the three releases cannot be tagged differently
# from one another.
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]][[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
  "$REPO/CMakeLists.txt" | head -1)"
if [ -z "$VERSION" ]; then
  echo "could not read the project version from CMakeLists.txt" >&2
  exit 1
fi

if [ "${#BUNDLES[@]}" -eq 0 ]; then
  cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -S "$REPO"
  cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"
  BUNDLES=("$BUILD/VST3/Release/NAMp-rations.vst3")
fi

for _b in "${BUNDLES[@]}"; do
  [ -d "$_b" ] || { echo "no bundle at $_b" >&2; exit 1; }
done

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/NAMp-rations-${VERSION}"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

# --- the bundle -------------------------------------------------------------
# The first slice becomes the package; the rest contribute only their binary.
cp -R "${BUNDLES[0]}" "$PKGDIR/NAMp-rations.vst3"
PKGBUNDLE="$PKGDIR/NAMp-rations.vst3"
BIN="$PKGBUNDLE/Contents/MacOS/NAMp-rations"

if [ ! -f "$BIN" ]; then
  echo "no NAMp-rations inside $PKGBUNDLE/Contents/MacOS/" >&2
  echo "The bundle layout is wrong; no host can load this." >&2
  find "$PKGBUNDLE" >&2
  exit 1
fi

# THE OTHER SLICES' RESOURCES MUST MATCH, and this is asserted rather than
# assumed. Only the first bundle's Contents/Resources reaches the user, so a
# difference here would mean the release ships one architecture's art and the
# other architecture's binary. moduleinfo.json is in this comparison too: it
# describes the plug-in's factory and parameters, which are the same source on
# both runners, so a difference in it would say something had genuinely diverged.
# The count guard is not decoration: macOS's own /bin/bash is 3.2, where
# "${A[@]:1}" on a single-element array is an unbound-variable error under set -u.
if [ "${#BUNDLES[@]}" -gt 1 ]; then
  for _b in "${BUNDLES[@]:1}"; do
    if ! diff -r "${BUNDLES[0]}/Contents/Resources" "$_b/Contents/Resources" >/dev/null; then
      echo "the slices' Contents/Resources differ; they are not the same build:" >&2
      diff -r "${BUNDLES[0]}/Contents/Resources" "$_b/Contents/Resources" | head -20 >&2
      exit 1
    fi
  done
fi

# --- lipo -------------------------------------------------------------------
if [ "${#BUNDLES[@]}" -gt 1 ]; then
  SLICE_BINS=()
  for _b in "${BUNDLES[@]}"; do
    _sb="$_b/Contents/MacOS/NAMp-rations"
    [ -f "$_sb" ] || { echo "no binary in $_b" >&2; exit 1; }
    SLICE_BINS+=("$_sb")
  done
  echo "joining ${#SLICE_BINS[@]} slices"
  lipo "${SLICE_BINS[@]}" -create -output "$BIN"
fi

# strip -x, not the GNU --strip-unneeded makedist-linux.sh uses: this is Apple's
# strip, where -x removes local symbols and is what the reference project runs on
# its own VST3 (NeuralAmpModelerPlugin/TemplateProject/scripts/makedist-mac.sh).
# Anything stronger would take the three entry points with it.
strip -x "$BIN"

# --- gates ------------------------------------------------------------------
# ARCHITECTURES. The whole point of the two-runner build, so it is checked rather
# than trusted to have happened.
#
# FLAGGED: `lipo -create -output` is the only lipo spelling verifiable from the
# machine this script was written on (WDL/eel2/regenerate-x64-objects.sh uses it);
# `-archs` is not, and no macOS man pages exist there. If this line is what fails
# on a first run, that is why, and the fix is one flag.
ARCHS="$(lipo -archs "$BIN")"
echo "architectures: $ARCHS"
if [ "${#BUNDLES[@]}" -gt 1 ]; then
  for _want in arm64 x86_64; do
    case " $ARCHS " in
      *" $_want "*) ;;
      *) echo "the joined binary has no $_want slice (got: $ARCHS)" >&2; exit 1 ;;
    esac
  done
fi

# The next two gates read the binary's symbol table and its load commands, and
# both are asked ONE SLICE AT A TIME, on a thin copy cut out with `lipo -thin`.
#
# That is not tidiness. `nm` and `otool` given a universal file emit a section per
# architecture with a header line in front of each, and the parsing below is
# positional -- so on a fat binary the SECOND header is read as though it were a
# symbol or a library. It is what happened: the dependency gate reported the
# plug-in's own path as a non-system library, on a bundle whose two halves had
# each already passed the identical gate as thin slices in CI.
#
# Cutting the slice out first means these gates run on exactly the shape they
# were written for and are proven against, with no claim needed about how either
# tool lays a fat file out -- a claim that cannot be checked from the machine this
# script is written on. It is also the stronger question: a universal binary whose
# x86_64 half links Homebrew and whose arm64 half does not is exactly the failure
# this gate exists to catch, and whole-file output would have let it pass on
# whichever half the tool happened to print first.
THIN="$STAGEDIR/thin"
mkdir -p "$THIN"
for _arch in $ARCHS; do
  _slice="$THIN/$_arch"
  if [ "${#BUNDLES[@]}" -gt 1 ]; then
    lipo -thin "$_arch" "$BIN" -output "$_slice"
  else
    cp "$BIN" "$_slice"          # already thin; lipo -thin refuses a thin input
  fi

  # ENTRY POINTS. Exactly the three a VST3 host calls, and no more.
  #
  # EXACTLY, unlike the Linux script, which can only check for presence: that one
  # is an ELF shared object where weak template instantiations legitimately stay
  # visible. Here -Wl,-exported_symbols_list names an explicit allow-list of three
  # (the SDK's own public.sdk/source/main/macexport.exp), so anything else in the
  # table means that list was not applied. nm -gU is global and defined-here, so
  # undefined imports are not counted; the leading underscore is the Mach-O C
  # prefix. Both sides sorted the same way and under LC_ALL=C, which is not
  # fussiness: a plain `sort` collates by locale, where "_GetPluginFactory" lands
  # AFTER "_bundleEntry" because case is folded, so comparing against a
  # hand-written order fails on a correct bundle. Found by dry-running this script
  # against stub tools, not on a runner.
  EXPORTS="$(nm -gU "$_slice" | awk '{print $3}' | LC_ALL=C sort -u)"
  EXPECTED="$(printf '%s\n' _GetPluginFactory _bundleEntry _bundleExit | LC_ALL=C sort)"
  if [ "$EXPORTS" != "$EXPECTED" ]; then
    echo "unexpected exported symbols in the $_arch slice:" >&2
    echo "$EXPORTS" | sed 's/^/  /' >&2
    echo "Expected exactly _GetPluginFactory, _bundleEntry and _bundleExit." >&2
    exit 1
  fi

  # DEPENDENCIES. Nothing but the system, which is what proves the static
  # dependency sysroot was linked rather than Homebrew's dylibs. A bundle naming
  # /opt/homebrew loads on the build machine and on nobody else's, and fails
  # silently: the host simply reports no such plug-in.
  BADLIBS="$(otool -L "$_slice" | tail -n +2 | awk '{print $1}' \
             | grep -vE '^(/usr/lib/|/System/Library/Frameworks/)' || true)"
  if [ -n "$BADLIBS" ]; then
    echo "the $_arch slice links non-system libraries:" >&2
    echo "$BADLIBS" | sed 's/^/  /' >&2
    exit 1
  fi

  echo "$_arch: three entry points, system libraries only"
done
rm -rf "$THIN"

# RESOURCES. This project embeds NO fallback art: src/gfx/resourcestore.h states
# that its built-in table is always empty here by design. So a bundle that reaches
# a user without Contents/Resources draws flat rectangles and says so only on
# stderr, which nobody reads. Same list as the other two makedist scripts.
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

# --- signing ----------------------------------------------------------------
# AD-HOC, AND IT IS NOT OPTIONAL. An arm64 Mach-O with no signature at all does
# not execute on Apple Silicon -- it is refused by the kernel, not by Gatekeeper --
# so this is a functional requirement rather than a distribution nicety. It also
# has to come LAST: lipo and strip both rewrite the binary, and either would
# invalidate a signature made before them.
#
# The SDK does not do this for us. smtg_target_codesign() is entirely inside an
# if(XCODE) branch (cmake/modules/SMTG_CodeSign.cmake), so under Ninja the only
# thing it signs is moduleinfo.json, from a separate post-build step. The linker's
# own default ad-hoc signature covers the Mach-O; what is missing without this is
# the BUNDLE's _CodeSignature, which is what codesign --verify reads.
#
# xattr -cr first, because extended attributes picked up from a download or an
# archive make codesign fail; the reference project clears them the same way
# before signing its own VST3.
xattr -cr "$PKGBUNDLE"
codesign --force --sign - --verbose "$PKGBUNDLE"
codesign --verify --deep --strict --verbose=2 "$PKGBUNDLE"

# RECORDED, NOT GATED. spctl asks Gatekeeper whether it would let this through,
# and without a Developer ID certificate the answer is no. That is the expected
# result of an ad-hoc signature and is what INSTALL.txt's quarantine step is for.
# It is run so the output is in the log, and so that the day a certificate exists
# this turns from a record into an assertion by deleting `|| true`.
echo "--- spctl (expected to reject an ad-hoc signature) ---"
spctl --assess -vvv "$PKGBUNDLE" 2>&1 || true
echo "-----------------------------------------------------"

# NOTARIZATION, wired and off. It needs a Developer ID and a stored notarytool
# credential profile, neither of which exists yet, so it runs only when one is
# named. Do NOT reach for the reference project's iPlug2/Scripts/notarise.sh: it
# drives `xcrun altool --notarize-app`, which Apple decommissioned in November
# 2023, so it cannot work at all.
#
# FLAGGED: the notarytool invocation below is written from the shape the tool
# replaced, and could not be checked against any documentation on the machine this
# was written on. Verify it against `xcrun notarytool --help` on a real Mac before
# the first real use. `stapler staple` is the one part grounded here, from that
# same (otherwise obsolete) reference script.
if [ -n "${RATIONS_NOTARIZE_PROFILE:-}" ]; then
  echo "notarizing with keychain profile $RATIONS_NOTARIZE_PROFILE"
  _NOTARY_ZIP="$STAGEDIR/notarize.zip"
  ditto -c -k --keepParent "$PKGBUNDLE" "$_NOTARY_ZIP"
  xcrun notarytool submit "$_NOTARY_ZIP" \
        --keychain-profile "$RATIONS_NOTARIZE_PROFILE" --wait
  xcrun stapler staple "$PKGBUNDLE"
fi

# --- licence, attribution, installer ----------------------------------------
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

cat > "$PKGDIR/install.sh" <<'EOF'
#!/usr/bin/env bash
# Install (or remove) NAMp Rations for the current user. Nothing here needs an
# administrator password, and nothing is installed outside your home directory.
#
#   NAMp-rations.vst3 -> ~/Library/Audio/Plug-Ins/VST3
#
# That is the per-user half of the pair every VST3 host scans: the SDK's own
# module loader looks in the user domain first and the system one second
# (public.sdk/source/vst/hosting/module_mac.mm, Module::getModulePaths). The
# system-wide /Library/Audio/Plug-Ins/VST3 is the other half and needs sudo;
# this script deliberately does not touch it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
BUNDLE="$VST3_DIR/NAMp-rations.vst3"

if [ "${1:-}" = "--uninstall" ]; then
  rm -rf "$BUNDLE"
  echo "NAMp Rations removed."
  exit 0
fi

mkdir -p "$VST3_DIR"
rm -rf "$BUNDLE"
cp -R "$HERE/NAMp-rations.vst3" "$VST3_DIR/"

# THE QUARANTINE FLAG, AND WHY THIS IS HERE RATHER THAN IN A README STEP.
#
# macOS marks everything downloaded from the internet with com.apple.quarantine,
# and Gatekeeper refuses to load quarantined code that is not signed with a
# Developer ID and notarized by Apple. This plug-in is signed AD-HOC -- which is
# what makes it run at all on Apple Silicon, but is not a Developer ID -- so
# without this the DAW finds the bundle, fails to load it, and usually says
# nothing useful about why.
#
# -cr clears every extended attribute recursively rather than naming the
# quarantine one, which is how the reference project does it before signing its
# own VST3. It does not disturb the signature: an ad-hoc bundle signature lives
# in Contents/_CodeSignature and inside the Mach-O, not in an attribute.
xattr -cr "$BUNDLE" 2>/dev/null || true

echo "Installed: $BUNDLE"
echo
echo "Rescan plug-ins in your DAW to pick it up."
echo "To remove it again:  ./install.sh --uninstall"
EOF
chmod +x "$PKGDIR/install.sh"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rations ${VERSION} - a four-channel Neural Amp Modeler amp head for macOS

This archive holds one thing:

    NAMp-rations.vst3        the plug-in, for your DAW

There is no standalone application in the macOS release. The Linux release has
one, because it is a JACK client; a macOS version would be a different program
rather than the same one rebuilt.

Install
-------
    ./install.sh

That copies the bundle to ~/Library/Audio/Plug-Ins/VST3 and clears the
quarantine flag. Nothing needs an administrator password. Then rescan plug-ins
in your DAW.

To remove it again: ./install.sh --uninstall

If you would rather copy it by hand
-----------------------------------
Drag NAMp-rations.vst3 into ~/Library/Audio/Plug-Ins/VST3 (or into
/Library/Audio/Plug-Ins/VST3, which is for every user and needs a password),
and then run this once, in Terminal:

    xattr -cr ~/Library/Audio/Plug-Ins/VST3/NAMp-rations.vst3

That last step is not optional, and here is why. macOS marks everything
downloaded from the internet as quarantined, and refuses to load quarantined
code unless it is signed with an Apple Developer ID and notarized by Apple.
This is a free MIT plug-in with no Developer ID, so it is signed "ad-hoc"
instead: enough to run, not enough for Gatekeeper. Without clearing the flag
your DAW finds the plug-in, fails to load it, and often does not say why.

install.sh does this for you. It is written out here only so that nothing about
it is a surprise.

Requirements
------------
macOS 11 (Big Sur) or later, on either Apple Silicon or Intel - the bundle is
universal and carries both. Nothing else: cairo, FreeType, libpng, pixman and
zlib are built into it rather than being things you have to install.

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

The file picker is drawn inside the plug-in rather than being a system dialog,
so it looks like the rest of the panel and cannot clash with whatever toolkit
your DAW is built on.

This is the first macOS release
-------------------------------
It is worth saying plainly. NAMp Rations was written and is tested on Linux, and
the Windows build is checked under Wine on that same machine. There is no Mac
here at all: this build comes off a hosted build runner, where the editor is
rendered and checked page by page and the SDK's validator passes, but where
nothing can listen to it.

What that means in practice is that the DSP has been proved on Linux and Windows
and NOT on macOS - not because it is expected to differ, but because the proofs
that would say so need capture files that are not ours to distribute, so they
cannot be run on a build runner. If something sounds wrong to you, that is worth
reporting rather than assuming it is your rig.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution.
EOF

# --- archive ----------------------------------------------------------------
# ditto, NOT tar or zip. It is Apple's own archiver and is what the reference
# project uses to package signed code before submitting it for notarization
# (iPlug2/Scripts/notarise.sh). It preserves the resource forks and extended
# attributes that an ordinary zip drops and that bsdtar turns into stray ._
# files, which is what keeps the code signature intact through the download.
mkdir -p "$REPO/dist"
if [ "${#BUNDLES[@]}" -gt 1 ]; then
  SUFFIX="universal"
else
  SUFFIX="$(uname -m)"
fi
ARCHIVE="$REPO/dist/NAMp-rations-${VERSION}-macos-${SUFFIX}.zip"
rm -f "$ARCHIVE"
ditto -c -k --keepParent "$PKGDIR" "$ARCHIVE"

# THE SIGNATURE MUST SURVIVE THE ARCHIVE, which is the whole reason ditto was
# chosen over tar or zip, so it is proved rather than asserted: extract the
# archive again and re-verify the copy that comes out. A plain zip drops the
# metadata and this check is what would catch it.
ditto -x -k "$ARCHIVE" "$STAGEDIR/verify"
VERIFYBUNDLE="$STAGEDIR/verify/NAMp-rations-${VERSION}/NAMp-rations.vst3"
if [ ! -d "$VERIFYBUNDLE" ]; then
  echo "the archive did not round-trip; no bundle at $VERIFYBUNDLE" >&2
  find "$STAGEDIR/verify" -maxdepth 3 >&2
  exit 1
fi
codesign --verify --deep --strict --verbose=2 "$VERIFYBUNDLE"

echo ""
echo "Packaged: $ARCHIVE"
echo "  $(du -h "$ARCHIVE" | cut -f1), $ARCHS, signature verified after extraction"
echo ""
echo "Contents:"
find "$STAGEDIR/verify" -mindepth 1 -maxdepth 3 | sed "s|$STAGEDIR/verify/||"
