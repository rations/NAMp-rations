// WindowEvent — what a native window reports, as a value rather than a platform detail.
//
// The standalone's windows are created and torn down by platform code, but everything that decides
// what an event MEANS — which control was hit, whether a drag moved a slider, whether a key belongs
// to an open text field — is the same on every platform and is written once. This type is the seam
// between the two: a native window translates whatever its windowing system reported into one of
// these, and the window classes above it never learn which system that was.
//
// THIS IS THE SAME CONTRACT THE PLUG-IN'S OWN EDITOR ALREADY USES, and it is spelled the same way
// deliberately. The editor is written against one hook set — onMouseDown, onKeyDown and the rest —
// that each platform base implements over its own window, and the hooks take an ASCII character, a
// VirtualKeyCodes value and a KeyModifier mask precisely so that the handler cannot tell which
// platform it is on. The standalone's windows had no such seam because they had only one platform
// to serve; this file gives them the one the editor has had all along, so the two halves of the
// product answer the same question the same way.
//
// MOUSE COORDINATES ARE PHYSICAL PIXELS, and dividing by the window's scale is the caller's job.
// The rack strip and the editor are one logical canvas at one scale (see rackwindow.h), and the
// scale belongs to the drawing code rather than to the window; a native window that divided would
// have to be told a factor it has no other use for.
//
// BUTTON NUMBERS ARE X'S — 1 left, 2 middle, 3 right — on every platform. The editor's Win32 base
// already made that choice for the same reason: it lets the hit-testing above be identical, and one
// translation table in the platform layer is cheaper than two spellings of every comparison above
// it. Wheel notches arrive as their own kind rather than as X's buttons 4 and 5, because Windows
// reports a signed delta and flattening that into two synthetic buttons would throw away the
// magnitude.

#pragma once

#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/base/ftypes.h"

namespace Rations
{

//------------------------------------------------------------------------
struct WindowEvent {
    enum class Kind {
        // The window's contents were damaged and must be painted again. Nothing else changed.
        //
        // Not spelled Expose, which is the X name for it: X11/X.h defines that as a macro, so an
        // enumerator of that name does not compile on the platform the word comes from.
        Redraw,
        // The window is now `width` x `height` PHYSICAL pixels.
        Resize,
        // The user asked to close the window — the window manager's close button, or Alt-F4. The
        // window is still alive; whoever handles this decides whether it stops existing.
        Close,
        // `x`, `y` in physical pixels; `button` in X numbering.
        MouseDown,
        MouseUp,
        MouseMove,
        // The pointer left the window, so any hover highlight must be cleared. Carries no position:
        // by the time this arrives the pointer is somewhere this window cannot describe.
        MouseLeave,
        // `delta` is notches, positive away from the user; `x`, `y` where the pointer was.
        Wheel,
        // `character` is ASCII and 0 when the key produced none; `virtualKey` is a VirtualKeyCodes
        // value and 0 when the key is not one of them; `modifiers` is a KeyModifier mask.
        Key,
        // The keyboard focus was taken away by the window manager. Whoever thought it held the
        // focus no longer does, and must not hand back a focus it has already lost.
        FocusLost,
    };

    Kind kind = Kind::Redraw;

    int width = 0;
    int height = 0;

    float x = 0.0f;
    float y = 0.0f;
    int button = 0;
    int delta = 0;

    Steinberg::char16 character = 0;
    Steinberg::int16 virtualKey = 0;
    Steinberg::int16 modifiers = 0;
};

// X's button numbering, used on every platform per the note at the top of this file.
constexpr int kButtonLeft = 1;
constexpr int kButtonMiddle = 2;
constexpr int kButtonRight = 3;

} // namespace Rations
