#!/bin/bash
# lv2-gate.sh — the LV2 bundle's gate: what a host sees, and whether it runs.
#
# The LV2 build is the SAME processor, controller and editor the VST3 bundle carries, wrapped so
# an LV2 host can drive them (lv2/rationslv2.h says why it is a wrapper and not a second plug-in).
# So the DSP needs no second proof — rations_offline, rations_switchcheck, rations_racecheck and
# rations_rtcheck are statements about code this build shares. What DOES need proving is
# everything between the host and that code, and all of it fails silently:
#
#   * the bundle's Turtle has to parse, and its manifest has to OFFER the UI. A UI declared only
#     in the plug-in's own file is invisible to most hosts, and nothing about the build says so.
#   * every output port the editor reads has to carry a ui:portNotification. Without them the
#     meters, the bank progress bar and the capture readout are permanently dead while the
#     plug-in loads, plays and otherwise works perfectly.
#   * the two shared objects have to export their one entry point and nothing else. Two plug-ins
#     in one host process that both export the same inline symbol can be merged through
#     STB_GNU_UNIQUE, leaving one running the other's code — and this tree ships the VST3 and the
#     LV2 of the same plug-in on Linux, so a host with both installed is exactly that path.
#   * the whole editor-to-DSP round trip has to join up: a capture load goes in as an atom, the
#     workers build the bank, and the capability report comes back out of the notify port. Every
#     one of those seams is new in this format.
#
# Three halves, in the order a failure is cheapest to read:
#
#   1. shape        the exports, measured with nm, and the bundle's contents.
#   2. rations_lv2check   what lilv — the reference discovery library, and what real hosts use —
#                   sees in the INSTALLED bundle, plus loading it, running it, and round-tripping
#                   its state. With --captures it also loads a real bank and asserts the amp makes
#                   a finite, non-silent sound with its meters moving.
#   3. sord_validate      the RDF schema check, when the tool is available. It is not packaged on
#                   this machine; build it from ~/third_party/source-lv2/sord with meson and put
#                   it on PATH, or point $RATIONS_SORD_VALIDATE at it.
#
# It needs no JACK server. It installs the bundle to ~/.lv2 first, because a gate that reads the
# BUILD directory would not catch a bundle that is wrong once installed — and because a stale
# root-owned copy under /usr/lib/lv2 shadows ~/.lv2 and would have you debugging the wrong binary.
#
# Usage:
#   scripts/lv2-gate.sh                              # everything that needs no captures
#   scripts/lv2-gate.sh --captures ~/NAM/captures    # ... plus the end-to-end bank load
#   scripts/lv2-gate.sh --no-install                 # gate whatever is already in ~/.lv2

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build="${RATIONS_BUILD_DIR:-$root/build}"
bundle="$build/lv2/rations.lv2"
installed="$HOME/.lv2/rations.lv2"

captures="${RATIONS_TEST_CAPTURES:-}"
do_install=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --captures) captures="$2"; shift 2 ;;
        --no-install) do_install=0; shift ;;
        -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
        *) echo "lv2-gate: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

fail=0
note() { printf '\n== %s ==\n' "$1"; }
bad() { printf 'lv2-gate: FAIL  %s\n' "$1" >&2; fail=1; }

# --- 0. the build -----------------------------------------------------------------------------
if [ ! -d "$bundle" ]; then
    echo "lv2-gate: $bundle is not there; build first:" >&2
    echo "  cmake --build $build" >&2
    echo "(a build with no lv2.pc on the system skips the LV2 targets and says so)" >&2
    exit 2
fi

note "shape"
for f in manifest.ttl rations.ttl rations.so rations_ui.so; do
    [ -f "$bundle/$f" ] || bad "the bundle has no $f"
done
# The five load-bearing resources, the same list the two makedist scripts gate on.
for f in fonts/Michroma-Regular.ttf fonts/Roboto-Regular.ttf img/head.png img/cabinet.png \
         img/dial.png img/File.svg; do
    [ -f "$bundle/$f" ] || bad "the bundle has no $f"
