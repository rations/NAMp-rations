#!/usr/bin/env bash
# Cross-build NAMp Rack for 64-bit Windows and package it into dist/.
#
# ONE PROGRAM. namp-rack.exe is the whole archive: the amp, its art and its fonts are linked in,
# and plug-in discovery re-execs this same binary in scan-child mode, so there is nothing beside it
# to install. NAMp-Rack-Amp.vst3 is built by every configure and is NOT packaged -- it exists so the
# SDK validator has something to validate.
#
# VST3 HOSTING ONLY ON WINDOWS. lilv, suil and jalv's event buffer are Linux-shaped here, so
# NAMPRACK_BUILD_LV2_HOST is OFF and the rack's CMakeLists refuses the combination loudly rather
# than ifdef'ing around it. A Windows user's LV2 plug-ins will not appear, and that is by
# construction rather than by a missing dependency.
#
# ASIO IS ON, AND BUILDING IT IS NOT GATED ON ANYTHING.
#
# ASIO is what interface drivers actually expose and what the latency of this program depends on,
# so it is the backend the Windows standalone opens first, with WASAPI as the runtime fallback for
# a machine with no vendor driver. The SDK is dual-licensed, this project is on the proprietary arm
# because the GPLv3 arm is closed to it, and under that arm PUBLISHING a binary requires the
# countersigned agreement -- but building, testing and every gate below require nothing from
# anybody. That distinction is the licence's own and it is deliberate here: this script builds,
# checks and packages a testable ASIO binary with no agreement in sight.
#
# The SDK reaches the compiler through the dependency sysroot and through nothing else, because it
# is untracked build output and a copy inside the repository would be a licence breach that a later
# deletion does not undo. scripts/build-win-deps.sh puts it there.
#
# ASIO is asked for EXPLICITLY below, which matters: left to default, a missing SDK demotes the
# configure to WASAPI-only with a warning, and the point of running this script is to get an ASIO
# binary. Asked for by name, a missing SDK is a configure error instead.
#
# NO WINDOWS MACHINE IS INVOLVED. The compiler is MinGW-w64 running here and the verification runs
# the cross-built binary under Wine, which proves it loads, prints its usage and draws its editor
# pages the way the Linux build does. WINE HAS NO ASIO DRIVER AT ALL and its WASAPI is a shim over
# this machine's own sound server, so nothing here says anything about audio timing, dropouts or
# whether a real driver works. That is a real Windows machine's job, and it is the reason this
# script exists.
#
# Environment:
#   NAMP_SKIP_ASIO=1      configure WASAPI-only, for a build that touches no third-party SDK.
#   NAMP_ASIO_AGREEMENT_SIGNED=1
#                         drop the -TESTBUILD mark from the archive name. Nothing here can know
#                         whether the agreement is signed, so it is declared rather than detected.
#                         It changes the NAME only: every check runs and the binary is identical.
#   WINEPREFIX            defaults to ~/.wine-rations.
#   NAMP_RACK_SKIP_WINE=1 package without the Wine verification. Says so loudly.
set -euo pipefail

PRODUCT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$PRODUCT/../.." && pwd)"
BUILD="${NAMP_RACK_WIN_BUILD_DIR:-$REPO/build-win-rack}"
TRIPLE="${NAMP_WIN_TRIPLE:-x86_64-w64-mingw32}"
OBJDUMP="$TRIPLE-objdump"
STRIP="$TRIPLE-strip"

. "$REPO/scripts/dist-common.sh"

command -v "$TRIPLE-g++" >/dev/null || {
  echo "error: $TRIPLE-g++ not found. Install the MinGW-w64 cross toolchain:" >&2
  echo "  sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64" >&2
  exit 1
}
SYSROOT_DIR="${NAMP_WIN_SYSROOT:-${NAMPRACK_WIN_SYSROOT:-$HOME/third_party/win-deps/sysroot}}"
if [ ! -d "$SYSROOT_DIR/lib/pkgconfig" ]; then
  echo "error: the Windows dependency sysroot is missing. Build it first:" >&2
  echo "  scripts/build-win-deps.sh" >&2
  exit 1
fi

