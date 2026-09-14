#!/usr/bin/env bash
# Cross-build NAMp Rack for 64-bit Windows and package it into dist/.
#
# ONE PROGRAM, AND FIVE PLUG-INS. namp-rack.exe is self-contained -- the amp, its art and its fonts
# are linked in, and plug-in discovery re-execs this same binary in scan-child mode, so there is
# nothing beside it that it needs. Beside it go the five Rations pedals (decision R7), built by this
# same cross build from a pinned submodule: ordinary VST3 bundles that the rack finds through the
# same catalogue and the same out-of-process scan as anybody else's, and that any other Windows host
# will find too. NAMp-Rack-Amp.vst3 is built by every configure and is NOT packaged -- it exists so
# the SDK validator has something to validate.
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
# a machine with no vendor driver.
#
# THIS ARCHIVE IS GPLv3, AND IT IS THE ONLY THING THIS PROJECT SHIPS THAT IS NOT MIT. The ASIO SDK
# is dual-licensed -- proprietary Steinberg agreement, or GPLv3 -- and this project takes the GPLv3
# arm. So there is no signature to wait for and nothing here is gated on one. What GPLv3 asks for
# instead is Corresponding Source, and that IS gated, below: an archive that ships without it is a
# licence breach on the release. The source itself stays MIT and no file was relicensed; only this
# one binary is conveyed under GPLv3, because only this one binary contains ASIO code.
# products/rack/LICENSE-windows-asio.txt is the full explanation and it ships in the archive.
#
# The SDK still reaches the compiler through the untracked dependency sysroot and through nothing
# else. Under the proprietary arm that was a prohibition; under GPLv3 it is a tidiness rule that
# also keeps the proprietary arm available. scripts/build-win-deps.sh puts it there, pinned by
# SHA-256, and scripts/corresponding-source.sh collects the same nine files at release time.
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
#   NAMP_SKIP_CORRESPONDING_SOURCE=1
#                         do not assemble the Corresponding Source tarball. For iterating locally.
#                         The archive is then named -NOSOURCE and MUST NOT be published: shipping a
#                         GPLv3 binary without its source is a licence breach. It changes the NAME
#                         and one text file; every check runs and the binary is identical.
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
# ... and put back the reproducibility strip just took away. See namp_dist_pe_derandomise in
# scripts/dist-common.sh: binutils rewrites the COFF TimeDateStamp with the wall clock as it
# strips, so the SHIPPED binary moved every time even though the build-tree one did not.
namp_dist_pe_derandomise "$PKGEXE"

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

# THE BINARY IS REPRODUCIBLE, AND THIS ASSERTION HAD TO BE REWRITTEN TO MEAN IT.
#
# Stage 7 found the Windows link carrying the wall-clock minute in the PE header's TimeDateStamp,
# which made "the binaries did not move" uncheckable on this platform. -Wl,--no-insert-timestamp
# fixes that, seeded from the toolchain as a _INIT variable -- which the cache takes on the FIRST
# configure only, so a build directory created before that change keeps its clock however often it
# is re-configured, and the only cure is deleting it.
#
# WHAT THE FIRST VERSION OF THIS CHECK GOT WRONG, found by comparing two packaged binaries that
# should have been identical and were not. `objdump -p` prints two fields one word apart in their
# labels: "Time/Date" is the COFF header's stamp, and "Time/Date stamp" is the debug directory's.
# The linker zeroes both. `strip` then rewrites the COFF one with the wall clock and leaves the
# debug one alone -- so this check, which read the debug one, sat at 0 and passed while the shipped
# file moved on every run. Two strips of one input four seconds apart differ in two bytes.
#
# So the stamp is now zeroed after stripping (above) and read out of the bytes rather than out of a
# label, and the assertion covers the artefact that actually ships.
namp_dist_pe_assert_no_timestamp "$PKGEXE" "namp-rack.exe"

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
  tr -s '[:space:]' ' ' < "$REPO/NOTICE" | grep -qF "$ASIO_NOTICE" ||
    namp_dist_die "NOTICE does not carry the ASIO trademark notice verbatim."
fi

# THE SDK IS NOT IN THE WORKING TREE. The phase gate sweeps for this too, and it is repeated here
# because this is the one script that deliberately puts ASIO sources on a compiler's command line,
# so the check belongs where the risk is. Under GPLv3 a committed copy is no longer a licence
# breach -- the source is redistributable, and the Corresponding Source step below redistributes
# exactly these files on purpose. It is still a dependency this tree does not own and the one act
# that would close off a return to the proprietary arm, so the sweep stays at full strength.
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

