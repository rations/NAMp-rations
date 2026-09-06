#!/usr/bin/env bash
# Syntax-check the macOS window class WITHOUT a Mac.
#
# WHY THIS EXISTS. The rest of this project is built on one Linux box, Windows
# included, because MinGW cross-compiles it and Wine runs every gate on it.
# macOS has neither half: the only mac toolchain this project has is a GitHub
# Actions runner, so a single misspelled selector in src/platform/macplugview.mm
# costs a push, a runner start and several minutes. That file is ~1000 lines of
# Objective-C++ written without a compiler in the room, which is exactly the
# situation this script is for.
#
# clang builds Objective-C++ on Linux; what it lacks is AppKit. tools/macstub/
# supplies the small part of AppKit and CoreGraphics that macplugview.mm
# actually touches, and this script compiles the file against it.
#
# WHAT IT PROVES, AND WHAT IT DOES NOT. It proves the file parses, that its C++
# is well-typed, that the Objective-C class is structurally sound, that every
# member it names exists, and — through the seam probe — that RationsEditorView's
# hook set still fits MacPlugView with the same signatures the other two
# platforms use. It does NOT and CANNOT validate an AppKit fact: the stub's
# signatures were written from the same reference trees macplugview.mm was, so
# checking one against the other would be circular. Every API fact in that file
# is grounded in a named reference at its site instead, and the real gate is the
# CI build.
#
# It is deliberately NOT wired into the CMake build or any phase gate. It is a
# convenience for iterating on one file, and a gate that could pass while the
# real compiler fails would be worse than no gate at all.
#
# Usage: scripts/mac-syntax-check.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLANG="${RATIONS_CLANG:-}"
if [ -z "$CLANG" ]; then
  for c in clang++ clang++-19 clang++-18 clang++-17; do
    if command -v "$c" >/dev/null 2>&1; then CLANG="$c"; break; fi
  done
fi
if [ -z "$CLANG" ]; then
  echo "no clang++ found; set \$RATIONS_CLANG to one" >&2
  exit 1
fi

# The GNU Objective-C runtime headers, which clang needs for @interface even
# though nothing here is ever linked. Provided by libobjc-<n>-dev on Debian.
# Chosen by testing for the header rather than by taking the last match: a
# machine set up to cross-build for Windows also has an objc directory under the
# MinGW toolchain, which sorts later and contains no objc.h at all.
OBJC_INC=""
while read -r d; do
  case "$d" in *mingw*) continue;; esac
  if [ -f "$d/objc.h" ]; then OBJC_INC="$(dirname "$d")"; break; fi
done < <(find /usr/lib/gcc -type d -name objc 2>/dev/null | sort)
if [ -z "$OBJC_INC" ]; then
  echo "no <objc/objc.h> found; install libobjc-14-dev (or set the include path)" >&2
  exit 1
fi

CAIRO_INC="$(pkg-config --cflags cairo)"

# RELEASE and NDEBUG because base/source/fdebug.h #errors without one of them,
# and -Wno-pragma-pack because the SDK's own falignpush.h/falignpop.h pair trips
# it on every include. Neither says anything about our code.
FLAGS=(-x objective-c++ -fsyntax-only -std=c++17 -Wall -Wno-pragma-pack
       -DRELEASE=1 -DNDEBUG
       -I"$ROOT/tools/macstub" -I"$ROOT/src" -I"$ROOT/vst3sdk"
       -I"$OBJC_INC")
# shellcheck disable=SC2206
FLAGS+=($CAIRO_INC)

fail=0
echo "== src/platform/macplugview.mm"
"$CLANG" "${FLAGS[@]}" src/platform/macplugview.mm || fail=1
echo "== tools/macstub/seamcheck.mm (the editor's hook set against MacPlugView)"
"$CLANG" "${FLAGS[@]}" tools/macstub/seamcheck.mm || fail=1

if [ "$fail" -ne 0 ]; then
  echo "FAILED" >&2
  exit 1
fi
echo "PASSED — both parse and type-check against the AppKit stub"