WANT_ASIO=ON
[ "${NAMP_SKIP_ASIO:-0}" = "1" ] && WANT_ASIO=OFF

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-mingw-w64.cmake" \
      -DNAMP_PRODUCT=rack -DNAMPRACK_BUILD_LV2_HOST=OFF \
      -DNAMPRACK_WIN_ASIO="$WANT_ASIO" -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

VERSION="$(namp_dist_version "$PRODUCT/CMakeLists.txt")"

STAGEDIR="$(mktemp -d)"
PKGDIR="$STAGEDIR/pkg"
mkdir -p "$PKGDIR"
trap 'rm -rf "$STAGEDIR"' EXIT

EXE="$BUILD/namp-rack.exe"
[ -f "$EXE" ] || namp_dist_die "namp-rack.exe was not built at $EXE"
cp "$EXE" "$PKGDIR/"
PKGEXE="$PKGDIR/namp-rack.exe"
"$STRIP" --strip-unneeded "$PKGEXE"

# --- gates ------------------------------------------------------------------
# IMPORTS. Nothing but Windows' own DLLs.
#
# The MinGW toolchain's default is to link libgcc_s_seh-1.dll, libstdc++-6.dll and
# libwinpthread-1.dll beside the binary. For this program that is not fatal the way it is for a
# VST3 bundle -- an .exe does search its own directory -- but the archive is ONE FILE by design,
# and three runtime DLLs silently added to it is the archive becoming something else. The -static
# in cmake/toolchain-mingw-w64.cmake is what prevents it and it is one edit away from being lost.
#
# The list is this build's, measured rather than copied from the plug-in's: AVRT is WASAPI's MMCSS
# thread-priority call, WINMM is the MIDI input, and ole32 is the COM that both WASAPI's device
# enumerator and the ASIO driver list need. The plug-in imports none of the three and has several
# this does not, which is why the two products do not share one list.
ALLOWED_DLLS='^(KERNEL32|USER32|GDI32|MSIMG32|SHELL32|ADVAPI32|ole32|OLEAUT32|COMDLG32|WINSPOOL|SHLWAPI|WINMM|AVRT|msvcrt|api-ms-win-)'
BAD_IMPORTS="$("$OBJDUMP" -p "$PKGEXE" | sed -n 's/^\tDLL Name: //p' \
  | sed 's/\.dll$//I' | grep -Ev "$ALLOWED_DLLS" || true)"
if [ -n "$BAD_IMPORTS" ]; then
  echo "namp-rack.exe imports non-system DLLs:" >&2
  echo "$BAD_IMPORTS" | sed 's/^/  /' >&2
  echo "This archive is one file. Check that -static survived in" >&2
  echo "cmake/toolchain-mingw-w64.cmake." >&2
  exit 1
fi

# EXPORTS. Exactly one, and the fact that it is not ZERO is a real difference from Linux.
#
# The Linux build of this program exports nothing at all, and its packaging script asserts exactly
# that, because it contains the NAM core and the SDK's plug-in-side classes and dlopens other
# people's plug-ins built against that same SDK. Here it exports GetPluginFactory, and the
# mechanism is the one this tree already knows: core/rationsfactory.cpp is compiled with the SDK's
# dllexport on the factory entry point, so its object file carries a .drectve section reading
# -export:"GetPluginFactory" -- read out of the object with objdump -s -j .drectve -- and the
# linker obeys it. -Wl,--exclude-all-symbols does NOT suppress a .drectve export, because that
# export is explicit rather than automatic. It is the same mechanism that put 151 FreeType symbols
# in the parent project's bundle.
#
# WHY IT IS TOLERATED HERE rather than chased out: PE has no global symbol interposition. A plug-in
# DLL this program LoadLibrary's resolves its imports from its own import table and never binds to
# an .exe's exports, so the collision the Linux rule prevents cannot happen this way round. What is
# NOT tolerated is the set GROWING, which is exactly how the FreeType case announced itself, so the
# assertion is the exact set rather than a cap.
EXPORTS="$("$OBJDUMP" -p "$PKGEXE" \
  | sed -n '/\[Ordinal\/Name Pointer\] Table/,$p' \
  | sed -n 's/^\t\[ *[0-9]* *\] +base\[ *[0-9]* *\] *[0-9a-f]* \(.*\)$/\1/p' \
  | sort)"
