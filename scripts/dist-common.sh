#!/usr/bin/env bash
# The Linux release machinery both products share, plus the ABI baseline gate.
#
# HOW THIS IS SPLIT, AND WHY IT IS NOT ONE SCRIPT. Everything here is either generic over an ELF
# (what it links, what it exports, what glibc it needs) or generic over a tarball. Nothing here
# knows what NAMp-rations.vst3 or namp-rack is, and that is the line:
#
#   this file          the mechanism -- one implementation, so a fix to it lands once
#   products/*/scripts/makedist-linux.sh
#                      WHICH assertions each artefact gets, spelled out one call per line
#
# The per-artefact assertions are deliberately NOT collapsed into a flag on a single packaging
# function. "The plug-in must not link JACK", "the standalone must", "the LV2 must be
# self-contained" and "the host must export nothing at all" are four different questions about four
# different files, and a flags table is how one of them quietly stops being asked -- the answer
# still says PASS because the flag that selected it was never set. Two callers, each naming its own
# checks, is the shape that makes a dropped check visible in a diff.
#
# Source it; do not run it, except for --probe-baseline below.

# --- the declared ABI baseline ----------------------------------------------
# A release is built on an OLDER distribution than the one it is developed on, so that users on
# older distributions can run it: glibc's symbol versioning is forward-compatible and not backward,
# so a binary linked against a new glibc names symbol versions an old loader simply does not have,
# and the failure is at load time with a message about a version node rather than anything a user
# can act on.
#
# THESE THREE NUMBERS ARE THE PACKAGING MACHINE'S, NOT THIS ONE'S, and they are PROVISIONAL. They
# describe the release distribution (Debian 12 "bookworm" / Devuan 5 "Daedalus"), and there is no
# copy of that distribution on the development machine to read them off -- no sysroot, no chroot,
# no archive of it. They are therefore written down as the intended ceiling rather than as a
# measured one, and confirming them is one command ON THE PACKAGING MACHINE:
#
#     scripts/dist-common.sh --probe-baseline
#
# which reads its own libc and libstdc++ and prints the three lines to paste back here. Until that
# has been run, treat a PASS from this gate as "no worse than the declared ceiling" rather than as
# "runs on the release machine" -- the two are the same statement only once the ceiling is right.
#
# The gate is still worth having before then, because its FAILING direction is exact and needs no
# confirmation at all: a development-machine build overshoots whatever the real ceiling is, and
# this catches it. That is the half of "proved in both directions" that can be proved from here --
# measured on the development machine the day this was written, where the plug-in reaches for
# GLIBC_2.38 through __isoc23_strtol, __isoc23_strtoll, __isoc23_strtoull, __isoc23_sscanf and
# fmodf, and for GLIBCXX_3.4.32 through _ZSt21ios_base_library_initv. Every one of those is the
# compiler picking a newer entry point for source that did not change.
NAMP_BASELINE_GLIBC="2.36"
NAMP_BASELINE_GLIBCXX="3.4.30"
NAMP_BASELINE_CXXABI="1.3.13"

namp_dist_die() {
    echo "$@" >&2
    exit 1
}

# The project() version, which is the first VERSION line in the product's lists file. Read rather
# than duplicated, and read the SAME way makedist-windows.sh reads it, so two releases of one
# product cannot be tagged differently from one another.
namp_dist_version() {
    local lists="$1" v
    v="$(sed -n 's/^[[:space:]]*VERSION[[:space:]][[:space:]]*\([0-9][0-9.]*\).*/\1/p' "$lists" | head -1)"
    [ -n "$v" ] || namp_dist_die "could not read the project version from $lists"
    printf '%s' "$v"
}

