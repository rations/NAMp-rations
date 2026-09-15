#!/usr/bin/env bash
# The Linux release machinery both products share, plus the ABI baseline gate.
#
# HOW THIS IS SPLIT, AND WHY IT IS NOT ONE SCRIPT. Everything here is either generic over an ELF
# (what it links, what it exports, what glibc it needs) or generic over a tarball. Nothing here
# knows what NAMp-rations.vst3 or namp-rack is, and that is the line:
#
#   this file          the mechanism -- one implementation, so a fix to it lands once
#   products/*/scripts/stage-linux.sh
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
# than duplicated, and read the SAME way every other release path reads it, so two releases of one
# product cannot be tagged differently from one another.
namp_dist_version() {
    local lists="$1" v
    v="$(sed -n 's/^[[:space:]]*VERSION[[:space:]][[:space:]]*\([0-9][0-9.]*\).*/\1/p' "$lists" | head -1)"
    [ -n "$v" ] || namp_dist_die "could not read the project version from $lists"
    printf '%s' "$v"
}

# THE RELEASE VERSION, AND THE ASSERTION THAT THERE IS ONLY ONE OF IT.
#
# The release is one package holding both products, so it has one version number -- and the two
# products each carry their own project() version in their own lists file. Those two numbers
# agreeing is a fact to check, not an arrangement to trust: they are in different files, edited by
# different hands on different days, and a package labelled 0.3.0 whose plug-in half says 0.2.9 is
# wrong in a way that survives every other check here and only surfaces in a host's plug-in list,
# months later, as a version nobody can account for.
#
# This is the same class of drift the whole repository exists to end. The two trees were kept in
# step by hand for twenty-six commits and a parameter default fell out of step silently; a version
# number is cheaper to check than that was and there is no reason to find out the hard way twice.
namp_dist_release_version() {
    local repo="$1" v_rations v_rack
    v_rations="$(namp_dist_version "$repo/products/rations/CMakeLists.txt")"
    v_rack="$(namp_dist_version "$repo/products/rack/CMakeLists.txt")"
    [ "$v_rations" = "$v_rack" ] || namp_dist_die "the two products disagree about the version:
  products/rations/CMakeLists.txt  $v_rations
  products/rack/CMakeLists.txt     $v_rack
They ship in ONE package, so they need ONE version. Bump whichever is behind and re-run."
    printf '%s' "$v_rations"
}

# ONE ARCHITECTURE FOLDER, AND IT IS THE LINUX ONE.
#
# A VST3 bundle holds Contents/<arch>/ per platform, so a build tree that was once configured with
# the MinGW toolchain and later re-configured natively keeps its Contents/x86_64-win directory:
# CMake writes the new binary beside the old one instead of replacing it, and `cp -r` then carries
# a Windows DLL into the Linux tarball. It loads nowhere and is pure weight. This is the mirror of
# the prune in the Windows stage scripts, for the mirror-image mistake.
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