EXPECTED="GetPluginFactory"
if [ "$EXPORTS" != "$EXPECTED" ]; then
  echo "namp-rack.exe does not export exactly [$EXPECTED]." >&2
  echo "found ($(printf '%s' "$EXPORTS" | grep -c . || true)):" >&2
  printf '%s\n' "$EXPORTS" | head -20 | sed 's/^/  /' >&2
  echo "A GROWN list means a static archive's .drectve export directives are being" >&2
  echo "obeyed -- check what was linked in and whether it was built with dllexport" >&2
  echo "still in effect. --exclude-all-symbols does not catch this." >&2
  exit 1
fi

# THE BINARY IS REPRODUCIBLE. Stage 7 found the Windows link carrying the wall-clock minute in the
# PE header's TimeDateStamp, which made "the binaries did not move" uncheckable on this platform.
# -Wl,--no-insert-timestamp fixes it, and it is seeded from the toolchain as a _INIT variable --
# which the cache takes on the FIRST configure only. So a build directory created before that
# change keeps its clock no matter how often it is re-configured, and the only cure is deleting it.
# Assert the field rather than trusting the flag is still reaching this build.
PE_STAMP="$("$OBJDUMP" -p "$PKGEXE" | sed -n 's/^Time\/Date stamp[[:space:]]*//p' | head -1)"
if [ "$PE_STAMP" != "0" ]; then
  echo "namp-rack.exe carries a PE TimeDateStamp of $PE_STAMP, so this build is not" >&2
  echo "reproducible. -Wl,--no-insert-timestamp seeds the cache from the toolchain on" >&2
  echo "the FIRST configure only: delete $BUILD and re-run." >&2
  exit 1
fi

# --- the audio backends ------------------------------------------------------
EXE_STRINGS="$STAGEDIR/namp-rack.strings"
strings "$PKGEXE" > "$EXE_STRINGS"

# WASAPI IS ALWAYS IN. It is the fallback for a machine with no vendor driver, so a build without
# it is one that plays nothing on an ordinary laptop.
grep -qxF 'N7Rations13WasapiBackendE' "$EXE_STRINGS" ||
  namp_dist_die "namp-rack.exe carries no WasapiBackend; it would have no audio at all on a
machine with no ASIO driver."

if [ "$WANT_ASIO" = "ON" ]; then
  # ASIO IS REALLY IN, and this is what the script is for. A configure that demoted to WASAPI-only
  # produces a binary that builds, runs, opens its editor and cannot reach the interface most of
  # its users own -- and says so in one status line nobody re-reads.
  #
  # THE TWO MARKERS WERE CHOSEN BY BUILDING BOTH AND COMPARING, not by looking at the ASIO build
  # alone, because a marker present in both proves nothing. Measured on a stripped ASIO build
  # against a stripped -DNAMPRACK_WIN_ASIO=OFF build of the same tree:
  #
  #     N7Rations11AsioBackendE    ASIO yes / WASAPI-only no    <- our backend's RTTI
  #     software\asio              ASIO yes / WASAPI-only no    <- the SDK's own registry key
  #     N7Rations13WasapiBackendE  ASIO yes / WASAPI-only yes   <- control, always present
  #     asio-driver                ASIO yes / WASAPI-only YES   <- REJECTED, see below
  #
  # The two that discriminate are used. The RTTI name is our AsioBackend, and typeinfo lives in
  # .rdata and survives --strip-unneeded; "software\asio" is the driver-enumeration registry key,
  # a string literal from the SDK's own asiolist.cpp, so it proves the SDK SOURCES were compiled in
  # rather than only our wrapper around them.
  #
  # What was tried first and does NOT work: the SDK's translation-unit names, asio.cpp /
  # asiodrivers.cpp / asiolist.cpp. Those are in the SYMBOL TABLE, not in .rodata, so they are
  # present in the build tree's binary and gone from the stripped copy this script packages -- the
  # check passed by hand and failed in the script. And "asio-driver", the command-line option, is
  # in BOTH builds: the option string is compiled unconditionally even though --help does not offer
  # it without ASIO. A check on it would have passed on a WASAPI-only build.
  grep -qxF 'N7Rations11AsioBackendE' "$EXE_STRINGS" ||
    namp_dist_die "namp-rack.exe carries no AsioBackend. The configure demoted to WASAPI-only --
