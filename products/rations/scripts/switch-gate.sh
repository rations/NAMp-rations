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
#   scripts/switch-gate.sh --settle 10              # longer for the card after a jackd restart
#   scripts/switch-gate.sh --pedals                 # the same three states with all five pedals on
#   scripts/switch-gate.sh --pedals chorus,delay    # ... or with only the ones named
#
# --pedals engages the whole pedalboard for the run. It is OFF by default and that is deliberate:
# what this gate protects is the channel switch's budget, a disengaged pedal is skipped outright
# rather than processed and mixed out, so the default run measures the switch and nothing else and
# stays comparable with every figure recorded for it before the pedalboard existed. Turning it on
# asks the other question - whether the board and the switch fit in one period together - and that
# is the question scripts/pedal-gate.sh exists to ask.
#
# NOTHING ELSE MAY BE RUNNING while this measures. It restarts jackd, and a build, another gate,
# or a second copy of this script will disturb the server underneath it and produce numbers that
# look like measurements and are not.
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
pedals=
# Seconds to leave the server alone after it reports itself available, before anything is
# measured through it. Not a guess at how long jackd takes to start - jackd_wait_ready above
# waits for that properly - but a margin over the card being reopened underneath it.
settle=5

while [ "$#" -gt 0 ]; do
    case "$1" in
        --periods)  periods="$2"; shift 2 ;;
        --budgets)  budgets="$2"; shift 2 ;;
        --seconds)  seconds="$2"; shift 2 ;;
        --passes)   passes="$2"; shift 2 ;;
        --settle)   settle="$2"; shift 2 ;;
        --pedals)
            # An optional argument: bare --pedals means all five, and a following word that is not
            # another option names the ones to engage. Naming them is how "the board does not fit"
            # becomes "this pedal does not fit", which is the only form of that finding anyone can
            # act on.
            case "${2:-}" in
                ""|--*) pedals="--pedals all"; shift ;;
                *)      pedals="--pedals $2"; shift 2 ;;
            esac ;;
        -h|--help)  sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "switch-gate: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

bundle="$build/VST3/Release/NAMp-rations.vst3"
jackcheck="$build/rations_jackcheck"
for f in "$bundle" "$jackcheck"; do
    [ -e "$f" ] || { echo "switch-gate: $f is missing — build first" >&2; exit 1; }
done

# The four banks. The plug-in ships none — every bank is a folder the user loads — so this gate has
# to say where they are, and it matters more here than in most places: a switch to an empty channel
# is HELD rather than performed, so a run against missing captures would report switch latencies
# that measured nothing at all and xrun counts of zero for the same reason.
captures="${RATIONS_TEST_CAPTURES:-$root/captures}"
for chan in Clean Crunch OD1 OD2; do
    [ -d "$captures/$chan" ] || {
        echo "switch-gate: $captures/$chan is missing" >&2
        echo "  set RATIONS_TEST_CAPTURES to a directory holding Clean, Crunch, OD1 and OD2" >&2
        exit 1
    }
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

# jackd does not come up in a fixed amount of time, and this script used to assume it did: it
# slept one second for the old server to die and three for the new one to appear, then carried on
# whatever had actually happened. When three seconds was not enough the header printed
# "########## Hz,  frames ##########" from two empty substitutions, every state failed because
# jack_client_open had no server to open, and the summary blamed kSwitchModelBudget for it - which
# is a measurement that looks exactly like a real one and is not. So nothing here sleeps for a
# fixed time any more: it waits for the condition and fails loudly if the condition never arrives.
jackd_wait_gone() {  # the old server, actually gone rather than merely signalled
    for _ in $(seq 1 100); do
        pgrep -x jackd >/dev/null 2>&1 || return 0
        sleep 0.1
    done
    return 1
}

jackd_wait_ready() {  # $1 = the period it must be running at
    # jack_wait -w returns when the server is available to clients. Deliberately not -q, which
    # blocks indefinitely when there is no server to wait on.
    jack_wait -w -t 20 >/dev/null 2>&1 || return 1
    # Available is not the same as running at the size that was asked for, and the whole point of
    # restarting it was the size.
    for _ in $(seq 1 100); do
        [ "$(jack_bufsize 2>/dev/null)" = "$1" ] && return 0
        sleep 0.1
    done
    return 1
}

