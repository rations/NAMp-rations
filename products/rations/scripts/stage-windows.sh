#!/usr/bin/env bash
# Cross-build NAMp Rations for 64-bit Windows, gate the result, and stage it for the release.
#
# THIS SCRIPT PRODUCES NO ARCHIVE AND NO INSTALLER. The Windows release is ONE download with ONE
# NAMp-install.exe in it, installing the plug-in, the standalone and the pedals together; it is
# assembled by scripts/makedist-windows.sh at the repository root, which calls this and the rack's
# stage script in turn. The assertions below are about THIS artefact and stay here beside the
# product that owns them; the installer, the licence files and the ZIP are shared and are written
# once at the root.
#
# WHAT IT STAGES, into <stagedir>/plugin/:
#
#   NAMp-rations.vst3   the amp as a plug-in, for a DAW. A folder, not a file.
#
# NO WINDOWS MACHINE IS INVOLVED. The compiler is MinGW-w64 running here; the verification runs
# the cross-built binaries under Wine, which is enough to prove the bundle loads, passes the SDK
# validator and draws its editor pages the same way the Linux build does. It is NOT a substitute
# for a test on real Windows, which is the actual gate before a release goes out.
#
# Environment:
#   WINEPREFIX          defaults to ~/.wine-rations — a prefix of its own, because a desktop's
#                       ~/.wine is usually managed by something else (here, by vstbridge).
#   NAMP_SKIP_WINE      set to 1 to stage without the Wine verification. The bundle is then
#                       unverified AND has no moduleinfo.json; the script says so loudly rather
#                       than quietly producing a lesser build.
#   NAMP_BUILD_DIR      the NATIVE Linux build directory, whose panelrender produces the
#                       reference render the Windows one is compared against. Defaults to build/.
set -euo pipefail

# PRODUCT is this product's own directory; REPO is the REPOSITORY root, and they have been
# different places since the two products moved under products/. This script needs both: the build
# directory, the CMake source root and the shared packaging machinery are the repository's, while
# the resources and the lists file that carries the version are this product's. Naming the product
# directory "REPO" is what hid three separate breakages here, so it is named for what it is.
PRODUCT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$PRODUCT/../.." && pwd)"
BUILD="${NAMP_WIN_BUILD_DIR:-$REPO/build-win}"
TRIPLE="${NAMP_WIN_TRIPLE:-${RATIONS_WIN_TRIPLE:-x86_64-w64-mingw32}}"
OBJDUMP="$TRIPLE-objdump"
STRIP="$TRIPLE-strip"

. "$REPO/scripts/dist-common.sh"

STAGE="${1:-}"
[ -n "$STAGE" ] && [ -d "$STAGE" ] ||
  namp_dist_die "usage: stage-windows.sh <stagedir>   (an existing directory to stage into)
This script stages; it packages nothing. Run scripts/makedist-windows.sh to build a release."

command -v "$TRIPLE-g++" >/dev/null || {
  echo "error: $TRIPLE-g++ not found. Install the MinGW-w64 cross toolchain:" >&2
  echo "  sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64" >&2
  exit 1
}
SYSROOT_DIR="${NAMP_WIN_SYSROOT:-${RATIONS_WIN_SYSROOT:-$HOME/third_party/win-deps/sysroot}}"
if [ ! -d "$SYSROOT_DIR/lib/pkgconfig" ]; then
  echo "error: the Windows dependency sysroot is missing. Build it first:" >&2
  echo "  scripts/build-win-deps.sh" >&2
  exit 1
fi

cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-mingw-w64.cmake" -S "$REPO"
cmake --build "$BUILD" --parallel "$(nproc)"

VERSION="$(namp_dist_version "$PRODUCT/CMakeLists.txt")"

PKGDIR="$STAGE/plugin"
mkdir -p "$PKGDIR"

# Working space OUTSIDE the staged tree, so nothing here can end up in the release by accident.
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

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