run scripts/build-win-deps.sh to put the ASIO SDK in the dependency sysroot."
  grep -qxF 'software\asio' "$EXE_STRINGS" ||
    namp_dist_die "namp-rack.exe does not carry the ASIO driver registry key, so the SDK's own
sources were not compiled into it even though the backend is present."

  # THE TRADEMARK NOTICE IS IN THE PRODUCT, NOT ONLY IN NOTICE, and that is a licence condition
  # rather than a courtesy: the agreement requires "ASIO" and Steinberg's notice in an About box,
  # a startup screen or the bundled documentation of a downloadable product. The string is fixed
  # and is reproduced exactly, so it is checked exactly -- a reworded version is not compliance.
  ASIO_NOTICE='ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries'
  grep -qxF "$ASIO_NOTICE" "$EXE_STRINGS" ||
    namp_dist_die "namp-rack.exe does not carry the ASIO trademark notice verbatim. The licence
requires it in the product itself, not only in NOTICE."
  # NOTICE is prose and wraps, so the sentence is matched with whitespace collapsed rather than
  # line by line. "Verbatim" is a claim about the WORDS -- the agreement fixes the sentence, not
  # its line breaks -- and a check that demanded one line would fail on a correctly formatted file.
  # It did: the notice sits across two lines at NOTICE:265-266 and the first version of this gate
  # called it missing.
  tr -s '[:space:]' ' ' < "$PRODUCT/NOTICE" | grep -qF "$ASIO_NOTICE" ||
    namp_dist_die "products/rack/NOTICE does not carry the ASIO trademark notice verbatim."
fi

# THE SDK IS NOT IN THE WORKING TREE. The phase gate sweeps for this too, and it is repeated here
# because this is the one script that deliberately puts ASIO sources on a compiler's command line:
# a breach committed is not undone by a later deletion, so the check belongs where the risk is.
#
# EXACT BASENAMES, NEVER GLOBS, and the reason is measured rather than stylistic. asiodrivers.*
# also matches asiodrivers.cpp.obj, and a Windows build directory is FULL of those -- CMake mirrors
# the sysroot's source path under CMakeFiles/, so the first version of this check reported the
# build's own object files as a licence breach. It is also why asiobackend.{h,cpp}, which is our
# own code, must not match. This is the same list the phase gate sweeps, kept identical on purpose:
# two sweeps that disagree about what an ASIO file is are worse than one.
ASIO_NAMES='asio.h asio.cpp asiosys.h iasiodrv.h asiodrvr.h asiodrvr.cpp
            asiodrivers.h asiodrivers.cpp asiolist.h asiolist.cpp
            ASIOConvertSamples.h ASIOConvertSamples.cpp'
ASIO_STRAY=""
for _n in $ASIO_NAMES; do
  _hits="$(find "$REPO" -name "$_n" -not -path '*/.git/*' 2>/dev/null || true)"
  [ -n "$_hits" ] && ASIO_STRAY="$ASIO_STRAY$_hits
"
done
if [ -n "$ASIO_STRAY" ]; then
  echo "ASIO SDK files are inside the repository working tree:" >&2
  echo "$ASIO_STRAY" | sed 's/^/  /' >&2
  echo "The SDK may never be committed, in whole or in part. It reaches the build" >&2
  echo "through the untracked dependency sysroot and through nothing else." >&2
  exit 1
fi

# --- verification under Wine -------------------------------------------------
if [ "${NAMP_RACK_SKIP_WINE:-0}" = "1" ]; then
  echo
  echo "WARNING: NAMP_RACK_SKIP_WINE=1 - the binary was NOT run and the editor pages" >&2
  echo "were NOT compared against the Linux render." >&2
  echo
