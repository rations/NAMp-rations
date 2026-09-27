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
# tarball under a FILE name carrying -DEVBUILD. There is no flag that turns the report off and none
# that produces a clean release name from an overshooting build: the difference is in the filename,
# where it cannot be lost by someone uploading the wrong file a month later.
#
# THE EXTRACTED DIRECTORY NO LONGER CARRIES IT, and that is a narrowing of this mark rather than a
# weakening of it. What the rule is written against is the wrong file being published, and a file
# is what gets published -- the directory only exists on the machine of whoever already downloaded
# it, where the question has been settled. Carrying the mark in both places bought nothing for that
# failure and cost every ordinary user a folder named after a machine detail. The directory is the
# product and its version; see namp_dist_tarball.
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
#
# THE FOLDER NAME AND THE FILE NAME ARE TWO ARGUMENTS, not one, and that is the whole shape of this
# helper. What a user ends up living with is the directory the archive extracts to, which wants to
# be the product and its version and nothing else; what a packager has to tell apart in a download
# folder is the file, which wants the architecture and any mark saying this one is not releasable.
# Folding them into one string served the file and made the directory carry a machine detail that
# means nothing once it is unpacked. macOS has always done it this way -- makedist-mac.sh stages
# NAMp-rations-<version>/ and dittos it into NAMp-rations-<version>-macos-<arch>.zip -- so this is
# the two other platforms catching up rather than a new idea. The caller passes the archive path in
# full, which is also what namp_dist_zip below has always taken.
namp_dist_tarball() {
    local stagedir="$1" rootname="$2" tarball="$3" epoch="${4:-}"
    mkdir -p "$(dirname "$tarball")"
    rm -f "$tarball"
    if [ -n "$epoch" ]; then
        namp_dist_stamp_tree "$stagedir/$rootname" "$epoch"
        # --sort=name for a defined order, --owner/--group/--numeric-owner to take the packager's
        # identity out, --mtime to override what is on disk, and `gzip -n` because gzip writes its
        # OWN timestamp into its header and tar -z gives no way to say otherwise.
        tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
            -cf - -C "$stagedir" "$rootname" | gzip -n > "$tarball"
    else
        tar -czf "$tarball" -C "$stagedir" "$rootname"
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
    local stagedir="$1" rootname="$2" zip="$3" epoch="${4:-}"
    rm -f "$zip"
    mkdir -p "$(dirname "$zip")"
    [ -z "$epoch" ] || namp_dist_stamp_tree "$stagedir/$rootname" "$epoch"
    ( cd "$stagedir" && python3 -m zipfile -c "$zip" "$rootname" )
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
# The menu and icon directories are derived from XDG_DATA_HOME, NOT hard-coded to ~/.local/share,
# because that variable is what the menu itself reads: GLib's g_get_user_data_dir() returns
# $XDG_DATA_HOME when it is set and $HOME/.local/share only as the fallback (measured against
# GLib 2.84.4). A user who has relocated XDG_DATA_HOME -- and some distributions and dotfile
# setups do -- would otherwise get the entry installed into a directory nothing ever scans, which
# looks exactly like the install having silently done nothing.
# The Base Directory Specification says a relative value is invalid and must be ignored, so the
# leading-slash test is the spec's rule and not a guess.
if [ -n "${XDG_DATA_HOME:-}" ] && [ "${XDG_DATA_HOME#/}" != "$XDG_DATA_HOME" ]; then
  DATA_HOME="$XDG_DATA_HOME"
else
  DATA_HOME="$HOME/.local/share"
fi
# There is no XDG variable for user binaries; ~/.local/bin is the location systemd's
# file-hierarchy(7) specifies, so it stays literal rather than being invented from one.
BIN_DIR="$HOME/.local/bin"
APP_DIR="$DATA_HOME/applications"
ICON_ROOT="$DATA_HOME/icons/hicolor"

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
#
# Exec and TryExec are REWRITTEN to an absolute path here rather than copied through, because the
# staged entry carries the bare command name -- the right form for a distribution that installs
# into /usr/bin, and the wrong one for this installer, which installs into ~/.local/bin. A desktop
# entry's Exec is resolved against the PATH of the SESSION, not of a login shell, and a session
# started by a display manager or by openbox-session commonly has PATH=/usr/local/bin:/usr/bin:/bin
# with no ~/.local/bin on it. Measured, both halves, on a machine where the binary was installed
# and present:
#   * openbox (obmenu-generator) reads Exec out of the file and hands it to exec, so the entry is
#     listed in the menu and clicking it fails with "file not found";
#   * GLib REJECTS the file outright -- g_desktop_app_info_load_from_keyfile returns FALSE when
#     g_find_program_in_path(argv[0]) is NULL (checked against GLib 2.84.4:
#     Gio.DesktopAppInfo.new_from_filename returned NULL under the session PATH and an object under
#     a PATH carrying ~/.local/bin). Everything built on GDesktopAppInfo therefore shows no entry
#     AT ALL, in no category, including "All" -- which is what MATE does.
# Exec is quoted and TryExec is not: GLib unquotes Exec per the Desktop Entry Specification and
# passes TryExec to g_find_program_in_path verbatim, so quoting TryExec would break the very lookup
# it exists to perform. Both were confirmed by loading and launching an entry whose path contained
# a space. TryExec is added so that a stale entry left behind by a hand-deleted binary hides itself
# instead of offering a launch that cannot work.
# Written to a sibling and renamed over, rather than redirected onto the target: a redirection
# truncates before grep runs, so a missing or unreadable staged entry would leave a WORKING install
# holding a zero-length one, which under `set -e` is where the installer would then stop.
namp_install_icons_install() {
    cat <<EOF
{
  grep -v -E '^(Exec|TryExec)=' "\$HERE/$2/$1.desktop"
  printf 'Exec="%s/$1"\n' "\$BIN_DIR"
  printf 'TryExec=%s/$1\n' "\$BIN_DIR"
} > "\$APP_DIR/$1.desktop.new"
chmod 644 "\$APP_DIR/$1.desktop.new"
mv -f "\$APP_DIR/$1.desktop.new" "\$APP_DIR/$1.desktop"
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

# The "works either way" claim below is true because namp_install_icons_install writes an ABSOLUTE
# Exec into the installed entry. It was not true while the entry carried the bare command name, and
# it must not be restated anywhere that stops being the case.
namp_install_path_note() {
    cat <<EOF
case ":\$PATH:" in
  *":\$BIN_DIR:"*) ;;
  *) echo "Note: \$BIN_DIR is not on your PATH, so '$1' will not be"
     echo "found by name from a shell. The menu entry works either way -- it"
     echo "was installed with the full path to the program, not just its name." ;;
esac
echo
echo "To remove it again:  ./install.sh --uninstall"
EOF
}