# ONE ARCHITECTURE FOLDER, AND IT IS THE LINUX ONE.
#
# A VST3 bundle holds Contents/<arch>/ per platform, so a build tree that was once configured with
# the MinGW toolchain and later re-configured natively keeps its Contents/x86_64-win directory:
# CMake writes the new binary beside the old one instead of replacing it, and `cp -r` then carries
# a Windows DLL into the Linux tarball. It loads nowhere and is pure weight. This is the mirror of
# the prune in makedist-windows.sh, for the mirror-image mistake.
namp_dist_prune_bundle_arches() {
    local bundle="$1" arch="$2" dir name
    for dir in "$bundle/Contents"/*/; do
        dir="${dir%/}"
        name="$(basename "$dir")"
        case "$name" in
            Resources | "${arch}-linux") ;;
            *)
                echo "warning: removing $name/ from the packaged bundle - it is not a Linux" >&2
                echo "  architecture folder and belongs to no Linux release. The build directory" >&2
                echo "  was configured for another platform at some point; delete it and re-run" >&2
                echo "  this script to stop seeing this." >&2
                rm -rf "$dir"
                ;;
        esac
    done
}

# NO STB_GNU_UNIQUE SYMBOLS. A unique symbol makes glibc's loader refuse to unload the library and
# binds it process-wide, so two NAM-derived plug-ins in one host collide - and the failure is an
# abort inside the host, not an error anything here could report. They appear when a static-local
# in an inline function or template escapes the visibility settings.
namp_dist_no_unique() {
    local elf="$1" label="$2" syms
    syms="$(nm -D --defined-only "$elf" | awk '$2 == "u" { print $3 }')"
    if [ -n "$syms" ]; then
        echo "$label exports STB_GNU_UNIQUE symbols:" >&2
        echo "$syms" | head -10 | sed 's/^/  /' >&2
        echo "A host loading a second NAM plug-in would abort. Check -fvisibility=hidden" >&2
        echo "and -Wl,--exclude-libs,ALL." >&2
        exit 1
    fi
}

# Exactly this set and nothing else. Affordable only where the target is built from our own sources
# with -fvisibility=hidden and --exclude-libs,ALL and carries no module entry point; anything else
# in the dynamic table is then a symbol a second plug-in in the same host process can be merged
# with. Pass no symbols at all to assert an empty dynamic table.
namp_dist_exports_exactly() {
    local elf="$1" label="$2"
    shift 2
    local want="$*" got
    got="$(nm -D --defined-only "$elf" | awk '$2 == "T" { print $3 }' | sort | tr '\n' ' ')"
    got="$(echo $got)"
    want="$(echo $want | tr ' ' '\n' | sort | tr '\n' ' ')"
    want="$(echo $want)"
    if [ "$got" != "$want" ]; then
        namp_dist_die "$label exports [$got] rather than exactly [$want]."
    fi
}

# Presence, not an exact set: an ELF shared object legitimately keeps a handful of weak C++
# template instantiations visible whatever -fvisibility=hidden and --exclude-libs,ALL do, so an
# "exactly these" rule fails on an innocent template.
namp_dist_exports_all() {
    local elf="$1" label="$2"
    shift 2
    local entry
    for entry in "$@"; do
        if ! nm -D --defined-only "$elf" | awk '$2 == "T" { print $3 }' | grep -qx "$entry"; then
            namp_dist_die "$label does not export $entry; no host can load it."
        fi
    done
}

namp_dist_must_not_link() {
    local elf="$1" pattern="$2" label="$3" why="$4"
    if ldd "$elf" | grep -qiE "$pattern"; then
        echo "$label links $pattern. $why" >&2
        ldd "$elf" | grep -iE "$pattern" >&2
        exit 1
    fi
}

namp_dist_must_link() {
    local elf="$1" pattern="$2" label="$3" why="$4"
    ldd "$elf" | grep -qiE "$pattern" || namp_dist_die "$label does not link $pattern. $why"
}

namp_dist_require_files() {
    local root="$1" label="$2"
    shift 2
    local f
    for f in "$@"; do
        [ -f "$root/$f" ] || namp_dist_die "$label is missing $f"
    done
}

# --- the ABI baseline gate --------------------------------------------------
# Reads the symbol VERSION REFERENCES out of the dynamic table -- what the binary asks its loader
# for -- and compares the highest of each family against the declared ceiling above. This is the
# right thing to measure rather than the distribution's package version: a binary that happens not
# to call anything added after 2.36 runs on 2.36 whatever it was compiled against, and one that
# calls a single such function does not, however old the rest of it is.
#
# THREE FAMILIES, BECAUSE THERE ARE THREE LIBRARIES BEHIND THEM. GLIBC_ is the C library, and it is
# the one everybody remembers. GLIBCXX_ and CXXABI_ are libstdc++, they move with GCC rather than
# with glibc, and they are just as fatal and less looked for: building with a newer compiler than
# the release machine's raises them even when the glibc calls are untouched.
namp_dist_max_version() {
    objdump -p "$1" 2>/dev/null |
        sed -n 's/.*[[:space:]]\('"$2"'_[0-9][0-9.]*\)[[:space:]]*$/\1/p;s/.*[[:space:]]\('"$2"'_[0-9][0-9.]*\).*/\1/p' |
        sed "s/^$2_//" | sort -uV | tail -1
}