else
  command -v wine >/dev/null || {
    echo "error: wine not found. Install it, or set NAMP_RACK_SKIP_WINE=1." >&2
    exit 1
  }
  export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-rations}"
  export WINEDEBUG="${WINEDEBUG:--all}"
  [ -d "$WINEPREFIX" ] || { echo "creating a Wine prefix at $WINEPREFIX"; wineboot --init >/dev/null 2>&1 || true; }

  echo "running namp-rack.exe --help under Wine"
  HELP_OUT="$(wine "$PKGEXE" --help 2>&1 || true)"
  printf '%s' "$HELP_OUT" | grep -q "usage: namp-rack" || {
    echo "namp-rack.exe --help did not print its usage:" >&2
    printf '%s\n' "$HELP_OUT" | head -20 >&2
    exit 1
  }

  #-------------------------------------------------------------------------
  # THE PANEL DIFF: does the Windows editor DRAW what the Linux one draws?
  #
  # The only automated check of this editor's appearance on a platform nobody here runs, and the
  # reason scripts/build-win-deps.sh pins its five dependencies to this machine's system versions.
  # Both panelrender binaries are built from the same sources against the same resources/ tree at
  # the same scale, so the comparison exercises cairo, FreeType rasterisation, PNG decode, NanoSVG
  # and the whole of core/gfx in the cross build.
  #
  # TWO PAGES, NOT FOUR. This product's editor is Head and Setup; the pedalboard page is what the
  # rack replaced, and its Setup page is the one with two independently scrolling columns.
  #
  # THE THRESHOLDS ARE THE SIBLING'S, AND THIS PRODUCT'S FIRST RUN CONFIRMED THEM. As first
  # measured here, at cairo 1.18.4 / freetype 2.13.3 / pixman 0.44.0 / libpng 1.6.48 / zlib 1.3.1:
  #
  #     head         32 of   456,599 px    worst channel delta 1/255
  #     setup         0 of 1,071,818 px    byte-identical
  #     TOTAL        32 of 1,528,417 px
  #
  # Exactly the sibling's pattern and, on the page the two products share, exactly its figure: its
  # head page also differs by 32 pixels at delta 1. Both editors draw that page's rotated dials
  # with the same code, and a rotated dial is where the trigonometry and so the float rounding
  # lives. Setup has no dials of its own and comes out identical, as the sibling's settings page
  # does. The caps sit well above this so an ordinary art edit does not trip them, while a real
  # divergence -- a different FreeType or cairo -- moves glyph rasterisation by thousands of pixels
  # at full contrast, three orders of magnitude away.
  #
  # MAX_DELTA IS THE SHARPER GATE. A rounding difference is 1/255 by definition; a real
  # rasterisation change puts down ink where there was none, which is tens or hundreds.
  #-------------------------------------------------------------------------
  PANEL_MAX_PIXELS=256                # per page
  PANEL_MAX_DELTA=1                   # per channel, any page
  PANEL_PAGES="head setup"            # this product's pages

  LINUX_PANELRENDER="${NAMP_RACK_BUILD_DIR:-$REPO/build-rack}/panelrender"
  if [ ! -x "$LINUX_PANELRENDER" ]; then
    echo "no Linux panelrender at $LINUX_PANELRENDER - build the native tree first:" >&2
    echo "  cmake -S . -B build-rack -G Ninja -DCMAKE_BUILD_TYPE=Release -DNAMP_PRODUCT=rack" >&2
    echo "  cmake --build build-rack" >&2
    exit 1
  fi

  echo "comparing the editor pages against the Linux render"
  PANELS="$STAGEDIR/panels"
  mkdir -p "$PANELS"
  "$LINUX_PANELRENDER" "$PANELS/lin" "$PRODUCT/resources" 1.0 >/dev/null
  # The resource directory is passed explicitly rather than left to respath.cpp's module-relative
  # fallback, which resolves to nothing for a bare .exe sitting outside a bundle.
  wine "$BUILD/panelrender.exe" "$(winepath -w "$PANELS")\\win" \
       "$(winepath -w "$PRODUCT/resources")" 1.0 >/dev/null

  PANEL_MAX_PIXELS="$PANEL_MAX_PIXELS" PANEL_MAX_DELTA="$PANEL_MAX_DELTA" \
  PANEL_PAGES="$PANEL_PAGES" \
    "$REPO/scripts/panel-diff.sh" "$PANELS" lin "$PANELS" win Linux Windows
