#!/usr/bin/env bash
# The cabinet page's phase gate: D10's two promises, over real cabinet IRs.
#
# rations_ircheck needs a pair of impulse responses to say anything, and cabinet IRs are not in
# this repository — they are third-party files with their own licences, and the plug-in ships no
# IRs at all (unlike the captures, which are the author's own). So the library lives outside the
# tree and this script points the gate at it.
#
# The eight pairs below are not arbitrary. They span the two regimes the blend has to handle and
# which the correct curve differs between: mic positions on ONE cabinet, which are strongly
# correlated, and crosses between two DIFFERENT cabinets, which are not. A gate run on only one
# regime would pass with a curve that is unusable in the other, which is exactly the failure this
# whole measurement exists to prevent.
#
#   RATIONS_IR_DIR   root of the IR library (default: $HOME/Impulse-Responses)
#
# Everything after the options is passed through to rations_ircheck, so a one-off can override the
# tolerance or the render length without editing this file.
set -uo pipefail

root="${RATIONS_IR_DIR:-$HOME/Impulse-Responses}"
build="${RATIONS_BUILD_DIR:-build}"
bundle="$build/VST3/Release/Rations.vst3"
tool="$build/rations_ircheck"

V="$root/Celestion Vintage 30 - 2002 Mesa Boogie Traditional 4x12 - SM57"
B="$root/1970 Fender Bassman 2x15 Cabinet with Original CTS Speakers IR Files"

for p in "$tool" "$bundle" "$V" "$B"; do
    if [[ ! -e "$p" ]]; then
        echo "ir-gate: missing $p" >&2
        [[ "$p" == "$V" || "$p" == "$B" ]] &&
            echo "  set RATIONS_IR_DIR to a library holding these two cabinets" >&2
        exit 2
    fi
done

args=()
add(){ args+=(--pair "$1" "$2"); }

# --- one cabinet, the mic moved: strongly correlated, and what the blend is really for ---------
add "$V/V30 LL 4FB 4x12 SM57 0.00in 0.0in 7603.wav"  "$V/V30 LL 4FB 4x12 SM57 0.25in 0.0in 7603.wav"
add "$V/V30 LL 4FB 4x12 SM57 0.00in 0.0in 7603.wav"  "$V/V30 UR 4FB 4x12 SM57 2.50in 0.0in 7603.wav"
add "$V/V30 LR 4FB 4x12 SM57 0.50in 0.0in VP28.wav"  "$V/V30 UL 4FB 4x12 SM57 1.25in 0.0in VP28.wav"
add "$B/1970 Bassman Cabinet CTS - 121 Lower - Cap Edge.wav" \
    "$B/1970 Bassman Cabinet CTS - 121 Upper - Cap.wav"
add "$B/1970 Bassman Cabinet CTS - 121 Lower - Cap.wav" \
    "$B/1970 Bassman Cabinet CTS - 121 Upper - Cone Edge.wav"

# --- two different cabinets: barely correlated, and where a linear mix digs a 2.5 dB hole ------
add "$V/V30 LL 4FB 4x12 SM57 0.00in 0.0in 7603.wav" \
    "$B/1970 Bassman Cabinet CTS - 121 Lower - Cap.wav"
add "$V/V30 UL 4FB 4x12 SM57 1.50in 0.0in 7603.wav" \
    "$B/1970 Bassman Cabinet CTS - 121 Lower - Cone.wav"
add "$B/1970 Bassman Cabinet CTS - 121 Upper - Cone Edge.wav" \
    "$V/V30 LR 4FB 4x12 SM57 0.75in 0.0in 7603.wav"

exec "$tool" "$bundle" "${args[@]}" "$@"