start_jackd() {  # $1 = period
    # pkill -x, never pkill -f: the full-command-line form also matches this script, because the
    # script's own text contains the process name.
    pkill -x jackd 2>/dev/null
    jackd_wait_gone || { echo "switch-gate: the old jackd will not exit" >&2; exit 1; }
    # shellcheck disable=SC2086
    setsid $(echo "$jackd_cmd" | sed "s/-p [0-9]*/-p $1/") >/dev/null 2>&1 &
    jackd_wait_ready "$1" || {
        echo "switch-gate: jackd did not come up at $1 frames" >&2
        exit 1
    }
    # And settled is not the same as available either: the card has just been reopened, and the
    # first seconds after that are not what this script is here to measure. jackcheck waits again
    # on its own account before it activates its client - it has banks to build - so this is the
    # server's share of the wait and not a duplicate of that one.
    sleep "$settle"
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
noresult=0   # runs that produced no verdict at all: a broken harness, not a failing plug-in
inside=0     # failures where the worst block was still inside the deadline

run_states() {  # $1 = label prefix
    for state in "--stomp-ms 1200|switches completing" \
                 "--stomp-ms 250|sustained catch-up (stomping faster than a switch completes)" \
                 "--stomp-ms 250 --sweep-gain|catch-up under a knob turn (must be held)"; do
        args="${state%%|*}"
        name="${state##*|}"
        printf '  %-58s ' "$name"
        # THE PERIOD IS RE-CHECKED BEFORE EVERY STATE, not just once per buffer size, because this
        # script prints the size it ASKED for as a heading and the deadline it MEASURED in each
        # row, and nothing used to make the two agree. A jackd restarted underneath a run - by a
        # session manager, by a GUI, or by another copy of this script exiting late and firing its
        # own restore trap - silently produced rows reading "3.44 ms of a 5.33 ms period" under a
        # "128 frames" heading, which is a 256-frame measurement filed as a 128-frame one. That is
        # worse than a failure: it is a number that looks like the gate passing at a size it never
        # ran at. Observed, not hypothetical.
        now=$(jack_bufsize 2>/dev/null)
        if [ "$now" != "$period" ]; then
            printf 'server is at %s frames, not %s | NO RESULT\n' "${now:-?}" "$period"
            noresult=$((noresult + 1))
            continue
        fi
        # shellcheck disable=SC2086
        out=$("$jackcheck" "$bundle" --captures "$captures" --seconds "$seconds" $pedals $args 2>&1)
        block=$(echo "$out" | grep -o 'worst block .*' | sed 's/worst block *//')
        verdict=$(echo "$out" | grep -oE '^(PASSED|FAILED)')
        # No verdict at all means jackcheck never got far enough to have an opinion - no server,
        # no bundle, banks that never built. That is a broken run, not a plug-in that dropped
        # audio, and calling it the latter is how a budget gets lowered for no reason.
        [ -n "$verdict" ] || { verdict="NO RESULT"; noresult=$((noresult + 1)); }
        printf '%s | %s\n' "$block" "$verdict"
        if [ "$verdict" = "FAILED" ]; then
            failures=$((failures + 1))
            # Whether the audio thread actually overran is knowable, and the two causes want
            # opposite responses. Over the deadline is the budget's problem. Comfortably inside it
            # and still dropping audio is the machine's or the server's, and lowering the budget
            # would only make the plug-in worse while leaving the xruns exactly where they were.
            pct=$(echo "$block" | grep -oE '\(([0-9]+)%' | tr -d '(%')
            if [ -n "$pct" ] && [ "$pct" -lt 75 ]; then
                inside=$((inside + 1))
            fi
        fi
    done
}

for period in $periods; do
    [ "$(jack_bufsize 2>/dev/null)" = "$period" ] || start_jackd "$period"
    rate=$(jack_samplerate 2>/dev/null)
    frames=$(jack_bufsize 2>/dev/null)
    [ -n "$rate" ] && [ -n "$frames" ] || {
        echo "switch-gate: no usable jack server; refusing to report a measurement" >&2
        exit 1
    }
    echo "########## $rate Hz, $frames frames${pedals:+, pedals: ${pedals#--pedals }} ##########"
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
if [ "$noresult" -gt 0 ]; then
    echo "INCONCLUSIVE - $noresult state(s) produced no measurement at all. Nothing here is"
    echo "evidence about the plug-in; find out why jackcheck could not run and try again."
    exit 1
fi
if [ "$failures" -eq 0 ]; then
    echo "PASSED - zero xruns in every state at every buffer size tested"
elif [ "$inside" -eq "$failures" ]; then
    echo "FAILED - $failures state(s) dropped audio, but the audio thread met its deadline in"
    echo "every one of them. That is the server or the machine, not kSwitchModelBudget: lowering"
    echo "the budget would slow the switch down and leave the xruns where they are. Check what"
    echo "else is running, and that jackd has been up long enough, before touching the constant."
else
    echo "FAILED - $failures state(s) dropped audio, $((failures - inside)) of them with the audio"
    echo "thread over its deadline; lower kSwitchModelBudget and run again"
fi
exit "$failures"