done

# Exactly one exported symbol each. Anything else in the dynamic table is a symbol another
# plug-in in the same host process can be merged with.
for pair in "rations.so lv2_descriptor" "rations_ui.so lv2ui_descriptor"; do
    set -- $pair
    so="$1"; want="$2"
    got=$(nm -D --defined-only "$bundle/$so" 2>/dev/null | awk '$2 == "T" { print $3 }')
    if [ "$got" != "$want" ]; then
        bad "$so exports [$(echo $got)] rather than exactly $want"
    else
        printf '  %-16s exports %s and nothing else\n' "$so" "$want"
    fi
done

# The DSP half must not drag the editor's libraries in: it draws nothing, and a headless host
# should be able to run it with no X server present at all.
if ldd "$bundle/rations.so" 2>/dev/null | grep -qE 'libX11|libcairo'; then
    bad "rations.so links X11 or cairo; the DSP half draws nothing and must not need them"
else
    printf '  %-16s links neither X11 nor cairo\n' "rations.so"
fi

# --- 1. install -------------------------------------------------------------------------------
if [ "$do_install" -eq 1 ]; then
    note "install"
    mkdir -p "$HOME/.lv2" || exit 1
    rm -rf "$installed"
    cp -r "$bundle" "$installed" || exit 1
    echo "  $installed"
    for shadow in /usr/local/lib/lv2/rations.lv2 /usr/lib/lv2/rations.lv2; do
        [ -e "$shadow" ] && bad "a copy at $shadow will shadow the one under test; remove it"
    done
fi

# --- 2. lilv ----------------------------------------------------------------------------------
note "what a host sees, and whether it runs"
check="$build/rations_lv2check"
if [ ! -x "$check" ]; then
    bad "rations_lv2check was not built (lilv's development files are what it needs)"
else
    args=("$installed")
    [ -n "$captures" ] && args+=(--captures "$captures")
    "$check" "${args[@]}" || fail=1
    [ -n "$captures" ] || echo "  (no --captures: the end-to-end bank load was skipped)"
fi

# --- 3. sord_validate -------------------------------------------------------------------------
note "RDF schema"
sordv="${RATIONS_SORD_VALIDATE:-$(command -v sord_validate 2>/dev/null || true)}"
if [ -z "$sordv" ]; then
    echo "  skip  sord_validate is not installed; build it from"
    echo "        ~/third_party/source-lv2/sord (meson setup build -Dtools=enabled)"
    echo "        and re-run, or set \$RATIONS_SORD_VALIDATE"
else
    # Every LV2 vocabulary the bundle names has to be loaded too, or every predicate in it is
    # "undefined". The distribution's own spec bundles carry warnings of their own, which is why
    # only the lines naming THIS plug-in are counted.
    specs=$(ls -d /usr/lib/lv2/*.lv2/*.ttl 2>/dev/null)
    out=$("$sordv" "$installed"/*.ttl $specs 2>&1)
    mine=$(printf '%s\n' "$out" | grep -c 'NAMp-rations')
    if [ "$mine" -ne 0 ]; then
        bad "sord_validate reports $mine line(s) against this bundle:"
        printf '%s\n' "$out" | grep -A3 'NAMp-rations' >&2
    else
        echo "  clean  (nothing reported against <https://github.com/rations/NAMp-rations>)"
    fi
fi

# ----------------------------------------------------------------------------------------------
note "result"
if [ "$fail" -ne 0 ]; then
    echo "lv2-gate: FAILED" >&2
    exit 1
fi
echo "lv2-gate: PASSED"
echo
echo "What this gate cannot say, and what closes it: a real host. XEmbed mapping behaviour"
echo "differs between them, and a UI that works in one can be invisible in another. Load the"
echo "bundle in at least two — NAMp's own rack host will do one of them:"
echo "  ~/NAMp/build/namp-standalone --pre https://github.com/rations/NAMp-rations --editors"
exit 0