# RESOURCES. This product embeds NO fallback art. core/gfx/resourcestore.h declares
# installBuiltinResources(), because products/rack's single-executable standalone
# links a generated table and needs it; nothing in products/rations links a
# definition, so the built-in table is empty here and there is nothing to fall
# back to. A bundle that reaches a user without Contents/Resources therefore draws
# flat rectangles and says so only on stderr, which nobody reads. This assertion is
# the entire safety net.
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

# THE STRIPPED BUNDLE WAS NEVER REPRODUCIBLE, and nothing here was watching. The toolchain passes
# -Wl,--no-insert-timestamp and the linked binary does carry a COFF TimeDateStamp of 0 -- but
# binutils writes a fresh wall-clock value into that field as it strips, so the copy that goes into
# the ZIP moved on every run. Found on the sibling product, which had an assertion and was reading
# the DEBUG directory's stamp rather than the COFF one; this script had no assertion at all, so it
# had nothing to be wrong about. Both now zero the field after stripping and assert it, through one
# pair of helpers in scripts/dist-common.sh.
namp_dist_pe_derandomise "$DLL"
namp_dist_pe_assert_no_timestamp "$DLL" "NAMp-rations.vst3"

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
if [ "${NAMP_SKIP_WINE:-${RATIONS_SKIP_WINE:-0}}" = "1" ]; then
  echo
  echo "WARNING: NAMP_SKIP_WINE=1 - the SDK validator was NOT run against" >&2
  echo "this bundle, the editor pages were NOT compared against the Linux" >&2
  echo "render, and no moduleinfo.json was generated. Do not release this." >&2
  echo
else
  command -v wine >/dev/null || {
    echo "error: wine not found. Install it, or set NAMP_SKIP_WINE=1 to" >&2
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
  PANEL_PAGES="head cabinet pedalboard settings"   # this product's pages; the rack has two

  # This used to be skipped with a warning when ImageMagick was absent, which is
  # a gate that can quietly not run. panel-diff.sh decodes the PNGs with the
  # Python standard library now, so there is nothing left to be missing and
  # nothing left to skip.
  echo "comparing the editor pages against the Linux render"
  PANELS="$SCRATCH/panels"
  mkdir -p "$PANELS"

  # The Linux reference has to come from a Linux build of the same tree.
  LINUX_PANELRENDER="${NAMP_BUILD_DIR:-${RATIONS_BUILD_DIR:-$REPO/build}}/panelrender"
  if [ ! -x "$LINUX_PANELRENDER" ]; then
    echo "no Linux panelrender at $LINUX_PANELRENDER - build the native tree first:" >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    echo "or set NAMP_BUILD_DIR to a native build directory." >&2
    exit 1
  fi

  "$LINUX_PANELRENDER" "$PANELS/lin" "$PRODUCT/resources" 1.0 >/dev/null
  # The resource directory is passed explicitly rather than left to
  # respath.cpp's module-relative fallback, which resolves to nothing for a
  # bare .exe sitting outside a bundle.
  wine "$BUILD/panelrender.exe" "$(winepath -w "$PANELS")\\win" \
       "$(winepath -w "$PRODUCT/resources")" 1.0 >/dev/null

  # The comparison itself is the repository's scripts/panel-diff.sh, which the
  # macOS workflow and the rack's own Windows packaging call too -- one
  # implementation, so every platform's figures are produced the same way and
  # stay comparable. The thresholds and the page list stay HERE because they are
  # this pair's measurement and this product's pages, not that script's.
  PANEL_MAX_PIXELS="$PANEL_MAX_PIXELS" PANEL_MAX_DELTA="$PANEL_MAX_DELTA" \
  PANEL_PAGES="$PANEL_PAGES" \
    "$REPO/scripts/panel-diff.sh" "$PANELS" lin "$PANELS" win Linux Windows
fi


echo "staged the plug-in:"
echo "  plugin/NAMp-rations.vst3"
