#!/bin/bash
# pedal-gate.sh — the pedalboard's own gate: the offline proof, then live JACK with the whole
# board engaged.
#
# Two halves, and they answer two different questions, which is why they are one script rather
# than two habits:
#
#   1. rations_pedalcheck — does each pedal DO what its panel says? Every knob's endpoints against
#      the circuit or the published theory it was built from, measured through the built bundle:
#      the Boost against Yeh's TS-9 equations, the Chorus and Flanger against PASP's delay-effect
#      chapters, the Delay's time against the phase at one FFT bin, the Reverb's T60 against the
#      Decay knob. It needs no JACK server and disturbs nothing.
#
#   2. switch-gate.sh --pedals — do they FIT? The channel switch's worst block is already about
#      two thirds of a 128-frame period, and five pedals have to live in what is left. This runs
#      the same three states switch-gate always runs, with all five engaged, and asserts the same
#      thing it always asserts: zero xruns.
#
# The second half restarts jackd if the server is not already at the size it wants, so it carries
# switch-gate's own warning: NOTHING ELSE MAY BE RUNNING while it measures, and a failure from it
# is re-run on an idle machine before it is believed. Pass --periods to name the sizes; on a
# machine whose jackd is managed elsewhere, name only the size it is already at and the server is
# never touched.
#
# Usage:
#   scripts/pedal-gate.sh                       # both halves, 128 and 256
#   scripts/pedal-gate.sh --periods "128"       # live half at one size, no jackd restart
#   scripts/pedal-gate.sh --offline-only        # half one, on any machine, with no server at all
#   scripts/pedal-gate.sh --passes 4            # repeat the live states, for a marginal result

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build="${RATIONS_BUILD_DIR:-$root/build}"

offline_only=0
gate_args=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --offline-only) offline_only=1; shift ;;
        --periods|--passes|--seconds|--settle) gate_args="$gate_args $1 $2"; shift 2 ;;
        -h|--help) sed -n '2,29p' "$0"; exit 0 ;;
        *) echo "pedal-gate: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

bundle="$build/VST3/Release/NAMp-rations.vst3"
pedalcheck="$build/rations_pedalcheck"
for f in "$bundle" "$pedalcheck"; do
    [ -e "$f" ] || { echo "pedal-gate: $f is missing — build first" >&2; exit 1; }
done

failures=0

echo "########## the pedals, offline ##########"
if "$pedalcheck" "$bundle"; then
    echo "pedal-gate: rations_pedalcheck PASSED"
else
    echo "pedal-gate: rations_pedalcheck FAILED" >&2
    failures=$((failures + 1))
fi

if [ "$offline_only" -eq 1 ]; then
    echo
    [ "$failures" -eq 0 ] && echo "PASSED - offline only; the live half was not run" \
                          || echo "FAILED - see above"
    exit "$failures"
fi

echo
echo "########## the board and the switch together, live ##########"
# shellcheck disable=SC2086
if "$root/scripts/switch-gate.sh" --pedals all $gate_args; then
    echo "pedal-gate: the live half PASSED"
else
    echo "pedal-gate: the live half FAILED" >&2
    failures=$((failures + 1))
fi

echo
if [ "$failures" -eq 0 ]; then
    echo "PASSED - every pedal measures as its panel claims, and the whole board fits beside the"
    echo "channel switch with zero xruns"
else
    echo "FAILED - $failures half(s) of this gate did not pass; see above"
fi
exit "$failures"