# --- the pedals --------------------------------------------------------------
# DECISION R7, the Windows half. Five plug-ins built by this same cross build from a pinned
# submodule and shipped beside the rack, which finds them through the same catalogue and the same
# out-of-process scan as anybody else's. They get no privileged route in, which is the point.
#
# The list comes from CMakeCache.txt rather than from a list restated here, so it is what the
# configure that produced these binaries actually decided.
PEDAL_BUNDLES="$(sed -n 's/^NAMPRACK_PEDAL_BUNDLES:INTERNAL=//p' "$BUILD/CMakeCache.txt" |
                 tr ';' ' ')"
[ -n "$PEDAL_BUNDLES" ] ||
  namp_dist_die "the build declared no NAMPRACK_PEDAL_BUNDLES, so it was configured with
-DNAMPRACK_BUILD_PEDALS=OFF. This release ships the pedals: reconfigure with it ON."

mkdir -p "$PKGDIR/pedals"
PEDAL_COUNT=0
for _pedal in $PEDAL_BUNDLES; do
  _src="$BUILD/VST3/Release/${_pedal}.vst3"
  [ -d "$_src" ] || namp_dist_die "${_pedal}.vst3 was not built at $_src."
  cp -r "$_src" "$PKGDIR/pedals/"
  _pkg="$PKGDIR/pedals/${_pedal}.vst3"

  # A build directory configured for another platform at some point keeps the old architecture
  # folder, and cp -r then carries a Linux .so into the Windows ZIP. Same prune, same reason, as
  # the sibling product's bundle.
  for _arch in "$_pkg/Contents"/*/; do
    _arch="${_arch%/}"
    case "$(basename "$_arch")" in
      Resources | x86_64-win) ;;
      *)
        echo "warning: removing $(basename "$_arch")/ from ${_pedal}.vst3 - not a Windows" >&2
        echo "  architecture folder. Delete $BUILD and re-run to stop seeing this." >&2
        rm -rf "$_arch" ;;
    esac
  done

  # SHAPE. module_win32.cpp's validateBundleStructure requires the inner DLL to be named exactly
  # like the bundle folder. A correctly-built-looking bundle with the binary somewhere else simply
  # does not load, and nothing before this point would say so.
  _dll="$_pkg/Contents/x86_64-win/${_pedal}.vst3"
  [ -f "$_dll" ] || namp_dist_die "no ${_pedal}.vst3 binary inside
$_pkg/Contents/x86_64-win/ - the bundle layout is wrong and no host can load it."
  "$STRIP" --strip-unneeded "$_dll"
  namp_dist_pe_derandomise "$_dll"
  namp_dist_pe_assert_no_timestamp "$_dll" "${_pedal}.vst3"

  for _res in "Contents/Resources/img/pedal-$(printf '%s' "$_pedal" | sed 's/^Rations//' | tr 'A-Z' 'a-z').png" \
              Contents/Resources/fonts/Michroma-Regular.ttf \
              Contents/Resources/fonts/Roboto-Regular.ttf; do
    [ -f "$_pkg/$_res" ] || namp_dist_die "${_pedal}.vst3 is missing $_res."
  done
  PEDAL_COUNT=$((PEDAL_COUNT + 1))
done
[ "$PEDAL_COUNT" = "5" ] ||
  namp_dist_die "packaged $PEDAL_COUNT pedals, expected 5. A release that ships four of five
pedals looks complete and is not."

# Their attribution travels with their binaries: this archive redistributes them, and therefore
# the third-party components they vendor, which are not the ones this project's own NOTICE covers.
cp "$REPO/rations-pedals/NOTICE" "$PKGDIR/pedals/NOTICE"

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

  #-------------------------------------------------------------------------
  # THE PEDALS, under Wine: moduleinfo.json, then the SDK validator on each.
  #
  # moduleinfo.json cannot be a build step here. CMake would run the freshly cross-built
  # moduleinfotool.exe natively on the build host, where it cannot execute -- which is why the
  # toolchain forces SMTG_CREATE_MODULE_INFO off when cross-compiling. It is OPTIONAL to a host
  # (Module::getModuleInfoPath returns an empty optional when it is absent) but it is what lets a
  # host list a plug-in's classes without loading its binary, so a release that omits it is a
  # lesser release for one Wine call.
  #
  # The validator runs on the STAGED, STRIPPED, de-randomised copy rather than the one in the
  # build tree, because that is the copy that ships.
  echo "checking the pedals"
  for _pedal in $PEDAL_BUNDLES; do
    _pkg="$PKGDIR/pedals/${_pedal}.vst3"
    _winb="$(winepath -w "$_pkg")"
    wine "$BUILD/bin/moduleinfotool.exe" -create -version "$VERSION" \
         -path "$_winb" \
         -output "$(winepath -w "$_pkg/Contents/Resources/moduleinfo.json")" 2>/dev/null || true
    [ -s "$_pkg/Contents/Resources/moduleinfo.json" ] ||
      namp_dist_die "moduleinfotool produced no moduleinfo.json for ${_pedal}.vst3."

    _val="$(wine "$BUILD/bin/validator.exe" "$_winb" 2>&1 || true)"
    printf '%s' "$_val" | grep -qE '^Result: [0-9]+ tests passed, 0 tests failed' ||
      namp_dist_die "the SDK validator did not pass against ${_pedal}.vst3:
$(printf '%s' "$_val" | tail -20)"
    printf '  %-20s %s\n' "${_pedal}.vst3" "$(printf '%s' "$_val" | grep -E '^Result:')"
  done
fi

# --- the GPLv3 obligations ---------------------------------------------------
# WHAT THIS SECTION IS FOR. With ASIO compiled in, this binary is a combined work containing GPLv3
# code and is conveyed under GPLv3. Two of that licence's requirements are mechanical, so they are
# machine-checked here rather than remembered:
#
#   section 4  "give all recipients a copy of this License along with the Program"
#              -> COPYING.GPL-3 goes in the archive.
#   section 6  the Complete Corresponding Source must be available to whoever got the binary
#              -> scripts/corresponding-source.sh assembles it, and CORRESPONDING-SOURCE.txt --
#                 the "clear directions" 6(d) asks for -- goes in the archive beside the .exe.
#
# THE MARK IS MEASURED NOW, NOT DECLARED. An earlier version of this script named the archive
# -TESTBUILD unless it was TOLD the Steinberg agreement had been signed, because nothing here could
# check that. Under GPLv3 there is nothing to declare and the one thing that matters is something
# this script can actually verify: whether the Corresponding Source for this exact binary exists.
# So the mark now tracks a fact. An archive named -NOSOURCE is one whose source was not assembled,
# and publishing it would be the breach.
CS_DIRECTIONS="$PKGDIR/CORRESPONDING-SOURCE.txt"
NOSOURCE=""
if [ "$WANT_ASIO" != "ON" ]; then
  # No ASIO, no GPLv3 code, no obligation: a WASAPI-only build is MIT like everything else here.
  echo "WASAPI-only build: MIT, no Corresponding Source obligation."
elif [ "${NAMP_SKIP_CORRESPONDING_SOURCE:-0}" = "1" ]; then
  NOSOURCE="asked for with NAMP_SKIP_CORRESPONDING_SOURCE=1"
elif ! git -C "$REPO" diff --quiet HEAD -- 2>/dev/null; then
  # Corresponding Source is identified by a commit. A dirty tree cannot produce it honestly, so the
  # archive is marked rather than the build refused -- this is the ordinary state while working,
  # and the whole point is that building is never gated.
  NOSOURCE="the working tree has uncommitted changes"
else
  echo
  echo "== Corresponding Source (GPLv3 section 6) =="
  "$REPO/scripts/corresponding-source.sh" "$PRODUCT/dist"
  "$REPO/scripts/corresponding-source.sh" --directions > "$CS_DIRECTIONS"
fi

if [ -n "$NOSOURCE" ]; then
  cat > "$PKGDIR/NOT-A-RELEASE.txt" <<NOTEOF
This archive is NOT a release and MUST NOT be published.

It contains a GPLv3 binary -- namp-rack.exe with the ASIO backend compiled in --
whose Corresponding Source was not assembled, because ${NOSOURCE}.
GPLv3 section 6 requires that source to be available to anyone who receives the
binary, so distributing this archive as it stands would be a licence breach.

The binary itself is fine and every check passed. To make a publishable archive,
commit the tree and run products/rack/scripts/makedist-windows.sh again without
NAMP_SKIP_CORRESPONDING_SOURCE.
NOTEOF
fi

# --- licence, attribution, instructions --------------------------------------
cp "$REPO/NOTICE" "$REPO/LICENSE" "$REPO/README.md" "$PKGDIR/"
if [ "$WANT_ASIO" = "ON" ]; then
  cp "$PRODUCT/COPYING.GPL-3" "$PRODUCT/LICENSE-windows-asio.txt" "$PKGDIR/"
fi

# The licence paragraph differs between the two configurations and says so plainly, because "this
# program is MIT" printed on a GPLv3 binary is the kind of wrong that a user acts on.
if [ "$WANT_ASIO" = "ON" ]; then
  LICENCE_PARA="This build is under the GNU GPL version 3, not MIT, and the reason is worth
knowing: it hosts ASIO, Steinberg's SDK for that is dual-licensed, and this
project takes its GPLv3 arm rather than sign a proprietary agreement. So you
get this program under GPLv3 and you are entitled to its complete source.
COPYING.GPL-3 has the terms, CORRESPONDING-SOURCE.txt says where the source is,
and LICENSE-windows-asio.txt explains the whole arrangement in one page.

The project's own code is MIT and stays MIT - see LICENSE. Only this particular
binary, the Windows one with ASIO in it, is GPLv3. The Linux build, the plug-ins
and the LV2 bundle are all MIT."
else
  LICENCE_PARA="MIT. See LICENSE. This build has no ASIO in it, which is what keeps it MIT -
the Windows build WITH ASIO is GPLv3 instead."
fi

cat > "$PKGDIR/INSTALL.txt" <<EOF
NAMp Rack ${VERSION} - a four-channel Neural Amp Modeler amp head, and a rack
for your plug-ins, for 64-bit Windows

This archive holds one program:

    namp-rack.exe

That is the whole program. The amp, its art and its fonts are inside it, so
there is nothing to install and nothing for it to go looking for. Put it
wherever you like and run it.

...and five pedals, in pedals\\:

    RationsBoost      a Tube Screamer-style overdrive
    RationsChorus     two modulated taps per channel
    RationsFlanger    swept comb with feedback
    RationsDelay      tempo-syncable, optional ping-pong
    RationsReverb     a Freeverb-lineage room

These are ordinary VST3 plug-ins. Copy the five .vst3 folders into your VST3
folder - usually C:\\Program Files\\Common Files\\VST3 - and they turn up in the
rack's plug-in list after a rescan, exactly like any other plug-in you have
installed and through exactly the same route. Nothing about them is special to
this program, and any other host on the machine will find them as well. Leaving
them where they are works too: the rack also scans its own directory.

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
${LICENCE_PARA}

NOTICE has the third-party attribution, which matters more for this build than
for the Linux one: it statically links cairo, pixman, FreeType, libpng and zlib
and therefore redistributes them.

The five pedals are MIT, separately from all of the above - they are their own
binaries, not part of namp-rack.exe - and carry their own attribution in
pedals\\NOTICE.

ASIO is a trademark of Steinberg Media Technologies GmbH, registered in Europe
and other countries.
EOF

MARK=""
if [ -n "$NOSOURCE" ]; then
  MARK="-NOSOURCE"
  echo
  echo "WARNING: this archive contains a GPLv3 binary whose Corresponding Source was" >&2
  echo "not assembled, because $NOSOURCE." >&2
  echo "It is named -NOSOURCE and MUST NOT be published -- GPLv3 section 6 requires the" >&2
  echo "source to be available to whoever receives the binary. The binary itself is fine" >&2
  echo "and every check above passed; commit the tree and re-run to get a releasable one." >&2
  echo
fi

# THE LICENCE FILES ARE ASSERTED, NOT ASSUMED. GPLv3 section 4 requires the licence text to travel
# with the program and section 6(d) requires the directions to the source to sit next to the object
# code. Both are one `cp` away from being silently dropped by an edit to the block above, and
# neither absence would break anything a user would notice -- which is exactly the shape of mistake
# a gate is for.
if [ "$WANT_ASIO" = "ON" ]; then
  namp_dist_require_files "$PKGDIR" "the GPLv3 archive" \
    COPYING.GPL-3 LICENSE-windows-asio.txt NOTICE LICENSE
  if [ -z "$NOSOURCE" ]; then
    namp_dist_require_files "$PKGDIR" "the GPLv3 archive" CORRESPONDING-SOURCE.txt
    CS_TARBALL="$PRODUCT/dist/namp-rack-${VERSION}-corresponding-source.tar.gz"
    [ -f "$CS_TARBALL" ] ||
      namp_dist_die "the Corresponding Source tarball was not produced at $CS_TARBALL. A GPLv3
binary may not be published without it."
  else
    namp_dist_require_files "$PKGDIR" "the unpublishable archive" NOT-A-RELEASE.txt
  fi
  # The GPL text must be the real one. A truncated or reflowed copy is not the licence, and this
  # file is copied around by hand often enough to be worth checking rather than trusting.
  grep -qx '                       Version 3, 29 June 2007' "$PKGDIR/COPYING.GPL-3" ||
    namp_dist_die "COPYING.GPL-3 in the archive is not the verbatim GPLv3 text."
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