# The same for directories -- a VST3 bundle and an LV2 bundle are DIRECTORIES, so require_files
# cannot ask after one. Separate rather than one function that accepts either, because "it is
# there" and "it is there and it is a directory" are different claims and a bundle that arrived
# as a stray regular file is a failure worth naming.
namp_dist_require_dirs() {
    local root="$1" label="$2"
    shift 2
    local d
    for d in "$@"; do
        [ -d "$root/$d" ] || namp_dist_die "$label is missing the $d directory"
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
    # THE OVERSHOOT HAS TO CROSS A PROCESS BOUNDARY, so it is recorded in a FILE and not only in
    # a variable. Each product builds and gates itself in its own stage script, which is a
    # separate process from the one that names the tarball -- so a plain shell variable set here
    # dies with that process and namp_dist_mark, running in the parent, reads its own copy and
    # sees 0 forever. Measured, not deduced: the first release run after the two products were
    # merged into one package produced an unmarked tarball from an overshooting build, which is
    # the precise failure -DEVBUILD exists to prevent and the precise shape the gate is meant to
    # make impossible.
    #
    # The file is the caller's to create and to place OUTSIDE the staged tree; one line per
    # artefact, so what comes back is the list of what overshot rather than a boolean.
    if [ "$bad" -ne 0 ]; then
        NAMP_DIST_ABI_OVER=1
        [ -z "${NAMP_DIST_ABI_OVER_FILE:-}" ] || echo "$label" >> "$NAMP_DIST_ABI_OVER_FILE"
    fi
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
    local over="${NAMP_DIST_ABI_OVER:-0}"
    # Either this process saw the overshoot itself, or a stage script in a child process recorded
    # it in the shared file. Both are checked, because the two packaging shapes -- one script that
    # gates and packages, and a root script that delegates the gating -- both exist here.
    if [ -s "${NAMP_DIST_ABI_OVER_FILE:-/nonexistent}" ]; then
        over=1
        echo >&2
        echo "  These overshot the declared release baseline:" >&2
        sed 's/^/    /' "$NAMP_DIST_ABI_OVER_FILE" >&2
    fi
    if [ "$over" -ne 0 ]; then
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

# --- Windows PE: the reproducibility the linker flag only half delivers ------
#
# THE LINKER ZEROES THE TIMESTAMP AND `strip` PUTS IT BACK. Measured, not deduced: the toolchain
# passes -Wl,--no-insert-timestamp, and the binary in the build tree does carry a COFF
# TimeDateStamp of 0. Then the packaging step runs `<triple>-strip --strip-unneeded` on its copy,
# and binutils writes a FRESH WALL-CLOCK VALUE into that field as it rewrites the file. Two strips
# of one input four seconds apart produced two files differing in exactly two bytes, and those two
# bytes were the low half of the stamp.
#
# So the artefact that SHIPS was never byte-reproducible, only the one in the build tree was, and
# "build it again and compare every byte" -- the method this whole project verifies itself with --
# did not reach the thing it verifies. There is no strip flag for this; the field has to be put
# back to zero afterwards.
#
# WHY THE CHECKSUM IS RECOMPUTED RATHER THAN LEFT. binutils computes the PE checksum over the file
# it wrote, stamp included, so zeroing four bytes afterwards leaves it stale. The algorithm is the
# documented one -- 16-bit ones-complement sum over the whole file with the checksum field itself
# read as zero, plus the file length -- and this implementation was validated by recomputing the
# checksum binutils had just written on two real binaries and getting the same value back, before
# it was ever used to write one.
namp_dist_pe_derandomise() {
    local exe="$1"
    python3 - "$exe" <<'PYEOF'
import struct, sys

path = sys.argv[1]
with open(path, 'rb') as fh:
    d = bytearray(fh.read())

e_lfanew = struct.unpack_from('<I', d, 0x3c)[0]
if d[e_lfanew:e_lfanew + 4] != b'PE\0\0':
    sys.exit("not a PE file: %s" % path)

stamp_off = e_lfanew + 4 + 4          # COFF header, TimeDateStamp
cksum_off = e_lfanew + 24 + 64        # optional header, CheckSum
struct.pack_into('<I', d, stamp_off, 0)
struct.pack_into('<I', d, cksum_off, 0)

total = 0
pad = bytes(d) + (b'\0' if len(d) & 1 else b'')
for i in range(0, len(pad), 2):
    if i == cksum_off or i == cksum_off + 2:
        continue
    total += struct.unpack_from('<H', pad, i)[0]
    total = (total & 0xffff) + (total >> 16)
total = (total & 0xffff) + (total >> 16)
struct.pack_into('<I', d, cksum_off, (total + len(d)) & 0xffffffff)

with open(path, 'wb') as fh:
    fh.write(d)
PYEOF
}

# Read the COFF header's TimeDateStamp -- the one that moves. Note that `objdump -p` prints TWO
# fields whose labels differ by one word: "Time/Date" is this one, and "Time/Date stamp" is the
# DEBUG DIRECTORY's, which the linker flag really does zero and which therefore reads 0 on a
# binary whose COFF stamp is live. A gate written against the wrong one passes on a
# non-reproducible file, which is exactly what happened here. Read the bytes instead: no label to
# get wrong, and no locale to render a date in.
namp_dist_pe_timestamp() {
    python3 - "$1" <<'PYEOF'
import struct, sys
with open(sys.argv[1], 'rb') as fh:
    d = fh.read()
e = struct.unpack_from('<I', d, 0x3c)[0]
print(struct.unpack_from('<I', d, e + 8)[0])
PYEOF
}

namp_dist_pe_assert_no_timestamp() {
    local exe="$1" label="$2" stamp
    stamp="$(namp_dist_pe_timestamp "$exe")"
    [ "$stamp" = "0" ] || namp_dist_die "$label carries a COFF TimeDateStamp of $stamp, so this
build is not reproducible. The linker zeroes that field and \`strip\` writes it back; the packaging
step must call namp_dist_pe_derandomise after stripping."
}

# --- reproducibility of the ARCHIVE, as distinct from the binaries inside it -----------------
#
# THE BINARIES WERE REPRODUCIBLE AND THE ARCHIVES WERE NOT, and nothing here was watching. This
# project verifies itself by building twice and comparing every byte, and a great deal of work has
# gone into making that true of the artefacts -- -ffile-prefix-map, -Wl,--no-insert-timestamp, and
# namp_dist_pe_derandomise above for the COFF stamp `strip` puts back. None of it reached the
# tarball or the ZIP, because an archive records more than its members' contents:
#
#   mtime        every member carries one, and it is whatever the staging `cp` happened to write
#   order        tar walks readdir order, which is the filesystem's, not a defined one
#   owner/group  the packager's uid, gid and names
#   gzip header  gzip writes the CURRENT TIME into its own header, independent of tar
#
# So two release runs from one commit produced two different files, and "did the release move?"
# could not be asked of the thing that is actually uploaded. These two helpers close that: stamp
# the staged tree to a fixed time, then archive it in a defined order with no identity in it.
#
# THE TIME IS THE COMMIT'S, not the wall clock and not zero. Zero would make every release in
# history look identical to a mirror, and the wall clock is the thing being removed; the commit
# date is the one timestamp that is a genuine fact about what is in the archive. A dirty tree
# still gets HEAD's, which is fine -- it is a fixed value, and a dirty tree is marked as
# unpublishable by the gates that care.
namp_dist_epoch() {
    local repo="$1" e
    e="$(git -C "$repo" show -s --format=%ct HEAD 2>/dev/null || true)"
    [ -n "$e" ] || e=0
    printf '%s' "$e"
}

namp_dist_stamp_tree() {
    local dir="$1" epoch="$2"
    find "$dir" -exec touch -h -d "@$epoch" {} +
}

# --- the tarball ------------------------------------------------------------
namp_dist_tarball() {
    local stagedir="$1" pkgname="$2" outdir="$3" epoch="${4:-}" tarball
    mkdir -p "$outdir"
    tarball="$outdir/${pkgname}.tar.gz"
    rm -f "$tarball"
    if [ -n "$epoch" ]; then
        namp_dist_stamp_tree "$stagedir/$pkgname" "$epoch"
        # --sort=name for a defined order, --owner/--group/--numeric-owner to take the packager's
        # identity out, --mtime to override what is on disk, and `gzip -n` because gzip writes its
        # OWN timestamp into its header and tar -z gives no way to say otherwise.
        tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
            -cf - -C "$stagedir" "$pkgname" | gzip -n > "$tarball"
    else
        tar -czf "$tarball" -C "$stagedir" "$pkgname"
    fi
    echo ""
    echo "Packaged: $tarball"
    echo ""
    echo "Contents:"
    tar -tzf "$tarball"
}

# The ZIP equivalent. python3 rather than zip(1): zip is not installed everywhere and this needs no
# extra package. It stores each member's mtime from the filesystem, so the stamp above is what
# makes it reproducible -- there is no flag to pass. -c takes the directory and stores it with its
# own name at the archive root, which is what an extract-anywhere release wants.
namp_dist_zip() {
    local stagedir="$1" pkgname="$2" zip="$3" epoch="${4:-}"
    rm -f "$zip"
    mkdir -p "$(dirname "$zip")"
    [ -z "$epoch" ] || namp_dist_stamp_tree "$stagedir/$pkgname" "$epoch"
    ( cd "$stagedir" && python3 -m zipfile -c "$zip" "$pkgname" )
    echo ""
    echo "Packaged: $zip"
    echo ""
    echo "Contents:"
    python3 -m zipfile -l "$zip"
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

# $1 is the application name; $2 is the staged directory holding its .desktop and its four PNGs,
# relative to the extracted archive. The directory is a parameter rather than a constant because
# the release package holds more than one product and each keeps its own files under its own
# subdirectory -- there is no one "desktop/" any more.
namp_install_icons_install() {
    cat <<EOF
install -m 644 "\$HERE/$2/$1.desktop" "\$APP_DIR/$1.desktop"
for SIZE in 256 128 64 48; do
  mkdir -p "\$ICON_ROOT/\${SIZE}x\${SIZE}/apps"
  install -m 644 "\$HERE/$2/$1-\${SIZE}.png" "\$ICON_ROOT/\${SIZE}x\${SIZE}/apps/$1.png"
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
