#!/usr/bin/env bash
# Assemble the Complete Corresponding Source for a GPLv3 build of NAMp Rack.
#
# WHY THIS EXISTS. The Windows build of the rack compiles part of the ASIO SDK in, the SDK is
# dual-licensed, and this project takes its GPLv3 arm rather than the proprietary one. GPLv3
# section 6 then obliges the Corresponding Source for that binary to be available to whoever
# received it, and section 1 defines Corresponding Source as "all the source code needed to
# generate, install, and ... run the object code", including "the scripts used to control those
# activities". That is an obligation on the RELEASE, so it needs a mechanism that runs at release
# time and fails loudly, not a paragraph in a licence file promising one exists.
#
# WHAT MAKES THIS COMPLETE RATHER THAN APPROXIMATE. Every input to the Windows build is pinned:
# the repository is at a commit, its four dependencies are submodules at fixed SHAs, and the five
# statically-linked graphics libraries plus the ASIO SDK are release tarballs fetched by
# scripts/build-win-deps.sh. So the set can be COLLECTED rather than described, and every member
# hashed as it goes in. A reader can rebuild the binary; they are not asked to take anyone's word
# for which cairo it was.
#
# THE ASIO SDK IS PART OF THIS SET, AND THAT IS THE WHOLE POINT OF THE GPLv3 ARM. Under the
# proprietary arm the SDK may not be redistributed at all, which is why the repository has never
# contained it and still does not. Under the GPLv3 arm Steinberg licenses it on GPLv3 terms, which
# grant the right to convey the source -- and section 6 then makes conveying it a DUTY for anyone
# who ships the binary. The SDK still never enters the repository; it enters this tarball, which is
# build output, at release time, from the same untracked dependency sysroot the compiler read.
#
# Only the nine files the build actually compiles are collected, plus the SDK's own two licence
# files. That is not a trimming exercise -- Corresponding Source is the source of the work being
# conveyed, and the driver examples and specification PDFs are not in the work.
#
# Usage:
#   scripts/corresponding-source.sh [outdir]     assemble the tarball (default: products/rack/dist)
#   scripts/corresponding-source.sh --directions the text that ships beside the binary
#
# Environment:
#   NAMP_WIN_SYSROOT / NAMP_WIN_DEPS_ROOT   where build-win-deps.sh put the dependency sources.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_ROOT="${NAMP_WIN_DEPS_ROOT:-${NAMPRACK_WIN_DEPS_ROOT:-${RATIONS_WIN_DEPS_ROOT:-$HOME/third_party/win-deps}}}"
SYSROOT="${NAMP_WIN_SYSROOT:-${NAMPRACK_WIN_SYSROOT:-$DEPS_ROOT/sysroot}}"
DL="$DEPS_ROOT/dl"
ASIO_INC="$SYSROOT/include/asiosdk"

. "$REPO/scripts/dist-common.sh"

die() { namp_dist_die "$*"; }

# The commit is the identity of the source half, so a dirty tree cannot produce a release set: the
# tarball would claim a commit whose content is not what was built.
COMMIT="$(git -C "$REPO" rev-parse HEAD)"
DIRTY=""
git -C "$REPO" diff --quiet HEAD -- 2>/dev/null || DIRTY="yes"
VERSION="$(namp_dist_version "$REPO/products/rack/CMakeLists.txt")"

# The five graphics tarballs, by the names scripts/build-win-deps.sh fetches. Read from that script
# rather than restated here, so a version bump there cannot leave this list behind describing a
# release that no longer exists.
read_pin() { sed -n "s/^$1=\\(.*\\)$/\\1/p" "$REPO/scripts/build-win-deps.sh" | head -1; }
ZLIB="$(read_pin ZLIB)";       LIBPNG="$(read_pin LIBPNG)"
PIXMAN="$(read_pin PIXMAN)";   FREETYPE="$(read_pin FREETYPE)"
CAIRO="$(read_pin CAIRO)";     ASIO_ZIP="$(read_pin ASIO_ZIP)"
for _v in ZLIB LIBPNG PIXMAN FREETYPE CAIRO; do
  [ -n "${!_v}" ] || die "could not read the $_v pin out of scripts/build-win-deps.sh"
done
TARBALLS="$ZLIB.tar.xz $LIBPNG.tar.xz $PIXMAN.tar.gz $FREETYPE.tar.xz $CAIRO.tar.xz"

# The nine ASIO files the Windows backend compiles, and the two licence files that govern them.
# Kept identical to the install list in scripts/build-win-deps.sh: two lists that disagree about
# what was compiled in would make this tarball a claim rather than a copy.
ASIO_FILES="common/asio.h common/asio.cpp common/asiosys.h common/iasiodrv.h
            common/LICENSE.txt
            host/asiodrivers.h host/asiodrivers.cpp host/ginclude.h
            host/pc/asiolist.h host/pc/asiolist.cpp
            LICENSE.txt"

