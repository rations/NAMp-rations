#!/usr/bin/env bash
# Cross-build NAMp Rations for 64-bit Windows and package the release ZIP into dist/.
#
# ONE PRODUCT ON WINDOWS. This archive is the VST3 bundle and nothing else. The
# standalone is a JACK application and therefore a Linux product - it ships with
# that release (scripts/makedist-linux.sh) - and there is no rack and no plug-in
# host on either platform, so there is no second binary to package here.
#
# TWO WAYS TO INSTALL IT, both in the ZIP. NAMp-rations-install.exe puts the bundle
# where hosts look and registers an uninstall entry; the NAMp-rations.vst3 folder
# beside it is the same bundle for anyone who would rather copy it themselves —
# which is not a stylistic preference, it is the fallback for a machine whose
# SmartScreen or antivirus refuses an unsigned installer. See
# installer/namp-rations.nsi.
#
# NO WINDOWS MACHINE IS INVOLVED. The compiler is MinGW-w64 running here; the
# verification runs the cross-built binaries under Wine, which is enough to
# prove the bundle loads, passes the SDK validator and draws its editor pages
# the same way the Linux build does. It is NOT a substitute for a test on real
# Windows, which is the actual gate before a release goes out.
#
# Environment:
#   WINEPREFIX          defaults to ~/.wine-rations — a prefix of its own,
#                       because a desktop's ~/.wine is usually managed by
#                       something else (here, by vstbridge).
#   RATIONS_SKIP_WINE   set to 1 to package without the Wine verification. The
#                       ZIP is then unverified AND has no moduleinfo.json; the
#                       script says so loudly rather than quietly producing a
#                       lesser build.
#   RATIONS_NSIS_DIR    the NSIS prefix, defaulting to ~/third_party/nsis (bin/
#                       and share/nsis/). A makensis on PATH is used if absent.
#   RATIONS_SKIP_INSTALLER
#                       set to 1 to package the bundle without
#                       NAMp-rations-install.exe.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build-win"
TRIPLE="${RATIONS_WIN_TRIPLE:-x86_64-w64-mingw32}"
OBJDUMP="$TRIPLE-objdump"
STRIP="$TRIPLE-strip"

command -v "$TRIPLE-g++" >/dev/null || {
  echo "error: $TRIPLE-g++ not found. Install the MinGW-w64 cross toolchain:" >&2
  echo "  sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64" >&2
  exit 1
}
if [ ! -d "${RATIONS_WIN_SYSROOT:-$HOME/third_party/win-deps/sysroot}/lib/pkgconfig" ]; then
  echo "error: the Windows dependency sysroot is missing. Build it first:" >&2
  echo "  scripts/build-win-deps.sh" >&2
  exit 1
fi

# makensis is a NATIVE Linux binary: it links one of NSIS's prebuilt PE stubs
# and appends the compressed payload, so NAMp-rations-install.exe is produced without
# Wine and without the cross compiler. Prefer an unpacked prefix (bin/ +
# share/nsis/, which is what `apt-get download nsis nsis-common` + `dpkg-deb -x`
# gives without root); fall back to a system install.
MAKENSIS=""
NSIS_PREFIX="${RATIONS_NSIS_DIR:-$HOME/third_party/nsis}"
if [ -x "$NSIS_PREFIX/bin/makensis" ] && [ -d "$NSIS_PREFIX/share/nsis" ]; then
  MAKENSIS="$NSIS_PREFIX/bin/makensis"
  export NSISDIR="$NSIS_PREFIX/share/nsis"
elif command -v makensis >/dev/null; then
  MAKENSIS="makensis"
fi
if [ "${RATIONS_SKIP_INSTALLER:-0}" != "1" ] && [ -z "$MAKENSIS" ]; then
  echo "error: makensis not found, so NAMp-rations-install.exe cannot be built." >&2
  echo "  sudo apt install nsis" >&2
  echo "or unpack it without root into \$RATIONS_NSIS_DIR (default" >&2
  echo "$NSIS_PREFIX) as bin/makensis and share/nsis/:" >&2
  echo "  apt-get download nsis nsis-common && dpkg-deb -x <each>.deb root/" >&2
  echo "Set RATIONS_SKIP_INSTALLER=1 to package the bundle without an installer." >&2
  exit 1
fi

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-mingw-w64.cmake" -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

# The project() version, which is the first VERSION line in the top-level lists
# file. Read rather than duplicated, so a release cannot be tagged one thing and
# packaged as another.
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

