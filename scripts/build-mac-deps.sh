#!/usr/bin/env bash
# Build the editor's graphics dependencies for macOS, statically, from source.
#
# WHY THIS EXISTS, and why Homebrew is not the answer. On Linux these five are
# system shared libraries and CMakeLists.txt just asks pkg-config for them. On
# macOS Homebrew has all five -- and ships them as dylibs under /opt/homebrew
# (or /usr/local on Intel), which is a path that exists on the build machine and
# on nobody else's. A bundle linked against those loads for the person who built
# it and fails for every user, with a dyld error naming a directory they have
# never heard of. So the same answer the Windows build reached, for the same
# reason: build them here, link them in, and let makedist-mac.sh assert with
# otool that the finished plug-in names nothing but system libraries.
#
# THIS IS A NATIVE BUILD, NOT A CROSS BUILD, which is the one structural
# difference from scripts/build-win-deps.sh. There is no meson cross file and no
# exe_wrapper: each macOS runner builds the slice its own CPU is, and the two
# slices are joined afterwards by lipo (see scripts/makedist-mac.sh). That is
# also why $SYSROOT carries the architecture in its path -- an arm64 and an
# x86_64 sysroot must not overwrite each other, and a mixed one would link and
# then fail at load with a "building for macOS-arm64 but attempting to link
# x86_64" that names the library rather than the mistake.
#
# VERSIONS MATCH THE LINUX BUILD, and that is a test requirement rather than
# tidiness -- the same requirement build-win-deps.sh states. CI renders all four
# editor pages with the macOS panelrender and compares them against the Linux
# render, and a different FreeType would move glyph rasterisation enough to make
# that comparison meaningless. Pinned against what the Linux machine's Debian
# actually ships:
#
#   zlib     1.3.1     (zlib1g-dev  1:1.3.dfsg+really1.3.1-1)
#   libpng   1.6.48    (libpng-dev  1.6.48-1)
#   pixman   0.44.0    (libpixman-1-dev 0.44.0-3)
#   freetype 2.13.3    (libfreetype-dev 2.13.3+dfsg-1)
#   cairo    1.18.4    (libcairo2-dev 1.18.4-1)
#
# WHAT IS DELIBERATELY LEFT OUT is the same list build-win-deps.sh gives, plus
# one entry of its own:
#   * fontconfig - Rations never calls it; there is not one Fc* symbol in the
#     tree, and FontStack loads its two TTFs by path.
#   * quartz     - cairo's Quartz BACKEND, which this does not want and must
#     not accidentally acquire. The mac editor composes into the same offscreen
#     image surface the other two platforms do and blits it once through
#     CoreGraphics, exactly as the Win32 view blits a DIB. Enabling the quartz
#     backend would add a second, differently-rounding rasteriser to the build
#     for a path nothing calls, and the panel diff against Linux is precisely a
#     test that the SAME rasteriser is in use.
#   * xlib / xcb, harfbuzz, brotli, bzip2, freetype's png, glib, lzo, spectre,
#     dwrite, tests, docs.
#
# NO DLL_EXPORT PATCH. build-win-deps.sh has to patch FreeType's meson.build
# because a static libfreetype.a there carries .drectve -export: directives that
# re-export 151 symbols out of the finished bundle. That is a PE mechanism with
# no Mach-O equivalent, and in any case the mac link uses the SDK's
# macexport.exp as an explicit -exported_symbols_list allow-list, so nothing can
# leak whatever the archives say. Left unpatched on purpose.
set -euo pipefail

ARCH="${RATIONS_MAC_ARCH:-$(uname -m)}"
DEPLOY="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
ROOT="${RATIONS_MAC_DEPS_ROOT:-$HOME/third_party/mac-deps}"
SYSROOT="${RATIONS_MAC_SYSROOT:-$ROOT/$ARCH/sysroot}"
DL="$ROOT/dl"
BUILD="$ROOT/$ARCH/build"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

ZLIB=zlib-1.3.1
LIBPNG=libpng-1.6.48
PIXMAN=pixman-0.44.0
FREETYPE=freetype-2.13.3
CAIRO=cairo-1.18.4

# Sources are unpacked per architecture, not shared. They could be shared -- the
# tarballs are identical -- but a meson or cmake build tree left inside a source
# directory by one architecture is picked up by the other, and the failure that
# produces names a compiler flag rather than the reason.
SRC="$ROOT/$ARCH/src"
mkdir -p "$DL" "$BUILD" "$SRC" "$SYSROOT/lib/pkgconfig" "$SYSROOT/include"

case "$ARCH" in
    arm64|x86_64) ;;
    *) echo "error: unsupported macOS architecture '$ARCH'" >&2; exit 1 ;;
esac

[ "$(uname -s)" = "Darwin" ] || { echo "error: this script builds on macOS only" >&2; exit 1; }
command -v meson >/dev/null || { echo "error: meson not found (brew install meson)" >&2; exit 1; }
command -v ninja >/dev/null || { echo "error: ninja not found (brew install ninja)" >&2; exit 1; }
command -v pkg-config >/dev/null || { echo "error: pkg-config not found (brew install pkg-config)" >&2; exit 1; }

