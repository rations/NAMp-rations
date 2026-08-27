#!/bin/bash
# switch-gate.sh — the live gate for the channel switch, and the harness that settles
# engine::kSwitchModelBudget.
#
# engineconfig.h says of that constant: "Raising this is not a decision that can be made by
# argument: re-run the gate." This is the gate. It runs the BUILT BUNDLE as a real JACK client
# through tools/rations_jackcheck, in the three states that cost different amounts:
#
#   switches completing        stomps spaced further apart than a switch takes
#   sustained catch-up         stomps four times a second, so the catch-up never finishes
#   catch-up under a knob turn stomping while a gain dial sweeps — two models already bound, so
#                              the rack must HOLD the switch rather than overrun the deadline
#
# The third is the one that matters and the one that is easy to forget: a budget that passes the
# first two and fails the third is a budget that drops audio the first time somebody rides the
# gain knob and changes channel.
#
# NOT CONNECTED TO PLAYBACK. What is being measured is whether the audio thread meets its
# deadline, and a WaveNet does the same arithmetic whatever the input is, so the cost is identical
# with nothing patched in and nothing reaches the monitors on a real interface.
#
# Buffer sizes: 128 and 256 by default. 64 is deliberately not in that list — it is not a size
# anyone records at, it crackles on this hardware before the plug-in is even loaded, and gating on
# it would hold the budget down for no benefit anyone would ever hear.
#
# Usage:
#   scripts/switch-gate.sh                          # verify the committed budget at 128 and 256
#   scripts/switch-gate.sh --passes 4               # repeat each state, for a marginal result
#   scripts/switch-gate.sh --periods "128"          # one buffer size
#   scripts/switch-gate.sh --budgets "2.4 2.5 2.6"  # SWEEP: edits engineconfig.h, see below
#
# --budgets rewrites the kSwitchModelBudget line in src/engineconfig.h and rebuilds once per
# value. The original value is put back when the script exits, however it exits, AND the tree is
# rebuilt from it — leaving the source saying one budget and the binary beside it built with
# another would make every later measurement a measurement of a number nobody chose. Nothing else
# in the tree is touched.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build="${RATIONS_BUILD_DIR:-$root/build}"
config="$root/src/engineconfig.h"

periods="128 256"
budgets=""
seconds=15
passes=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --periods)  periods="$2"; shift 2 ;;
        --budgets)  budgets="$2"; shift 2 ;;
        --seconds)  seconds="$2"; shift 2 ;;
        --passes)   passes="$2"; shift 2 ;;
        -h|--help)  sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "switch-gate: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

bundle="$build/VST3/Release/Rations.vst3"
jackcheck="$build/rations_jackcheck"
for f in "$bundle" "$jackcheck"; do
    [ -e "$f" ] || { echo "switch-gate: $f is missing — build first" >&2; exit 1; }
done

jackd_pid=$(pgrep -x jackd || true)
[ -n "$jackd_pid" ] || { echo "switch-gate: no jackd running" >&2; exit 1; }

# The server's own command line, read from /proc rather than written out here, so restoring it
# cannot drift away from the setup actually in use.
jackd_cmd=$(tr '\0' '\n' < "/proc/$jackd_pid/cmdline" | paste -sd' ')
original_period=$(jack_bufsize)
original_budget=$(sed -n 's/^inline constexpr double kSwitchModelBudget = \(.*\);/\1/p' "$config")

set_budget() {  # $1 = value
    sed -i "s/^inline constexpr double kSwitchModelBudget = .*/inline constexpr double kSwitchModelBudget = $1;/" "$config"
}

start_jackd() {  # $1 = period
    # pkill -x, never pkill -f: the full-command-line form also matches this script, because the
    # script's own text contains the process name.
    pkill -x jackd 2>/dev/null
    sleep 1
    # shellcheck disable=SC2086
    setsid $(echo "$jackd_cmd" | sed "s/-p [0-9]*/-p $1/") >/dev/null 2>&1 &
    sleep 3
}

restore() {
    if [ -n "$budgets" ] && [ -n "$original_budget" ]; then
        set_budget "$original_budget"
        # Rebuild as well, or the tree is left inconsistent in the worst way: the source says one
        # budget and the binary next to it was built with another, and every later measurement is
        # of a number nobody chose.
        cmake --build "$build" >/dev/null 2>&1
        echo "restored: $(grep -o 'kSwitchModelBudget = [0-9.]*' "$config") (rebuilt)"
    fi
    if [ "$(jack_bufsize 2>/dev/null)" != "$original_period" ]; then
        start_jackd "$original_period"
        echo "restored: jackd at $(jack_bufsize) frames"
    fi
}
trap restore EXIT

failures=0

run_states() {  # $1 = label prefix
    for state in "--stomp-ms 1200|switches completing" \
                 "--stomp-ms 250|sustained catch-up (stomping faster than a switch completes)" \
                 "--stomp-ms 250 --sweep-gain|catch-up under a knob turn (must be held)"; do
        args="${state%%|*}"
        name="${state##*|}"
        printf '  %-58s ' "$name"
        # shellcheck disable=SC2086
        out=$("$jackcheck" "$bundle" --seconds "$seconds" $args 2>&1)
        printf '%s | %s\n' \
            "$(echo "$out" | grep -o 'worst block .*' | sed 's/worst block *//')" \
            "$(echo "$out" | grep -oE '^(PASSED|FAILED)')"
        echo "$out" | grep -q '^PASSED' || failures=$((failures + 1))
    done
}

for period in $periods; do
    [ "$(jack_bufsize)" = "$period" ] || start_jackd "$period"
    echo "########## $(jack_samplerate) Hz, $(jack_bufsize) frames ##########"
    if [ -n "$budgets" ]; then
        for budget in $budgets; do
            set_budget "$budget"
            cmake --build "$build" >/dev/null 2>&1 || { echo "switch-gate: build failed" >&2; exit 1; }
            echo "--- budget $budget models ---"
            for pass in $(seq 1 "$passes"); do
                [ "$passes" -gt 1 ] && echo "  pass $pass"
                run_states
            done
        done
    else
        for pass in $(seq 1 "$passes"); do
            [ "$passes" -gt 1 ] && echo "--- pass $pass ---"
            run_states
        done
    fi
done

echo
if [ "$failures" -eq 0 ]; then
    echo "PASSED - zero xruns in every state at every buffer size tested"
else
    echo "FAILED - $failures state(s) dropped audio; lower kSwitchModelBudget and run again"
fi
exit "$failures"