# ONE ARCHITECTURE FOLDER, AND IT IS THE WINDOWS ONE.
#
# A VST3 bundle holds Contents/<arch>/ per platform, so a build tree that was
# once configured natively and later re-configured with the MinGW toolchain
# keeps its Contents/x86_64-linux directory: CMake writes the new binary beside
# the old one instead of replacing it, and `cp -r` then carries a Linux .so into
# the Windows ZIP. It loads nowhere and is pure weight. Prune anything that is
# not x86_64-win from the staged copy and say why, loudly enough that a stale
# build-win gets cleaned rather than tolerated.
for _arch in "$PKGBUNDLE/Contents"/*/; do
  _arch="${_arch%/}"
  _name="$(basename "$_arch")"
  case "$_name" in
    Resources | x86_64-win) ;;
    *)
      echo "warning: removing $_name/ from the packaged bundle - it is not a" >&2
      echo "  Windows architecture folder and belongs to no Windows release." >&2
      echo "  '$BUILD' was configured for another platform at some point; run" >&2
      echo "  'rm -rf $BUILD' and re-run this script to stop seeing this." >&2
      rm -rf "$_arch"
      ;;
  esac
done

# The art and fonts in Contents/Resources are copied as they are: they are
# looked up at run time, not embedded, which is what lets a user replace them
# without a rebuild.
DLL="$PKGBUNDLE/Contents/x86_64-win/NAMp-rations.vst3"

# SHAPE. Module::validateBundleStructure in the SDK's module_win32.cpp requires
# the inner DLL to be named exactly like the bundle folder — NAMp-rations.vst3 inside
# NAMp-rations.vst3/Contents/<arch>/. A DLL called NAMp-rations.dll in the right folder
# does not load, and nothing before this point would have said so: the CMake
# bundle machinery places the binary with a foreach over
# CMAKE_CONFIGURATION_TYPES, which is EMPTY under every single-config generator,
# so getting this wrong produces a complete-looking bundle with the binary
# somewhere else entirely.
if [ ! -f "$DLL" ]; then
  echo "no NAMp-rations.vst3 binary inside $PKGBUNDLE/Contents/x86_64-win/" >&2
  echo "The bundle layout is wrong; see the LIBRARY_OUTPUT_DIRECTORY block in" >&2
  echo "CMakeLists.txt. No host can load this." >&2
  find "$PKGBUNDLE" -type f >&2
  exit 1
fi

# RESOURCES. This project embeds NO fallback art: src/gfx/resourcestore.h states
# that its built-in table is always empty here by design, and NAMp's
# installBuiltinResources() is deliberately absent from the header. So a bundle
# that reaches a user without Contents/Resources draws flat rectangles and says
# so only on stderr, which nobody reads. This assertion is the entire safety net.
# The SVG in the list is File.svg, which the IR and capture loader rows draw; it
# was Gear.svg until the settings control became a labelled button and stopped
# drawing the gear at all.
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

"$STRIP" --strip-unneeded "$DLL"

# IMPORTS. Nothing but Windows' own DLLs may appear here.
#
# The MinGW toolchain's default is to link libgcc_s_seh-1.dll, libstdc++-6.dll
# and libwinpthread-1.dll, and a VST3 bundle CANNOT ship those beside the
# binary: the SDK loads a plug-in with a plain LoadLibraryW of the full path
# (module_win32.cpp, loadAsPackage) and the default DLL search order does not
# include the loaded module's own directory. The -static in
# cmake/toolchain-mingw-w64.cmake is what prevents it, it is one edit away from
# being lost, and losing it fails on the user's machine — as a plug-in that
# simply never appears — rather than here.
ALLOWED_DLLS='^(KERNEL32|USER32|GDI32|MSIMG32|SHELL32|ADVAPI32|ole32|OLEAUT32|COMDLG32|WINSPOOL|SHLWAPI|msvcrt|api-ms-win-)'
BAD_IMPORTS="$("$OBJDUMP" -p "$DLL" | sed -n 's/^\tDLL Name: //p' \
  | sed 's/\.dll$//I' | grep -Ev "$ALLOWED_DLLS" || true)"
if [ -n "$BAD_IMPORTS" ]; then
  echo "the VST3 bundle imports non-system DLLs:" >&2
  echo "$BAD_IMPORTS" >&2
  echo "A VST3 bundle cannot ship sibling DLLs - the host LoadLibraryW's the" >&2
  echo "plug-in by full path and its own directory is not searched. Check that" >&2
  echo "-static survived in cmake/toolchain-mingw-w64.cmake." >&2
  exit 1
fi

# EXPORTS. Exactly three, no more.
#
# The three are what a VST3 host calls. "No more" is the part worth checking:
# a static archive built with __declspec(dllexport) still in effect carries
# .drectve export directives that the linker obeys, so linking one silently
# re-exports somebody else's entire API from our plug-in. FreeType's meson build
# does exactly that on Windows — it defines DLL_EXPORT without testing
# default_library — and the parent project's bundle shipped 151 FreeType symbols
# until scripts/build-win-deps.sh started removing it.
# -Wl,--exclude-all-symbols does NOT catch this, because a .drectve export is
# explicit rather than automatic.
EXPORTS="$("$OBJDUMP" -p "$DLL" \
  | sed -n '/\[Ordinal\/Name Pointer\] Table/,$p' \
  | sed -n 's/^\t\[ *[0-9]* *\] +base\[ *[0-9]* *\] *[0-9a-f]* \(.*\)$/\1/p' \
  | sort)"
EXPECTED="$(printf 'ExitDll\nGetPluginFactory\nInitDll\n')"
if [ "$EXPORTS" != "$EXPECTED" ]; then
  echo "the VST3 bundle does not export exactly the three entry points." >&2
  echo "expected:" >&2; echo "$EXPECTED" | sed 's/^/  /' >&2
  echo "found ($(echo "$EXPORTS" | grep -c .)):" >&2
  echo "$EXPORTS" | head -20 | sed 's/^/  /' >&2
  [ "$(echo "$EXPORTS" | grep -c .)" -gt 20 ] && echo "  ..." >&2
  exit 1
fi

# --- verification under Wine ------------------------------------------------
if [ "${RATIONS_SKIP_WINE:-0}" = "1" ]; then
  echo
  echo "WARNING: RATIONS_SKIP_WINE=1 - the SDK validator was NOT run against" >&2
  echo "this bundle, the editor pages were NOT compared against the Linux" >&2
  echo "render, and no moduleinfo.json was generated. Do not release this." >&2
  echo
else
  command -v wine >/dev/null || {
    echo "error: wine not found. Install it, or set RATIONS_SKIP_WINE=1 to" >&2
    echo "package an unverified build." >&2
    exit 1
  }
  export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-rations}"
  export WINEDEBUG="${WINEDEBUG:--all}"
  if [ ! -d "$WINEPREFIX" ]; then
    echo "creating a Wine prefix at $WINEPREFIX"
    wineboot --init >/dev/null 2>&1 || true
  fi

  WIN_BUNDLE="$(winepath -w "$PKGBUNDLE")"

  # moduleinfo.json is OPTIONAL — Module::getModuleInfoPath returns an empty
  # optional when it is absent and the validator does not require it — but it is
  # one Wine call, so generate it. It has to happen here rather than as a build
  # step because CMake would run the freshly cross-built moduleinfotool.exe
  # natively on the build host, where it cannot execute.
  echo "generating moduleinfo.json"
  wine "$BUILD/bin/moduleinfotool.exe" -create -version "$VERSION" \
       -path "$WIN_BUNDLE" \
       -output "$(winepath -w "$PKGBUNDLE/Contents/Resources/moduleinfo.json")" 2>/dev/null
  if [ ! -s "$PKGBUNDLE/Contents/Resources/moduleinfo.json" ]; then
    echo "moduleinfotool produced no moduleinfo.json" >&2
    exit 1
  fi

  echo "running the SDK validator"
  VALIDATOR_OUT="$(wine "$BUILD/bin/validator.exe" "$WIN_BUNDLE" 2>&1 || true)"
  if ! printf '%s' "$VALIDATOR_OUT" | grep -qE '^Result: [0-9]+ tests passed, 0 tests failed'; then
    echo "the SDK validator did not pass against the packaged bundle:" >&2
    printf '%s\n' "$VALIDATOR_OUT" | tail -40 >&2
    exit 1
  fi
  printf '%s\n' "$VALIDATOR_OUT" | grep -E '^Result:'

  #-------------------------------------------------------------------------
  # THE PANEL DIFF: does the Windows editor DRAW what the Linux one draws?
  #
  # This is the only automated check of the Windows editor's appearance, and it
  # is why scripts/build-win-deps.sh pins its five dependencies to this
  # machine's system versions. It renders all four pages with both panelrender
  # binaries — the same sources, the same resources/ tree, the same scale — and
  # compares them pixel for pixel. It therefore exercises cairo, FreeType
  # rasterisation, PNG decode, NanoSVG and the whole of src/gfx in the cross
  # build, plus panelrender's own art, text-clearance and hit-target audits.
  #
  # THE THRESHOLDS ARE MEASURED, NOT CHOSEN. As first taken, at cairo 1.18.4 /
  # freetype 2.13.3 / pixman 0.44.0 / libpng 1.6.48 / zlib 1.3.1:
  #
  #     head         32 of  456,599 px    worst channel delta 1/255
  #     cabinet       0 of  294,400 px    byte-identical
  #     pedalboard   32 of  450,822 px    worst channel delta 1/255
  #     settings      0 of  746,240 px    byte-identical
  #     TOTAL        64 of 1,948,061 px
  #
  # Every differing pixel is a single quantisation step, and the two pages that
  # differ at all are the two that draw rotated dials — which is where the
  # trigonometry, and so the float rounding, lives. The caps below sit well
  # above that so an ordinary art edit does not trip them, while a genuine
  # divergence stays impossible to miss: a different FreeType or cairo moves
  # glyph rasterisation by thousands of pixels at full contrast, three orders of
  # magnitude away.
  #
  # MAX_DELTA IS THE SHARPER OF THE TWO GATES and the one to trust. A rounding
  # difference is 1/255 by definition; a real rasterisation change puts down ink
  # where there was none, which is a delta of tens or hundreds whatever the
  # pixel count says.
  #-------------------------------------------------------------------------
  PANEL_MAX_PIXELS=256     # per page
  PANEL_MAX_DELTA=1        # per channel, any page

  # This used to be skipped with a warning when ImageMagick was absent, which is
  # a gate that can quietly not run. panel-diff.sh decodes the PNGs with the
  # Python standard library now, so there is nothing left to be missing and
  # nothing left to skip.
  echo "comparing the editor pages against the Linux render"
  PANELS="$STAGEDIR/panels"
  mkdir -p "$PANELS"

  # The Linux reference has to come from a Linux build of the same tree.
  LINUX_PANELRENDER="${RATIONS_BUILD_DIR:-$REPO/build}/panelrender"
  if [ ! -x "$LINUX_PANELRENDER" ]; then
    echo "no Linux panelrender at $LINUX_PANELRENDER - build the native tree first:" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    echo "or set RATIONS_BUILD_DIR to a native build directory." >&2
    exit 1
  fi

  "$LINUX_PANELRENDER" "$PANELS/lin" "$REPO/resources" 1.0 >/dev/null
  # The resource directory is passed explicitly rather than left to
  # respath.cpp's module-relative fallback, which resolves to nothing for a
  # bare .exe sitting outside a bundle.
  wine "$BUILD/panelrender.exe" "$(winepath -w "$PANELS")\\win" \
       "$(winepath -w "$REPO/resources")" 1.0 >/dev/null

  # The comparison itself is scripts/panel-diff.sh, which the macOS workflow
  # calls too -- one implementation, so the two platforms' figures are
  # produced the same way and stay comparable. The thresholds stay HERE
  # because they are this pair's measurement, not that script's.
  PANEL_MAX_PIXELS="$PANEL_MAX_PIXELS" PANEL_MAX_DELTA="$PANEL_MAX_DELTA" \
    "$REPO/scripts/panel-diff.sh" "$PANELS" lin "$PANELS" win Linux Windows
fi

# --- licence, attribution and instructions ----------------------------------
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rations ${VERSION} - a four-channel Neural Amp Modeler amp head,
VST3 for Windows

This archive holds the plug-in, twice over - an installer, and the same bundle
loose so you can install it by hand instead:

    NAMp-rations-install.exe  the installer
    NAMp-rations.vst3         a folder, not a file. Copy the WHOLE folder.

Install, the easy way
---------------------
Run NAMp-rations-install.exe.

It is not code-signed, so Windows SmartScreen will show a blue "Windows
protected your PC" box. Click "More info", then "Run anyway" - or install by
hand instead, below; the two put exactly the same folder in exactly the same
place.

Run it as an administrator and it installs for everyone, in
C:\\Program Files\\Common Files\\VST3. Run it normally and it installs just for
you, in %LOCALAPPDATA%\\Programs\\Common\\VST3. Either way it tells you which,
and you can change the folder. To remove it later, use "Apps & features" in
Windows Settings, or the uninstaller it leaves in the folder it names at the
end.

Install, by hand
----------------
A VST3 plug-in is installed by copying its folder to where hosts look. There
are two such places, and hosts search them in this order:

  1. Just for you - no administrator rights needed:

         %LOCALAPPDATA%\\Programs\\Common\\VST3\\

     Paste that into the Explorer address bar. If the VST3 folder is not there,
     create it.

  2. For every user on the machine - needs administrator rights:

         C:\\Program Files\\Common Files\\VST3\\

Copy NAMp-rations.vst3 into one of them, then rescan plug-ins in your DAW.

Do not rename anything inside the folder. The bundle carries its own art and
fonts in NAMp-rations.vst3\\Contents\\Resources, and the binary in
NAMp-rations.vst3\\Contents\\x86_64-win must keep the name
NAMp-rations.vst3 or no host will load it.

To uninstall a hand-installed copy, delete the NAMp-rations.vst3 folder.

Whichever way you install, make sure you only have ONE copy. Hosts scan both
folders, so a copy left in each shows up as two NAMp Rations entries in the
plug-in list. The installer offers to remove the other one for you.

Requirements
------------
64-bit Windows and a VST3 host. Nothing else: cairo, FreeType, libpng, zlib and
the GCC runtime are all linked into the plug-in, so there is no redistributable
to install and nothing to put beside the binary.

There is no 32-bit build, and no standalone in this archive: the standalone is
a JACK application, so it ships with the Linux release - which is a separate
download, and the only place anything here is built for Linux.

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
rows, and the output section - Raw / Normalized / Calibrated, plus input
calibration. Normalized is the default.

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

The file picker is drawn inside the plug-in rather than being a Windows dialog,
so it looks like the rest of the panel. ".." goes up; above a drive root it
lists the drives, so captures on D: are reachable.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution - which matters more
for this build than for the Linux one, because the Windows plug-in statically
links cairo, pixman, FreeType, libpng and zlib and therefore redistributes
them.
EOF

# --- the installer ----------------------------------------------------------
# Built LAST, from the staged tree, so NAMp-rations-install.exe carries exactly
# the bundle that is also loose in the ZIP - the same stripped binary and the same
# moduleinfo.json. Building it from build-win instead would quietly ship an
# unstripped, unverified copy the moment either step above changed.
if [ "${RATIONS_SKIP_INSTALLER:-0}" = "1" ]; then
  echo
  echo "WARNING: RATIONS_SKIP_INSTALLER=1 - the ZIP has no NAMp-rations-install.exe." >&2
  echo
else
  # VIProductVersion wants exactly four components; project() gives three.
  VERSION4="$VERSION"
  while [ "$(printf '%s' "$VERSION4" | tr -cd '.' | wc -c)" -lt 3 ]; do
    VERSION4="$VERSION4.0"
  done

  echo "building NAMp-rations-install.exe with $MAKENSIS"
  "$MAKENSIS" -V2 -NOCD \
    "-DVERSION=$VERSION" "-DVERSION4=$VERSION4" \
    "-DBUNDLE_DIR=$PKGBUNDLE" "-DDOC_DIR=$PKGDIR" \
    "-DOUTFILE=$PKGDIR/NAMp-rations-install.exe" \
    "$REPO/installer/namp-rations.nsi"

  if [ ! -s "$PKGDIR/NAMp-rations-install.exe" ]; then
    echo "makensis produced no NAMp-rations-install.exe" >&2
    exit 1
  fi
  # It must be a PE executable, not whatever else ended up at that path. file(1)
  # is not guaranteed to be installed, so check the magic directly.
  if [ "$(head -c2 "$PKGDIR/NAMp-rations-install.exe")" != "MZ" ]; then
    echo "NAMp-rations-install.exe is not a PE executable" >&2
    exit 1
  fi
fi

mkdir -p "$REPO/dist"
ZIP="$REPO/dist/NAMp-rations-${VERSION}-windows-x86_64.zip"
rm -f "$ZIP"
# python3 rather than zip(1): zip is not installed everywhere and this needs no
# extra package. -c takes the directory and stores it with its own name at the
# archive root, which is what an extract-anywhere release wants.
( cd "$STAGEDIR" && python3 -m zipfile -c "$ZIP" "NAMp-rations-${VERSION}" )

echo ""
echo "Packaged: $ZIP"
echo ""
echo "Contents:"
python3 -m zipfile -l "$ZIP"