# --- --directions: the text that travels with the binary ----------------------------------------
# GPLv3 section 6(d) lets the Corresponding Source sit somewhere other than beside the object code
# "provided you maintain clear directions next to the object code saying where to find" it. This is
# those directions, and it is generated from the same variables that assemble the tarball so the
# two cannot drift.
if [ "${1:-}" = "--directions" ]; then
  cat <<DIRECTIONS
Corresponding Source for NAMp Rack ${VERSION} (Windows, with ASIO)
=================================================================

This binary is distributed under the GNU General Public License version 3 --
see LICENSE-windows-asio.txt for why, and COPYING.GPL-3 for the terms. GPLv3
section 6 entitles you to its Complete Corresponding Source. This file is the
"clear directions" section 6(d) asks for.

The source set is published as:

    namp-rack-${VERSION}-corresponding-source.tar.gz

on the releases page this binary came from:

    https://github.com/rations/NAMp-rations/releases

It is uploaded to the same release as this archive, so it is one page away from
wherever you got this file. If it is not there, that is a bug in the release and
not a limit on your rights -- ask for it.

It contains, and its MANIFEST.txt records the SHA-256 of, every one of:

  * this project's own source, at commit
        ${COMMIT}
    including every script that drives the build;
  * its five pinned dependencies, at the exact commits the build used --
    the VST3 SDK, NeuralAmpModelerCore, AudioDSPTools, Eigen, and the source of
    the five pedals that ship beside this program;
  * the five libraries this binary links statically --
        ${ZLIB}, ${LIBPNG}, ${PIXMAN},
        ${FREETYPE}, ${CAIRO}
    as their upstream release archives, unmodified;
  * the ASIO SDK files compiled into this binary, under the GPLv3 arm of that
    SDK's own dual licence.

REBUILDING. The set is self-contained: there is no step that fetches anything.
README-BUILD.txt inside the tarball has the commands. The build is
bit-reproducible, so a correct rebuild produces a byte-identical binary, and you
can check that rather than trust it.
DIRECTIONS
  exit 0
fi

# --- assemble -----------------------------------------------------------------------------------
OUTDIR="${1:-$REPO/products/rack/dist}"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"

[ -z "$DIRTY" ] || die "the working tree has uncommitted changes. The tarball would name commit
${COMMIT}, whose content is not what is here. Commit or stash first."

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
NAME="namp-rack-${VERSION}-corresponding-source"
ROOT="$STAGE/$NAME"
mkdir -p "$ROOT"

echo "== this project, at $COMMIT =="
mkdir -p "$ROOT/namp-rack"
git -C "$REPO" archive --format=tar HEAD | tar -x -C "$ROOT/namp-rack"

# Submodules are gitlinks: the superproject's archive records their commit and none of their
# content, so each is archived from its own repository at the SHA the superproject pins. Recorded
# by SHA rather than by tag, because the SHA is what the build used.
echo "== the five pinned dependencies =="
# NOT --recursive, and that is measured rather than cautious. TWO nested submodules exist and
# neither is used: AudioDSPTools declares its own eigen, and rations-pedals declares its own
# vst3sdk. Both products compile against the eigen and the SDK at the REPOSITORY ROOT -- the
# pedals' CMakeLists guards its SDK block with `if(NOT TARGET sdk)` precisely so the parent's is
# taken -- and nothing in any CMakeLists names either nested path. Asking recursively would fail
# on a tree that builds perfectly well, and would drag in a second 1.5 GB copy of the SDK if it
# did not. The five the build actually consumes are the five at the root, and the count is
# asserted so a sixth cannot appear unnoticed and be left out of a release.
SUBS="$(git -C "$REPO" submodule status)"
[ -n "$SUBS" ] || die "no submodules are checked out; see README.md for the init sequence"
_nsub="$(printf '%s\n' "$SUBS" | grep -c .)"
[ "$_nsub" = "5" ] || die "expected 5 root submodules, found $_nsub. The Corresponding Source must
carry every pinned dependency the build compiles; check what changed before releasing."
while read -r _line; do
  [ -n "$_line" ] || continue
  # The status prefix is ONE leading character (+ = different SHA checked out, - = not
  # initialised, U = merge conflict), stripped with sed rather than `tr -d '+-U'`: in tr that
  # spelling is the RANGE + through U, which eats most of a hex SHA and was measured doing it.
  _sha="$(printf '%s' "$_line" | awk '{print $1}' | sed 's/^[-+U]//')"
  _path="$(printf '%s' "$_line" | awk '{print $2}')"
  [ -e "$REPO/$_path/.git" ] ||
    die "submodule $_path is not checked out; see README.md for the init sequence"
  mkdir -p "$ROOT/namp-rack/$_path"
  git -C "$REPO/$_path" archive --format=tar "$_sha" | tar -x -C "$ROOT/namp-rack/$_path" ||
    die "could not archive submodule $_path at $_sha"
  printf '  %-24s %s\n' "$_path" "$_sha"
done <<< "$SUBS"