# pkg-config must see the sysroot and nothing of the host's. On macOS that means
# nothing of Homebrew's, which is the whole point: if a dependency resolves to
# /opt/homebrew here, the bundle it produces is broken on every other machine
# and nothing until otool at packaging time would say so.
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
export PKG_CONFIG_PATH=""

# One architecture, one deployment target, applied to every module the same way.
export MACOSX_DEPLOYMENT_TARGET="$DEPLOY"
export CFLAGS="-arch $ARCH -mmacosx-version-min=$DEPLOY -O2 ${CFLAGS:-}"
export CXXFLAGS="-arch $ARCH -mmacosx-version-min=$DEPLOY -O2 ${CXXFLAGS:-}"
export LDFLAGS="-arch $ARCH -mmacosx-version-min=$DEPLOY ${LDFLAGS:-}"

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
    [ -d "$SRC/$2" ] && return 0
    tar -xf "$DL/$1" -C "$SRC"
}

MESON_COMMON=(
    --prefix "$SYSROOT"
    --libdir lib
    --buildtype release
    --default-library static
)

#---------------------------------------------------------------------------
# zlib. macOS ships one in the SDK, and it is deliberately not used: it is a
# dylib, its version tracks the OS rather than this pin, and libpng would then
# link a system zlib while cairo linked ours. Built through zlib's own
# configure, which is the native path upstream documents and which writes a
# correct zlib.pc -- the Windows script has to hand-write that file only because
# win32/Makefile.gcc has no install target.
#---------------------------------------------------------------------------
if ! done_stamp zlib; then
    say "zlib $ZLIB"
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/$ZLIB.tar.xz $ZLIB.tar.xz
    unpack $ZLIB.tar.xz $ZLIB
    ( cd "$SRC/$ZLIB" && ./configure --prefix="$SYSROOT" --static && make -j"$JOBS" && make install )
    # configure --static still leaves a dylib behind on some zlib releases.
    # Remove it: a stray libz.dylib in the sysroot is exactly the thing the
    # linker would prefer, silently undoing the static build.
    rm -f "$SYSROOT/lib"/libz*.dylib "$SYSROOT/lib"/libz.*.dylib
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
    cmake -S "$SRC/$LIBPNG" -B "$BUILD/$LIBPNG" -G Ninja \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
        -DCMAKE_PREFIX_PATH="$SYSROOT" \
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
    meson setup "$BUILD/$PIXMAN" "$SRC/$PIXMAN" "${MESON_COMMON[@]}" \
        -Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled \
        -Dopenmp=disabled
    meson compile -C "$BUILD/$PIXMAN" -j "$JOBS"
    meson install -C "$BUILD/$PIXMAN"
    mark pixman
fi

#---------------------------------------------------------------------------
# FreeType. harfbuzz off to match the Linux system build (verified there with
# ldd on libfreetype.so.6, which links zlib/bz2/png/brotli but NOT harfbuzz), so
# glyph rasterisation stays identical to the Linux render and the panel diff
# measures the platform rather than the font stack.
#---------------------------------------------------------------------------
if ! done_stamp freetype; then
    say "freetype $FREETYPE"
    fetch https://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/$FREETYPE.tar.xz $FREETYPE.tar.xz
    unpack $FREETYPE.tar.xz $FREETYPE
    meson setup "$BUILD/$FREETYPE" "$SRC/$FREETYPE" "${MESON_COMMON[@]}" \
        -Dharfbuzz=disabled -Dbrotli=disabled -Dbzip2=disabled \
        -Dpng=disabled -Dzlib=disabled -Dtests=disabled
    meson compile -C "$BUILD/$FREETYPE" -j "$JOBS"
    meson install -C "$BUILD/$FREETYPE"
    mark freetype
fi

#---------------------------------------------------------------------------
# cairo. freetype must be requested EXPLICITLY, and this is the case
# build-win-deps.sh's comment predicted in as many words. cairo's meson.build
# does
#     freetype_required = host_machine.system() not in ['windows', 'darwin']
#     freetype_option = freetype_option.disable_auto_if(not freetype_required)
# and 'darwin' is in that list beside 'windows' -- so on macOS the default
# 'auto' resolves to DISABLED, and a cairo without cairo-ft would fail to build
# gfx/fontstack.cpp.
#
# quartz stays disabled; see the header. The Windows script disables it too, and
# there it is a formality -- here it is a decision.
#---------------------------------------------------------------------------
if ! done_stamp cairo; then
    say "cairo $CAIRO"
    fetch https://cairographics.org/releases/$CAIRO.tar.xz $CAIRO.tar.xz
    unpack $CAIRO.tar.xz $CAIRO
    meson setup "$BUILD/$CAIRO" "$SRC/$CAIRO" "${MESON_COMMON[@]}" \
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
say "sysroot summary"
echo "arch:   $ARCH"
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

# A dylib in this prefix is not a warning, it is the failure this script exists
# to prevent: the linker prefers it over the .a beside it, and the result gets
# all the way to a user's machine before anything notices.
STRAY="$(ls -1 "$SYSROOT/lib"/*.dylib 2>/dev/null || true)"
if [ -n "$STRAY" ]; then
    echo
    echo "$STRAY" | sed 's|^|  stray: |'
    die "the sysroot contains shared libraries; the plug-in would link those instead of the archives"
fi

if [ "$MISSING" = "1" ]; then
    echo
    die "the sysroot is incomplete - a module above did not install a .pc file"
fi
