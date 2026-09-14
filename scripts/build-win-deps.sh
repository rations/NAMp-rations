#!/usr/bin/env bash
# Build the Windows dependency sysroot: the editor's graphics stack, and the ASIO
# SDK the audio backend hosts.
#
# WHY THIS EXISTS. On Linux, cairo/pixman/freetype/libpng/zlib are system shared
# libraries and CMakeLists.txt just asks pkg-config for them. Debian packages no
# MinGW build of any of them — the entire mingw-cross library set in the archive
# is zlib plus GnuPG's private dependencies — so a Windows build has to bring its
# own. This script produces that prefix.
#
# STATIC, ON PURPOSE. A VST3 bundle cannot ship these as sibling DLLs: the SDK
# loads the plug-in with a plain LoadLibraryW of the full path
# (public.sdk/source/vst/hosting/module_win32.cpp, loadAsPackage), and the
# default DLL search order does not include the loaded module's own directory,
# so a DLL next to the bundle would simply not be found. Everything is linked in
# instead, which is also why makedist-windows.sh can assert that the finished
# plug-in and standalone import nothing but system DLLs.
#
# VERSIONS MATCH THE LINUX BUILD, and that is a test requirement rather than
# tidiness: makedist-windows.sh renders all four editor pages with the Windows
# panelrender under Wine and compares them pixel for pixel against the Linux
# render, and a different FreeType would move glyph rasterisation enough to make
# that comparison meaningless. Pinned against what this machine's Debian/Devuan
# actually ships:
#
#   zlib     1.3.1     (zlib1g-dev  1:1.3.dfsg+really1.3.1-1)
#   libpng   1.6.48    (libpng-dev  1.6.48-1)
#   pixman   0.44.0    (libpixman-1-dev 0.44.0-3)
#   freetype 2.13.3    (libfreetype-dev 2.13.3+dfsg-1)
#   cairo    1.18.4    (libcairo2-dev 1.18.4-1)
#
# WHAT IS DELIBERATELY LEFT OUT.
#   * fontconfig - cairo makes it optional on Windows, and this tree never calls
#     it: there is not one Fc* symbol in the tree. FontStack loads its two TTFs
#     by path. cairo-ft.h guards its fontconfig include behind CAIRO_HAS_FC_FONT,
#     so dropping it costs nothing.
#   * xlib / xcb   - the Windows editor blits a DIB section, not an X drawable.
#   * harfbuzz     - the system FreeType here is built without it too (checked
#     with ldd), so leaving it out is what keeps rasterisation identical.
#   * brotli / bzip2 / freetype's png - these only add font *container* formats
#     (WOFF2, compressed PCF, colour bitmap glyphs). This tree loads plain TTFs.
#   * glib, lzo, spectre, dwrite, quartz, tests, docs.
#
# CONSUMERS MUST DEFINE CAIRO_WIN32_STATIC_BUILD. cairo.h declares every entry
# point __declspec(dllimport) unless that macro is set, and cairo's own .pc file
# does not carry it even though this is a static build — so without it every
# cairo call fails to link as "undefined reference to __imp_cairo_*".
# CMakeLists.txt puts it on the PkgConfig::CAIRO imported target.
#
# THE PREFIX IS SHARED WITH THE PARENT PROJECT. The default root below is the
# same one the parent plug-in NAMp's identical script installs to, deliberately:
# the two want byte-identical dependencies, and building them twice would only
# create a way for them to differ. Override it with $NAMPRACK_WIN_DEPS_ROOT if
# that is not wanted. The sharing is also why the FreeType patch below tests for
# a project-NEUTRAL sentinel — see the comment at that step.
set -euo pipefail

TRIPLE="${TRIPLE:-x86_64-w64-mingw32}"
ROOT="${NAMPRACK_WIN_DEPS_ROOT:-$HOME/third_party/win-deps}"
SYSROOT="${NAMPRACK_WIN_SYSROOT:-$ROOT/sysroot}"
DL="$ROOT/dl"
BUILD="$ROOT/build"
JOBS="$(nproc)"

