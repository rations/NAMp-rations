#!/bin/bash
# editor-drive.sh — open the plug-in's editor in the SDK's editorhost and drive it with real
# pointer input, so that what a control does when a human uses it can be checked rather than
# assumed.
#
# The editor is painted by hand into the host's own window and has no accessibility surface, no
# scripting hook and no test seam. The only way to find out whether clicking a bat switch changes
# a channel, or whether dragging a dial draws the readout where the art audit says it will, is to
# put a pointer on it and photograph the result. That is what this does.
#
# Three things here were expensive to learn and are the reason this is a script and not a one-liner:
#
#   * FIND THE WINDOW BY SIZE, never by title. Matching the plug-in's name against the window tree
#     also matches the terminal you are running from, because the project directory is in its
#     title — which silently hands back a shell window and photographs that instead.
#   * THE FIRST SYNTHETIC CLICK AFTER THE WINDOW MAPS IS SWALLOWED. Something in the embedding
#     handshake eats it. A control that "does not respond" to the first click responds to the
#     second, so a warm-up click is burned before anything is measured. Without it a working
#     control reads as broken and you go looking for a bug that is not there.
#   * THIS HOST DELIVERS NO KEYBOARD INPUT, so `type` and `key` below will appear to do nothing
#     and that is not a plug-in bug. The SDK routes keys to a plug-in through
#     IPlugView::onKeyDown and forbids a view from taking them off its own window; the SDK's own
#     editorhost selects KeyPressMask on the window and then never calls onKeyDown at all — there
#     is not one occurrence of it anywhere in its source. So a null result from `type` here says
#     only that this rig cannot ask the question. The commands are kept because the question is
#     real and a host that DOES route keys will answer it, and because the alternative is somebody
#     later concluding from silence that the field is broken. The channel-rename field is
#     deliberately the second way to name a channel for this exact reason: the first is the
#     basename of whatever the channel loaded, which needs no keyboard and works everywhere.
#
#   * A DRAG HAS TO TAKE REAL TIME. The editor coalesces motion and repaints on a timer, and the
#     drag readout is only drawn while the button is held, so a screenshot has to be taken from a
#     second process while the drag is still in flight.
#
# The window size doubles as the page identity, since every page brings its own: head 1133x403,
# cabinet 640x460, pedalboard 480x220, settings 560x280 (at scale 1.0). So --size is both how the
# window is found and how you say which page you expect to be looking at.
#
# Usage:
#   scripts/editor-drive.sh shot out.png
#   scripts/editor-drive.sh click 251 226 shot out.png
#   scripts/editor-drive.sh drag 251 226 0 -70 shot out.png     # photographed mid-drag
#   scripts/editor-drive.sh click 140 110 type "JCM800" key Return shot out.png
#   scripts/editor-drive.sh --size 640x460 click 320 200 shot cab.png
#
# Commands are applied in order, so several clicks and shots can be chained in one session. The
# host is started once and torn down at the end.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build="${RATIONS_BUILD_DIR:-$root/build}"
export DISPLAY="${DISPLAY:-:0}"

size="1133x403"
settle=8

while [ "$#" -gt 0 ]; do
    case "$1" in
        --size)   size="$2"; shift 2 ;;
        --settle) settle="$2"; shift 2 ;;
        -h|--help) sed -n '2,44p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
[ "$#" -gt 0 ] || { echo "editor-drive: nothing to do; see --help" >&2; exit 2; }

bundle="$build/VST3/Release/Rations.vst3"
host="$build/bin/Release/editorhost"
for f in "$bundle" "$host"; do
    [ -e "$f" ] || { echo "editor-drive: $f is missing — build first" >&2; exit 1; }
done
for tool in xwininfo import; do
    command -v "$tool" >/dev/null || { echo "editor-drive: $tool is not installed" >&2; exit 1; }
done

work=$(mktemp -d)
poke="$work/editor-drive"
gcc -O1 -Wall -o "$poke" "$root/scripts/editor-drive.c" -lX11 -lXtst || exit 1

cleanup() {
    pkill -x editorhost 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

"$host" "$bundle" >"$work/host.log" 2>&1 &
sleep "$settle"   # the workers build the banks, and the editor polls for their capture names

# One pass over the whole window tree, parsed for the geometry directly. Asking xwininfo about
# each window id in turn instead takes minutes on a populated desktop.
window=$(xwininfo -root -tree 2>/dev/null | awk -v want="$size" '
    match($0, /0x[0-9a-f]+/) {
        id = substr($0, RSTART, RLENGTH)
        for (i = 1; i <= NF; ++i)
            if ($i ~ ("^" want "\\+")) { print id; exit }
    }')
if [ -z "$window" ]; then
    echo "editor-drive: no $size window appeared" >&2
    tail -5 "$work/host.log" >&2
    exit 1
fi
echo "editor window $window ($size)"

# Burn the swallowed first click somewhere inert — the faceplate margin, well clear of every hit
# box the art audit knows about.
"$poke" click "$window" 8 8
sleep 1

while [ "$#" -gt 0 ]; do
    case "$1" in
        click)
            "$poke" click "$window" "$2" "$3"
            echo "  click $2,$3"
            sleep 1
            shift 3 ;;
        drag)
            # Photograph from here while the drag runs in the background: the readout exists only
            # for as long as the button is down.
            "$poke" drag "$window" "$2" "$3" "$4" "$5" &
            drag_pid=$!
            sleep 1
            if [ "${6:-}" = "shot" ]; then
                import -window "$window" "$7"
                echo "  drag $2,$3 by $4,$5 -> $7 (mid-drag)"
                wait "$drag_pid" 2>/dev/null
                shift 7
            else
                echo "  drag $2,$3 by $4,$5"
                wait "$drag_pid" 2>/dev/null
                shift 5
            fi ;;
        type)
            "$poke" type "$window" "$2"
            echo "  type '$2'"
            sleep 1
            shift 2 ;;
        key)
            "$poke" key "$window" "$2"
            echo "  key $2"
            sleep 1
            shift 2 ;;
        shot)
            import -window "$window" "$2"
            echo "  shot -> $2"
            shift 2 ;;
        *) echo "editor-drive: unknown command '$1'" >&2; exit 2 ;;
    esac
done