echo "== the five statically linked libraries =="
mkdir -p "$ROOT/win-deps"
for _t in $TARBALLS; do
  [ -f "$DL/$_t" ] || die "$DL/$_t is missing. Run scripts/build-win-deps.sh first -- the
Corresponding Source is the archive the build actually used, not one fetched again now."
  cp "$DL/$_t" "$ROOT/win-deps/"
  printf '  %s\n' "$_t"
done

echo "== the ASIO SDK, GPLv3 arm =="
if [ -d "$ASIO_INC" ]; then
  for _f in $ASIO_FILES; do
    [ -f "$ASIO_INC/$_f" ] || die "$ASIO_INC/$_f is missing; the sysroot is incomplete."
    mkdir -p "$ROOT/asiosdk/$(dirname "$_f")"
    cp "$ASIO_INC/$_f" "$ROOT/asiosdk/$_f"
  done
  cp "$REPO/products/rack/COPYING.GPL-3" "$ROOT/asiosdk/COPYING.GPL-3"
  echo "  nine sources and two licence files"
  ASIO_STATE="included (GPLv3 arm of the SDK's dual licence)"
else
  echo "  NOT PRESENT -- no ASIO in the sysroot" >&2
  echo "  This set corresponds to a WASAPI-only build, which contains no ASIO code" >&2
  echo "  and is MIT rather than GPLv3." >&2
  ASIO_STATE="absent (WASAPI-only build; that binary is MIT, not GPLv3)"
fi

cp "$REPO/products/rack/COPYING.GPL-3" "$ROOT/COPYING.GPL-3"
cp "$REPO/products/rack/LICENSE-windows-asio.txt" "$ROOT/"
cp "$REPO/LICENSE" "$ROOT/LICENSE.MIT"

cat > "$ROOT/README-BUILD.txt" <<BUILDEOF
Rebuilding NAMp Rack ${VERSION} for Windows from this source set
================================================================

This set is self-contained. Nothing here fetches anything from the network.

  namp-rack/   this project at commit ${COMMIT},
               with its five pinned dependencies already in place
  win-deps/    the five libraries the Windows binary links statically,
               as their unmodified upstream release archives
  asiosdk/     the ASIO SDK files compiled into the Windows binary
  MANIFEST.txt SHA-256 of every file above

You need a Linux host with the MinGW-w64 cross toolchain, CMake, Ninja and
Meson. The build is a cross build; no Windows machine is involved.

  1. Build the dependency sysroot. The script normally downloads these; point
     it at the copies here instead:

       cd namp-rack
       NAMP_WIN_DEPS_ROOT=/somewhere/writable \\
       NAMP_ASIO_SDK_DIR="\$PWD/../asiosdk" \\
         scripts/build-win-deps.sh

     Put the win-deps/ archives in \$NAMP_WIN_DEPS_ROOT/dl/ first and the
     script will use them rather than fetching.

  2. Build and package:

       products/rack/scripts/makedist-windows.sh

REPRODUCIBILITY. This build is bit-reproducible: the PE TimeDateStamp is zeroed
by the link, no source path reaches the binary, and the same source produces a
byte-identical namp-rack.exe from a different directory on a different day. So
you can verify that this source set is the one that produced the binary you
received, by building it and comparing the bytes. That check is available to you
and is the reason the pins above are exact.
BUILDEOF

echo "== hashing =="
( cd "$ROOT" && find . -type f ! -name MANIFEST.txt -print0 | sort -z |
  xargs -0 sha256sum > MANIFEST.txt )
{
  echo "Complete Corresponding Source -- NAMp Rack ${VERSION} (Windows, with ASIO)"
  echo "GNU General Public License version 3; see COPYING.GPL-3 and LICENSE-windows-asio.txt."
  echo
  echo "project commit : ${COMMIT}"
  echo "ASIO SDK       : ${ASIO_STATE}"
  echo "assembled by   : scripts/corresponding-source.sh"
  echo
  echo "SHA-256 of every file in this tarball:"
  echo
} > "$ROOT/MANIFEST.head"
cat "$ROOT/MANIFEST.head" "$ROOT/MANIFEST.txt" > "$ROOT/MANIFEST.new"
mv "$ROOT/MANIFEST.new" "$ROOT/MANIFEST.txt"
rm -f "$ROOT/MANIFEST.head"

TARBALL="$OUTDIR/${NAME}.tar.gz"
rm -f "$TARBALL"
# Deterministic: fixed owner, fixed mtime from the commit, sorted names. Two runs at the same
# commit produce the same bytes, which is what lets a recipient check this set as well as read it.
COMMIT_EPOCH="$(git -C "$REPO" show -s --format=%ct HEAD)"
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$COMMIT_EPOCH" -C "$STAGE" -czf "$TARBALL" "$NAME"

echo
echo "Corresponding Source: $TARBALL"
echo "  $(du -h "$TARBALL" | cut -f1),  $(tar -tzf "$TARBALL" | wc -l) entries"