ZLIB=zlib-1.3.1
LIBPNG=libpng-1.6.48
PIXMAN=pixman-0.44.0
FREETYPE=freetype-2.13.3
CAIRO=cairo-1.18.4

# The ASIO SDK. Pinned by archive name AND by content hash: Steinberg publishes it
# under a dated filename behind a stable redirect, and a re-rolled archive under
# the same name is exactly the kind of upstream move that has to fail loudly here
# rather than quietly change what the product links.
#
# The hash was measured on the archive that URL served, and the extracted tree was
# diffed against the pristine reference checkout on this machine: identical, file
# for file. Re-measure if either moves.
ASIO_ZIP=ASIO-SDK_2.3.4_2025-10-15.zip
ASIO_URL=https://www.steinberg.net/asiosdk
ASIO_SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
ASIO_DIR=ASIOSDK

mkdir -p "$DL" "$BUILD" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

command -v "$TRIPLE-gcc" >/dev/null || { echo "error: $TRIPLE-gcc not found" >&2; exit 1; }
command -v meson >/dev/null || { echo "error: meson not found" >&2; exit 1; }

# pkg-config must see the sysroot and nothing of the host's.
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
export PKG_CONFIG_PATH=""

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
die()  { echo "error: $*" >&2; exit 1; }
done_stamp() { [ -f "$BUILD/.$1.stamp" ]; }
mark()       { touch "$BUILD/.$1.stamp"; }

fetch() { # url filename
    [ -f "$DL/$2" ] && return 0
    echo "fetching $2"
    curl -sSL -o "$DL/$2.part" "$1" && mv "$DL/$2.part" "$DL/$2"
}

unpack() { # tarball dirname
    [ -d "$ROOT/$2" ] && return 0
    tar -xf "$DL/$1" -C "$ROOT"
}

#---------------------------------------------------------------------------
# The meson cross file. Generated rather than checked in: every value in it is
# derived from $TRIPLE and $SYSROOT, so a stale copy could only ever be wrong.
#---------------------------------------------------------------------------
CROSS="$BUILD/cross-$TRIPLE.ini"
cat > "$CROSS" <<EOF
[binaries]
c          = '$TRIPLE-gcc'
cpp        = '$TRIPLE-g++'
ar         = '$TRIPLE-ar'
ranlib     = '$TRIPLE-ranlib'
strip      = '$TRIPLE-strip'
windres    = '$TRIPLE-windres'
pkg-config = 'pkg-config'
exe_wrapper = 'wine'

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = '$SYSROOT/lib/pkgconfig'

[host_machine]
system     = 'windows'
cpu_family = 'x86_64'
cpu        = 'x86_64'
endian     = 'little'
EOF

MESON_COMMON=(
    --cross-file "$CROSS"
    --prefix "$SYSROOT"
    --libdir lib
    --buildtype release
    --default-library static
)

#---------------------------------------------------------------------------
# zlib. Built through its own win32/Makefile.gcc, which is the path upstream
# documents for MinGW; its CMake build insists on producing a shared library
# too. Installed by hand, including a .pc file, because that makefile has no
# install target that knows about pkg-config.
#---------------------------------------------------------------------------
if ! done_stamp zlib; then
    say "zlib $ZLIB"
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/$ZLIB.tar.xz $ZLIB.tar.xz
    unpack $ZLIB.tar.xz $ZLIB
    make -C "$ROOT/$ZLIB" -f win32/Makefile.gcc clean >/dev/null 2>&1 || true
    make -C "$ROOT/$ZLIB" -f win32/Makefile.gcc -j"$JOBS" \
         PREFIX="$TRIPLE-" CFLAGS="-O3 -DNDEBUG" libz.a
    install -m644 "$ROOT/$ZLIB/zlib.h" "$ROOT/$ZLIB/zconf.h" "$SYSROOT/include/"
    install -m644 "$ROOT/$ZLIB/libz.a" "$SYSROOT/lib/"
    cat > "$SYSROOT/lib/pkgconfig/zlib.pc" <<EOF