# --- can this machine run what is about to be installed? --------------------
# $1 is the architecture the package was built for, baked into the installer because the installer
# cannot know it any other way: it is the same script in every package.
#
# Two questions, asked in order, before anything is copied.
#
# WRONG PACKAGE. An aarch64 bundle on an x86_64 machine installs cleanly and then never loads: a
# host skips a bundle whose binary it cannot open, and says nothing. The same happens on a Raspberry
# Pi running a 32-bit OS on its 64-bit kernel -- uname -m names the KERNEL and says aarch64 there,
# while every library on the system is 32-bit -- so the word size of the userland is asked too, with
# getconf, which is part of glibc and therefore present wherever these binaries could run at all.
#
# MISSING LIBRARIES. Asked of the dynamic loader, through ldd, and NOT of the package manager. A
# tester's namp-rack did not start until libsuil-0-0 was installed, and INSTALL.txt already said to
# install it -- eighty lines down, which is where the check belongs instead. ldd rather than
# `dpkg -s` for three reasons:
#   * it asks the question that decides whether a program starts. A package can be installed while
#     its library is not on the loader's path, and on Debian, Devuan and Ubuntu that is exactly the
#     state of PipeWire's libjack: pipewire-jack puts it in pipewire-0.3/jack/ and ships the
#     ld.so.conf.d entry that would publish it only as an EXAMPLE under /usr/share/doc. Read out of
#     the Contents index of Debian 12, Debian 13, Ubuntu 22.04 and Ubuntu 24.04; every one does it.
#   * it works on every glibc distribution, whatever its package manager;
#   * it has no list to keep in step with the build. Whatever the binaries were linked against,
#     the loader reports.
#
# What is then DONE about a missing library depends on the package manager, and only apt is acted
# on. The soname-to-package table below was read out of the same four Contents indexes rather than
# recalled, and all six names are identical in all four -- none of these libraries was renamed by
# the 64-bit time_t transition. Every other distribution is told the library names and left to its
# own package manager, because a table for dnf or pacman would be a guess, and a guess that runs
# under sudo is not an acceptable failure mode for an installer.
#
# JACK has three honest answers and the installer must not pick the wrong one. On a machine running
# PulseAudio or plain JACK -- including Devuan, which has no systemd and often no PipeWire at all --
# the library is libjack-jackd2-0, and that is the default. Only when a pipewire process is actually
# running for this user is the PipeWire route given instead, because installing jackd2's library
# there produces a program that STARTS and then talks to a JACK server that does not exist: silence
# that looks like success, the worst outcome available. The running process is asked with pgrep,
# not with `systemctl --user`, so the test means the same thing without systemd. The PipeWire route
# is printed and never executed: its two options are the two that Debian's own README.Debian for
# pipewire gives (pw-jack, or the ld.so.conf.d copy plus ldconfig), and no machine this was written
# on runs PipeWire, so it has never been run from here.
#
# JACK's LIBRARY is installed and its SERVER is not. The library is what stops the program
# starting; jackd2 would bring a debconf dialogue and, through its Recommends, qjackctl and Qt, and
# which JACK server someone runs is their decision. The closing note names it instead.
#
# Nothing here fails the install. The plug-ins need only cairo, FreeType and X11, so a machine
# missing lilv can still use them; the files are installed either way and what will not start is
# said at the end, by name.
namp_install_runtime_check() {
    printf 'PKG_ARCH="%s"\n' "$1"
    cat <<'EOF'

namp_wrong_machine() {
  local kern bits
  kern="$(uname -m)"
  bits="$(getconf LONG_BIT 2>/dev/null || echo '?')"
  if [ "$kern" != "$PKG_ARCH" ]; then
    echo "This package is for $PKG_ARCH, and this machine is $kern." >&2
    echo "Download the linux-$kern package instead. Nothing has been installed." >&2
    return 0
  fi
  if [ "$bits" != 64 ]; then
    echo "This machine has a 64-bit processor but runs a $bits-bit operating system, and" >&2
    echo "NAMp is 64-bit only - there is no 32-bit build. On a Raspberry Pi, that means" >&2
    echo "the 64-bit Raspberry Pi OS. Nothing has been installed." >&2
    return 0
  fi
  return 1
}

if namp_wrong_machine; then
  exit 1
fi

# One "soname<TAB>who needs it" line per library the loader cannot find.
namp_missing_libs() {
  local f who
  for f in "$HERE"/plugin/NAMp-rations.vst3/Contents/*-linux/*.so \
           "$HERE"/plugin/NAMp-rations.lv2/*.so \
           "$HERE"/pedals/*.vst3/Contents/*-linux/*.so \
           "$HERE"/rack/namp-rack; do
    [ -f "$f" ] || continue
    case "$f" in
      "$HERE"/rack/*)   who="the standalone (namp-rack)" ;;
      "$HERE"/pedals/*) who="the pedals" ;;
      *)                who="the plug-in" ;;
    esac
    { ldd "$f" 2>/dev/null || true; } |
      awk -v W="$who" '$2 == "=>" && $3 == "not" { print $1 "\t" W }'
  done | sort -u
}

namp_list_missing() {
  printf '%s\n' "$1" | awk -F '\t' '
    { if ($1 in who) who[$1] = who[$1] ", " $2; else who[$1] = $2 }
    END { for (s in who) printf "    %-20s needed by %s\n", s, who[s] }' | sort
}

namp_apt_package() {
  case "$1" in
    libX11.so.6)      echo libx11-6 ;;
    libcairo.so.2)    echo libcairo2 ;;
    libfreetype.so.6) echo libfreetype6 ;;
    libjack.so.0)     echo libjack-jackd2-0 ;;
    liblilv-0.so.0)   echo liblilv-0-0 ;;
    libsuil-0.so.0)   echo libsuil-0-0 ;;
  esac
}

namp_pipewire_running() {
  command -v pgrep >/dev/null 2>&1 && pgrep -x -u "$(id -u)" pipewire >/dev/null 2>&1
}

if command -v ldd >/dev/null 2>&1; then
  MISSING="$(namp_missing_libs)"
else
  MISSING=""
  echo "Note: ldd is not available, so whether this machine has the libraries NAMp"
  echo "needs could not be checked. INSTALL.txt lists them under Requirements."
  echo
fi

if [ -n "$MISSING" ]; then
  echo "Some libraries NAMp needs are not installed on this machine:"
  namp_list_missing "$MISSING"
  echo

  PKGS=""
  UNMAPPED=""
  PW_JACK=0
  for SO in $(printf '%s\n' "$MISSING" | cut -f1 | sort -u); do
    if [ "$SO" = libjack.so.0 ] && namp_pipewire_running; then
      PW_JACK=1
      continue
    fi
    P="$(namp_apt_package "$SO")"
    if [ -n "$P" ]; then PKGS="$PKGS $P"; else UNMAPPED="$UNMAPPED $SO"; fi
  done
  HAVE_APT=0
  if command -v apt-get >/dev/null 2>&1; then HAVE_APT=1; fi

  if [ "$PW_JACK" = 1 ]; then
    echo "PipeWire is running as your sound server, so JACK should come from PipeWire"
    echo "rather than from a separate JACK library. Install PipeWire's JACK support if it"
    echo "is not already there (the package is pipewire-jack; on Ubuntu 22.04 it is"
    echo "pipewire-audio-client-libraries), and then EITHER start the standalone with"
    echo
    echo "    pw-jack namp-rack"
    echo
    echo "OR let every JACK program use PipeWire, which also makes the menu entry work:"
    echo
    echo "    sudo cp /usr/share/doc/pipewire/examples/ld.so.conf.d/pipewire-jack-*.conf /etc/ld.so.conf.d/"
    echo "    sudo ldconfig"
    echo
  fi

  if [ -n "$PKGS" ] && [ "$HAVE_APT" = 1 ]; then
    if [ "$PW_JACK" = 1 ]; then
      echo "The rest are in these packages:"
    else
      echo "They are in these packages:"
    fi
    echo
    echo "    sudo apt-get install$PKGS"
    echo
    if [ "$(id -u)" -eq 0 ]; then
      SUDO=""
    elif command -v sudo >/dev/null 2>&1; then
      SUDO="sudo"
    else
      SUDO="-"
    fi
    # Only ever asked at a terminal: piped or scripted, the command above is the whole answer.
    if [ -t 0 ] && [ -t 1 ] && [ "$SUDO" != "-" ]; then
      ANSWER=""
      read -r -p "Install them now? [Y/n] " ANSWER || ANSWER=n
      case "$ANSWER" in
        [nN]*) echo ;;
        *)
          # $SUDO and $PKGS unquoted on purpose: the first may be empty, the second is a list.
          if ! $SUDO apt-get install $PKGS; then
            echo
            echo "apt could not install them. If it could not FIND a package, run"
            echo "'sudo apt-get update' and try again; on Ubuntu, liblilv-0-0 and libsuil-0-0"
            echo "are in the universe repository, which has to be enabled."
          fi
          echo
          MISSING="$(namp_missing_libs)"
          ;;
      esac
    fi
  fi

  # Whatever the table above does not cover: every library on a system without apt, and on one
  # with it any library the table has no entry for -- a dependency of a dependency, say, which is
  # still reported by ldd and would otherwise be listed with no advice at all.
  if [ "$HAVE_APT" = 0 ] && [ -n "$PKGS$UNMAPPED" ]; then
    echo "Install whatever provides them with your distribution's package manager, then"
    echo "run ./install.sh again."
    echo
  elif [ -n "$UNMAPPED" ]; then
    echo "Install whatever package provides$UNMAPPED, then run ./install.sh again."
    echo
  fi
fi
EOF
}

# The closing half: said after the files are in place, so it is the last thing on the screen.
namp_install_runtime_report() {
    cat <<'EOF'
if [ -n "$MISSING" ]; then
  echo
  echo "STILL MISSING - what needs these will not start until they are installed:"
  namp_list_missing "$MISSING"
  if [ "${PW_JACK:-0}" = 1 ]; then
    echo "(libjack.so.0 is the exception: 'pw-jack namp-rack' starts it without it, as above.)"
  fi
  echo "Run ./install.sh again afterwards to check."
fi
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