NAMP_DIST_ABI_OVER=0

namp_dist_abi_baseline() {
    local label="$1" elf="$2" family declared found bad=0
    for family in GLIBC GLIBCXX CXXABI; do
        case "$family" in
            GLIBC) declared="$NAMP_BASELINE_GLIBC" ;;
            GLIBCXX) declared="$NAMP_BASELINE_GLIBCXX" ;;
            CXXABI) declared="$NAMP_BASELINE_CXXABI" ;;
        esac
        found="$(namp_dist_max_version "$elf" "$family")"
        [ -n "$found" ] || continue
        if [ "$(printf '%s\n%s\n' "$declared" "$found" | sort -V | tail -1)" != "$declared" ]; then
            echo "$label needs ${family}_${found}, and the declared baseline is ${family}_${declared}." >&2
            objdump -T "$elf" | grep -F "${family}_${found}" | awk '{print "    " $NF, $(NF-1)}' |
                sort -u | head -8 >&2
            bad=1
        fi
    done
    [ "$bad" -eq 0 ] || NAMP_DIST_ABI_OVER=1
}

# WHAT AN OVERSHOOT DOES, AND WHAT IT DELIBERATELY DOES NOT DO. It refuses the RELEASE, not the
# run. Every other assertion in a caller still executes and still has to pass, because the ceiling
# "may never block a build or a check tool -- it is a condition on the tarball, not on the
# compiler", and a packaging script that aborted on the development machine would take the
# bundle-shape, export, link and resource checks down with it, on the machine where those are
# actually being developed.
#
# So a development-machine run does everything, reports the overshoot in full, and then writes its
# tarball under a name carrying -DEVBUILD -- inside and out, since the staged directory is renamed
# too. There is no flag that turns the report off and none that produces a clean release name from
# an overshooting build: the difference is in the filename, where it cannot be lost by someone
# uploading the wrong file a month later.
namp_dist_mark() {
    if [ "${NAMP_DIST_ABI_OVER:-0}" -ne 0 ]; then
        echo >&2
        echo "  ^ This build needs a newer runtime than the declared release baseline, so it is" >&2
        echo "  NOT a release: it would fail to load on the distribution releases are built for." >&2
        echo "  That is expected here -- releases are packaged on the older machine for exactly" >&2
        echo "  this reason. The tarball below is marked -DEVBUILD accordingly." >&2
        echo >&2
        printf '%s' "-DEVBUILD"
    else
        printf '%s' ""
    fi
}

# --- the tarball ------------------------------------------------------------
namp_dist_tarball() {
    local stagedir="$1" pkgname="$2" outdir="$3" tarball
    mkdir -p "$outdir"
    tarball="$outdir/${pkgname}.tar.gz"
    rm -f "$tarball"
    tar -czf "$tarball" -C "$stagedir" "$pkgname"
    echo ""
    echo "Packaged: $tarball"
    echo ""
    echo "Contents:"
    tar -tzf "$tarball"
}

