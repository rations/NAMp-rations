// The platform seam: which IPlugView base the editor is built on.
//
// RationsEditorView is written against ONE protected hook set — onAttached,
// onRemoved, onDraw(cairo_t*), onMouseDown/Up/Move/Leave/Wheel, onTick,
// isResizable, constrainSize, onResized, invalidate, isWindowOpen — and knows
// nothing about the windowing system underneath it. Each platform base
// implements that hook set over its own native window, and this header picks
// one. Nothing else in the plug-in includes a platform header.
//
// The two are genuinely different shapes, which is why this is a seam and not a
// set of #ifdefs inside one class:
//
//   X11PlugView   embeds an X window and owns NO thread. It has to borrow the
//                 host's Linux::IRunLoop, registering an IEventHandler on the X
//                 connection's file descriptor and an ITimerHandler for the
//                 repaint tick, because — as the SDK's own comment on
//                 Linux::IRunLoop puts it — "there's no global event run loop
//                 defined as on other platforms".
//
//   Win32PlugView creates a WS_CHILD HWND. Windows *is* that global run loop:
//                 the host pumps messages, our WndProc receives them, and the
//                 repaint tick is a plain SetTimer. There is no IRunLoop, no
//                 IEventHandler and no ITimerHandler on this platform at all.
//
// What both guarantee, and what the editor depends on, is the deferred-paint
// discipline: input handlers only ever set a dirty flag, and the actual draw
// happens on the timer tick. See either implementation's header for why.

#pragma once

#include "pluginterfaces/base/fplatform.h"

#if SMTG_OS_WINDOWS
#include "platform/win32plugview.h"
#else
#include "platform/x11plugview.h"
#endif

namespace Steinberg
{

#if SMTG_OS_WINDOWS
using NativePlugView = Win32PlugView;
#else
using NativePlugView = X11PlugView;
#endif

} // namespace Steinberg