prefix=$SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: 1.3.1
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
    mark zlib
fi

#---------------------------------------------------------------------------
# libpng. Needed by cairo for PNG surfaces (gfx/image.cpp loads every panel
# layer through cairo_image_surface_create_from_png*). Not needed by FreeType,
# whose png option only covers colour bitmap glyphs.
#---------------------------------------------------------------------------
if ! done_stamp libpng; then
    say "libpng $LIBPNG"
    fetch https://downloads.sourceforge.net/project/libpng/libpng16/1.6.48/$LIBPNG.tar.xz $LIBPNG.tar.xz
    unpack $LIBPNG.tar.xz $LIBPNG
    cmake -S "$ROOT/$LIBPNG" -B "$BUILD/$LIBPNG" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER="$TRIPLE-gcc" \
        -DCMAKE_RC_COMPILER="$TRIPLE-windres" \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DZLIB_INCLUDE_DIR="$SYSROOT/include" \
        -DZLIB_LIBRARY="$SYSROOT/lib/libz.a" \
        -DPNG_SHARED=OFF -DPNG_STATIC=ON \
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_EXECUTABLES=OFF \
        -DPNG_FRAMEWORK=OFF
    cmake --build "$BUILD/$LIBPNG" --parallel "$JOBS"
    cmake --install "$BUILD/$LIBPNG"
    mark libpng
fi

#---------------------------------------------------------------------------
# pixman. cairo's software rasteriser. No external dependencies at all once
# gtk/libpng/tests/demos are off.
#---------------------------------------------------------------------------
if ! done_stamp pixman; then
    say "pixman $PIXMAN"
    fetch https://cairographics.org/releases/$PIXMAN.tar.gz $PIXMAN.tar.gz
    unpack $PIXMAN.tar.gz $PIXMAN
    meson setup "$BUILD/$PIXMAN" "$ROOT/$PIXMAN" "${MESON_COMMON[@]}" \
        -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled \
        -Dopenmp=disabled
    meson compile -C "$BUILD/$PIXMAN" -j "$JOBS"
    meson install -C "$BUILD/$PIXMAN"
    mark pixman
fi

#---------------------------------------------------------------------------
# FreeType. harfbuzz off to match the system build (verified with ldd on
# libfreetype.so.6, which links zlib/bz2/png/brotli but NOT harfbuzz), so glyph
# rasterisation stays identical to the Linux render.
#---------------------------------------------------------------------------
if ! done_stamp freetype; then
    say "freetype $FREETYPE"
    fetch https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/$FREETYPE.tar.xz $FREETYPE.tar.xz
    unpack $FREETYPE.tar.xz $FREETYPE

    # freetype's meson.build defines DLL_EXPORT on Windows unconditionally:
    #     if host_machine.system() == 'windows'
    #       ft2_defines += ['-DDLL_EXPORT=1']
    # with no test of default_library. That makes FT_EXPORT expand to
    # __declspec(dllexport) even in a STATIC build, so every object in
    # libfreetype.a carries a .drectve section full of -export:"FT_..."
    # directives, and the linker honours those in whatever links the archive:
    # the parent project's bundle came out re-exporting 151 FreeType symbols
    # beside its own three entry points. -Wl,--exclude-all-symbols does not
    # help, because a .drectve export is explicit rather than automatic
    # (verified by trying it). makedist-windows.sh's exports gate is what
    # detects a regression here.
    #
    # Removing the define is the fix at the cause. Guarded so that a future
    # freetype release which drops or reshapes these lines fails here loudly
    # rather than silently going back to exporting everything, and idempotent
    # because unpack() keeps an already-extracted tree.
    #
    # THE SENTINEL IS PROJECT-NEUTRAL, AND THAT IS LOAD-BEARING. This prefix is
    # shared with the parent projects, whose copies of this script wrote their
    # own markers into the very same meson.build. A sentinel naming
    # this project would match neither branch on such a tree — the marker is
    # absent AND the DLL_EXPORT line it would look for has already been
    # replaced — so the script would die on a prefix that is in fact correctly
    # patched. Test for the substring both markers share.
    FT_MESON="$ROOT/$FREETYPE/meson.build"
    FT_PATCH_MARK="# DLL_EXPORT removed by build-win-deps.sh, this is a static build"
    if grep -q "DLL_EXPORT removed" "$FT_MESON"; then
        : # already patched, by this script or by the parent project's copy
    elif grep -q "^  ft2_defines += \['-DDLL_EXPORT=1'\]$" "$FT_MESON"; then
        sed -i "s|^  ft2_defines += \['-DDLL_EXPORT=1'\]$|  $FT_PATCH_MARK|" "$FT_MESON"
    else
        die "$FREETYPE/meson.build no longer has the DLL_EXPORT line this script patches; re-check it"
    fi

    meson setup "$BUILD/$FREETYPE" "$ROOT/$FREETYPE" "${MESON_COMMON[@]}" \
        -Dharfbuzz=disabled -Dbrotli=disabled -Dbzip2=disabled \
        -Dpng=disabled -Dzlib=disabled -Dtests=disabled
    meson compile -C "$BUILD/$FREETYPE" -j "$JOBS"
    meson install -C "$BUILD/$FREETYPE"
    mark freetype