# --- install.sh fragments ---------------------------------------------------
# The generated installer's MECHANISM, emitted on stdout for the caller to splice around its own
# product-specific copying and prose. Shared because a fix to the icon-cache dance or the PATH
# note must land in both installers at once -- the two trees were kept in step by hand once and
# that is the failure this repository exists to end.
# Named groups rather than one block, so an installer that puts nothing in the menu does not
# declare ICON_ROOT and define a refresh() it never calls. This is a selector and NOT the flags
# table the header refuses: a group left out is an unbound variable under `set -u` and the
# installer dies on the spot, where a dropped assertion behind a flag would have reported PASS.
namp_install_preamble() {
    local group
    cat <<'EOF'
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EOF
    for group in "$@"; do
        case "$group" in
            plugin)
                cat <<'EOF'
VST3_DIR="$HOME/.vst3"
LV2_DIR="$HOME/.lv2"
EOF
                ;;
            desktop)
                cat <<'EOF'
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_ROOT="$HOME/.local/share/icons/hicolor"

refresh() {
  command -v update-desktop-database >/dev/null && update-desktop-database "$APP_DIR" 2>/dev/null || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -f -t "$ICON_ROOT" 2>/dev/null || true
}
EOF
                ;;
            *) namp_dist_die "namp_install_preamble: unknown group '$group'" ;;
        esac
    done
}

namp_install_icons_install() {
    cat <<EOF
install -m 644 "\$HERE/desktop/$1.desktop" "\$APP_DIR/$1.desktop"
for SIZE in 256 128 64 48; do
  mkdir -p "\$ICON_ROOT/\${SIZE}x\${SIZE}/apps"
  install -m 644 "\$HERE/desktop/$1-\${SIZE}.png" "\$ICON_ROOT/\${SIZE}x\${SIZE}/apps/$1.png"
done
EOF
}

namp_install_icons_remove() {
    cat <<EOF
rm -f "\$APP_DIR/$1.desktop"
for SIZE in 256 128 64 48; do
  rm -f "\$ICON_ROOT/\${SIZE}x\${SIZE}/apps/$1.png"
done
EOF
}

namp_install_path_note() {
    cat <<EOF
case ":\$PATH:" in
  *":\$BIN_DIR:"*) ;;
  *) echo "Note: \$BIN_DIR is not on your PATH, so '$1' will not be"
     echo "found by name from a shell. The menu entry works either way." ;;
esac
echo
echo "To remove it again:  ./install.sh --uninstall"
EOF
}

# --- the probe ---------------------------------------------------------------
# Run ON THE PACKAGING MACHINE to turn the declared ceiling above into a measured one.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    if [ "${1:-}" = "--probe-baseline" ]; then
        _libc="$(ldd /bin/true | sed -n 's/.*=> \(.*libc\.so\.6\) .*/\1/p' | head -1)"
        _libcxx="$(/sbin/ldconfig -p 2>/dev/null | sed -n 's/.*=> \(.*libstdc++\.so\.6\)$/\1/p' | head -1)"
        [ -n "$_libc" ] || namp_dist_die "could not find this machine's libc.so.6"
        [ -n "$_libcxx" ] || namp_dist_die "could not find this machine's libstdc++.so.6"
        echo "# measured on $(uname -srm), $(ldd --version | head -1)" >&2
        echo "# libc     $_libc" >&2
        echo "# libstdc++ $_libcxx" >&2
        echo >&2
        echo "NAMP_BASELINE_GLIBC=\"$(objdump -T "$_libc" | grep -o 'GLIBC_[0-9][0-9.]*' |
            sed 's/^GLIBC_//' | sort -uV | tail -1)\""
        echo "NAMP_BASELINE_GLIBCXX=\"$(objdump -T "$_libcxx" | grep -o 'GLIBCXX_[0-9][0-9.]*' |
            sed 's/^GLIBCXX_//' | sort -uV | tail -1)\""
        echo "NAMP_BASELINE_CXXABI=\"$(objdump -T "$_libcxx" | grep -o 'CXXABI_[0-9][0-9.]*' |
            sed 's/^CXXABI_//' | sort -uV | tail -1)\""
        exit 0
    fi
    namp_dist_die "dist-common.sh is a library; source it. The one exception is --probe-baseline."
fi
