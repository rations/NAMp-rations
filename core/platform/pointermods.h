// Modifier keys held during a pointer event, as the editor sees them on every platform.
//
// Each window class translates its own platform's state into this mask — X's ShiftMask and
// ControlMask, Win32's MK_SHIFT and MK_CONTROL, AppKit's Shift and Command — so the editor's
// gestures are written once and cannot learn which platform they are on. Only the two keys a
// fine-adjust gesture needs are carried: the original plug-in's knobs treat either as "fine"
// (iPlug2's IKnobControlBase::IsFineControl, `mod.C || mod.S`).
//
// kPointerControl is the COMMAND key on macOS, not Control, and that is upstream's mapping rather
// than a choice made here: iPlug2's IGraphicsMac_view.mm builds its IMouseMod with `C` taken from
// NSCommandKeyMask. It is also the only mapping that works there, because a Control-click is a
// right-click on that platform (see macplugview.mm) and so cannot also be a fine left-drag.

#pragma once

namespace Steinberg
{

enum PointerMod : unsigned {
    kPointerShift = 1u << 0,
    kPointerControl = 1u << 1,
};

} // namespace Steinberg