fi

#---------------------------------------------------------------------------
# cairo. freetype must be requested EXPLICITLY: cairo's meson.build does
#     freetype_required = host_machine.system() not in ['windows', 'darwin']
#     freetype_option = freetype_option.disable_auto_if(not freetype_required)
# so the default 'auto' resolves to DISABLED when cross-compiling to Windows,
# and a cairo without cairo-ft would fail to build gfx/fontstack.cpp.
#---------------------------------------------------------------------------
if ! done_stamp cairo; then
    say "cairo $CAIRO"
    fetch https://cairographics.org/releases/$CAIRO.tar.xz $CAIRO.tar.xz
    unpack $CAIRO.tar.xz $CAIRO
    meson setup "$BUILD/$CAIRO" "$ROOT/$CAIRO" "${MESON_COMMON[@]}" \
        -Dfreetype=enabled -Dpng=enabled -Dzlib=enabled \
        -Dfontconfig=disabled -Ddwrite=disabled -Dquartz=disabled \
        -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled \
        -Dtee=disabled -Dglib=disabled -Dspectre=disabled -Dlzo=disabled \
        -Dsymbol-lookup=disabled -Dtests=disabled -Dgtk2-utils=disabled \
        -Dgtk_doc=false
    meson compile -C "$BUILD/$CAIRO" -j "$JOBS"
    meson install -C "$BUILD/$CAIRO"
    mark cairo
fi