fi

# --- licence, attribution, instructions --------------------------------------
cp "$PRODUCT/NOTICE" "$REPO/LICENSE" "$PKGDIR/"

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rack ${VERSION} - a four-channel Neural Amp Modeler amp head, and a rack
for your plug-ins, for 64-bit Windows

This archive holds one file:

    namp-rack.exe

That is the whole program. The amp, its art and its fonts are inside it, so
there is nothing to install and nothing for it to go looking for. Put it
wherever you like and run it.

Audio
-----
It opens ASIO first, because that is what your interface's own driver exposes
and what the latency depends on, and falls back to WASAPI when the machine has
no ASIO driver installed. --list-devices shows what it can see.

If you have an audio interface, install its manufacturer's ASIO driver and use
that. ASIO4ALL is a wrapper rather than a driver and will work, but the latency
is not what a real driver gives you.

Plug-ins
--------
It hosts VST3. It does NOT host LV2 on Windows - lilv and suil are Linux
libraries and there is no Windows build of this program that includes them, so
LV2 plug-ins will not appear in the list and that is by design rather than a
missing dependency.

Plug-ins are scanned in a separate process on purpose: one that crashes while it
is being examined takes that process with it and is recorded as bad rather than
retried, so a single broken plug-in on your disk cannot stop the rack starting.

Your settings
-------------
Saved racks, which captures were loaded, your audio device choice and your MIDI
bindings live under %APPDATA%\\NAMp-Rack. The plug-in scan cache lives under
%LOCALAPPDATA%\\NAMp-Rack and can be deleted at any time; a rescan rebuilds it.

Captures
--------
NAMp Rack ships NO captures - it plays yours, and it wants four sets of them.
Open the setup page and point each channel's loader at a folder of .nam files,
or at a single .nam. That folder's name becomes the channel's name, and each
channel's dial then sweeps that whole bank continuously.

Captures are ordered by the number in the filename (the LAST run of digits, so
"GAIN 2" comes before "GAIN 10"), then anything ending in MAX, then anything
with no number, alphabetically. Capture your amp at each mark of its own gain
control and put that mark last in the name, and the dial becomes that amp's gain
control at the amp's own spacing.

A channel with nothing loaded is silent rather than broken. Captures must be
feed-forward (WaveNet or ConvNet); an LSTM capture is refused.

Requirements
------------
64-bit Windows. Nothing else: cairo, FreeType, libpng and zlib are statically
linked, and so is the C++ runtime.

Licence
-------
MIT. See LICENSE, and NOTICE for third-party attribution - which matters more
for this build than for the Linux one, because it statically links cairo,
pixman, FreeType, libpng and zlib and therefore redistributes them.

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe
and other countries.
EOF

MARK=""
if [ "$WANT_ASIO" = "ON" ] && [ "${NAMP_ASIO_AGREEMENT_SIGNED:-0}" != "1" ]; then
  MARK="-TESTBUILD"
  echo
  echo "This build hosts ASIO, and publishing an ASIO binary needs the countersigned" >&2
  echo "Steinberg agreement. Nothing here can know whether that is in place, so the" >&2
  echo "archive is named -TESTBUILD. Building and testing need no agreement and were" >&2
  echo "not gated on one; only the NAME changed. Set NAMP_ASIO_AGREEMENT_SIGNED=1 to" >&2
  echo "drop the mark once it is signed -- the binary is identical either way." >&2
  echo
fi

PKGNAME="namp-rack-${VERSION}${MARK}-windows-x86_64"
mv "$PKGDIR" "$STAGEDIR/$PKGNAME"
mkdir -p "$PRODUCT/dist"
ZIP="$PRODUCT/dist/${PKGNAME}.zip"
rm -f "$ZIP"
# python3 rather than zip(1): zip is not installed everywhere and this needs no extra package.
( cd "$STAGEDIR" && python3 -m zipfile -c "$ZIP" "$PKGNAME" )

echo ""
echo "Packaged: $ZIP"
echo ""
echo "Contents:"
python3 -m zipfile -l "$ZIP"