#---------------------------------------------------------------------------
# The ASIO SDK — the Windows audio backend's reason for existing.
#
# NOT BUILT, INSTALLED. There is no library to compile here: the SDK is a handful
# of sources that a host compiles into itself, so this step's whole job is to put
# them where the build can find them and nowhere else. CMakeLists.txt looks for
# $SYSROOT/include/asiosdk and compiles the four host-side sources out of it.
#
# WHY IT LIVES IN THE SYSROOT AT ALL. Steinberg's agreement forbids
# redistributing the SDK "or parts of it ... for example, integrated in any
# framework application", so it must never be committed to the repository. This
# prefix is untracked build output, so putting it here breaches nothing — and it
# is the only route into the build, which is what makes that claim checkable.
# .gitignore guards the SDK's filenames and the project's phase gate sweeps the
# working tree for them, because a licence breach is not undone by a later
# deletion.
#
# WHERE IT COMES FROM, in order: an explicit $NAMPRACK_ASIO_SDK_DIR, then an
# already-unpacked tree beside the other reference checkouts, then a zip already
# in the download cache, and only then the network. Preferring a local copy is
# not just politeness to the network: the download is licence-gated on
# Steinberg's own page, and a machine that already has it should never be asked
# to take it again.
#
# WHAT IS COPIED, and what is left behind: the host side and the interface
# definitions only. The sample DRIVER, the COM registration code, the debug
# helpers and the MSVC project files are all for writing a driver rather than
# hosting one, and none of them is on this product's include path.
#
# NAMPRACK_SKIP_ASIO=1 SKIPS THIS STEP, and the rest of the sysroot still builds.
# The download is licence-gated on Steinberg's page, so requiring it here would
# mean nobody can compile a line of an MIT-licensed project until they have
# accepted a third party's terms — and they are not publishing anything by
# building. The result is a sysroot that produces a WASAPI-only standalone;
# CMakeLists.txt notices the absence, says so, and demotes itself for exactly the
# same reason. Waived loudly on stderr, in the same shape as this project's other
# named opt-outs, so a log always shows which gate was skipped rather than
# leaving a skipped step to look like a passed one.
#---------------------------------------------------------------------------
if [ "${NAMPRACK_SKIP_ASIO:-0}" = "1" ]; then
    say "ASIO SDK"
    echo "warning: NAMPRACK_SKIP_ASIO=1 - the ASIO SDK was NOT installed." >&2
    echo "warning: this sysroot builds a WASAPI-only standalone, which is not the" >&2
    echo "warning: shipped product: ASIO is the primary Windows backend and the one" >&2
    echo "warning: the latency depends on. Re-run without NAMPRACK_SKIP_ASIO to fetch it." >&2
elif ! done_stamp asiosdk; then
    say "ASIO SDK"

    ASIO_SRC=""
    if [ -n "${NAMPRACK_ASIO_SDK_DIR:-}" ]; then
        ASIO_SRC="$NAMPRACK_ASIO_SDK_DIR"
        [ -d "$ASIO_SRC" ] || die "NAMPRACK_ASIO_SDK_DIR=$ASIO_SRC is not a directory"
        echo "using $ASIO_SRC (NAMPRACK_ASIO_SDK_DIR)"
    elif [ -d "$HOME/third_party/$ASIO_DIR" ]; then
        ASIO_SRC="$HOME/third_party/$ASIO_DIR"
        echo "using $ASIO_SRC (already unpacked)"
    else
        if [ ! -f "$DL/$ASIO_ZIP" ]; then
            echo "fetching $ASIO_ZIP"
            command -v curl >/dev/null || die "curl not found, and no local ASIO SDK to copy"
            curl -sSL -o "$DL/$ASIO_ZIP.part" "$ASIO_URL" ||
                die "could not download the ASIO SDK from $ASIO_URL"
            mv "$DL/$ASIO_ZIP.part" "$DL/$ASIO_ZIP"
        fi
        # The hash is checked on the zip whether it was just downloaded or was
        # already in the cache, so a stale or tampered archive cannot slip in.
        GOT_SHA="$(sha256sum "$DL/$ASIO_ZIP" | cut -d' ' -f1)"
        if [ "$GOT_SHA" != "$ASIO_SHA256" ]; then
            echo "  expected $ASIO_SHA256" >&2
            echo "  got      $GOT_SHA" >&2
            die "the ASIO SDK archive is not the pinned one. Upstream has re-rolled it, or the
     download is damaged. Check what changed, diff the tree, and update ASIO_SHA256
     deliberately — do not raise it to make the build pass."
        fi
        command -v unzip >/dev/null || die "unzip not found (needed for the ASIO SDK)"
        [ -d "$ROOT/$ASIO_DIR" ] || unzip -q "$DL/$ASIO_ZIP" -d "$ROOT"
        ASIO_SRC="$ROOT/$ASIO_DIR"
    fi

    # Verify what arrived rather than trusting the name. Each of these is a file
    # the backend actually includes or compiles; a release that moves or renames
    # one has to fail here, loudly, and not at link time three steps later.
    ASIO_NEED="common/asio.h common/asio.cpp common/asiosys.h common/iasiodrv.h
               host/asiodrivers.h host/asiodrivers.cpp host/ginclude.h
               host/pc/asiolist.h host/pc/asiolist.cpp"
    for f in $ASIO_NEED; do
        [ -f "$ASIO_SRC/$f" ] || die "$ASIO_SRC is missing $f - is this an ASIO SDK 2.3 tree?"
    done
    # The entry points the backend calls, checked in the header rather than assumed
    # from the version number. ASIOGetLatencies is the 2.0 one; the drop-out
    # detection query arrived in 2.3 and dropouts() is written against it.
    for sym in ASIOInit ASIOCreateBuffers ASIOStart ASIOGetBufferSize ASIOGetLatencies \
               ASIOOutputReady kAsioResetRequest kAsioResyncRequest; do
        grep -q "$sym" "$ASIO_SRC/common/asio.h" ||
            die "$ASIO_SRC/common/asio.h does not declare $sym"
    done

    ASIO_INC="$SYSROOT/include/asiosdk"
    rm -rf "$ASIO_INC"
    mkdir -p "$ASIO_INC/common" "$ASIO_INC/host/pc"
    install -m644 "$ASIO_SRC"/common/asio.h "$ASIO_SRC"/common/asio.cpp \
                  "$ASIO_SRC"/common/asiosys.h "$ASIO_SRC"/common/iasiodrv.h "$ASIO_INC/common/"
    install -m644 "$ASIO_SRC"/host/asiodrivers.h "$ASIO_SRC"/host/asiodrivers.cpp \
                  "$ASIO_SRC"/host/ginclude.h "$ASIO_INC/host/"
    install -m644 "$ASIO_SRC"/host/pc/asiolist.h "$ASIO_SRC"/host/pc/asiolist.cpp \
                  "$ASIO_INC/host/pc/"
    # The licence travels with the copy, in both of its forms: the top-level dual
    # licence and common/'s own. Neither is redistributed by this project, but the
    # sysroot is a place a person looks, and a copy of someone's SDK with no
    # licence beside it is how a licence gets forgotten.
    install -m644 "$ASIO_SRC/LICENSE.txt" "$ASIO_INC/LICENSE.txt"
    install -m644 "$ASIO_SRC/common/LICENSE.txt" "$ASIO_INC/common/LICENSE.txt"
    mark asiosdk
fi

#---------------------------------------------------------------------------
say "sysroot summary"
echo "prefix: $SYSROOT"
MISSING=0
for m in zlib libpng16 pixman-1 freetype2 cairo cairo-ft; do
    v="$(pkg-config --modversion "$m" 2>/dev/null || echo 'MISSING')"
    printf '  %-12s %s\n' "$m" "$v"
    [ "$v" = "MISSING" ] && MISSING=1
done
echo
echo "static libraries:"
ls -1 "$SYSROOT/lib"/*.a 2>/dev/null | sed 's|^|  |'
echo
if [ -f "$SYSROOT/include/asiosdk/common/asio.h" ]; then
    printf '  %-12s %s\n' "asiosdk" "include/asiosdk (host side only, not redistributed)"
elif [ "${NAMPRACK_SKIP_ASIO:-0}" = "1" ]; then
    # Absent because it was waived, which is a complete sysroot for a WASAPI-only
    # build rather than a broken one. Still printed, and still says what it costs:
    # the whole point of a named opt-out is that the log shows it was taken.
    printf '  %-12s %s\n' "asiosdk" "skipped (NAMPRACK_SKIP_ASIO=1) - WASAPI-only standalone"
else
    printf '  %-12s %s\n' "asiosdk" "MISSING"
    MISSING=1
fi
if [ "$MISSING" = "1" ]; then
    echo
    die "the sysroot is incomplete - a module above did not install a .pc file"
fi
