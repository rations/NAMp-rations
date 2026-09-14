// The platform seam: which native window the standalone's windows are built on.
//
// Four things in this directory own a window — the top-level the amp's editor is embedded in, the
// rack strip that sits under it, the window a hosted plug-in's editor is given, and the generic
// parameter panel for a plug-in that has no editor of its own. Before the Windows port each of them
// opened an X window itself and read XEvents in its own handler, which meant each of them mixed two
// entirely different kinds of code: which call creates a window, and what a click at (412, 96)
// MEANS.
//
// The second kind is the same on every platform and is most of the code. The first kind is none of
// it. So this seam takes the first kind, and the four classes above it keep the second and became
// platform-neutral — which is why supporting Windows added ONE window implementation rather than
// four, and why every future feature is still written once.
//
// The verb set both implementations provide:
//
//   createTopLevel / createChild / destroy      lifetime; both created hidden, shown by show()
//   handle / systemWindow                       the native id, and what IPlugView::attached wants
//   setEventCallback                            WindowEvents, translated from whatever arrived
//   setTitle / setClassHint / setSizeHints      what the window manager is told
//   show / raise / resize / moveResize          geometry
//   flush / sync                                make pending drawing real
//   createSurfaces / resizeSurfaces / destroySurfaces
//   drawingSurface / present                    compose offscreen, then one blit
//   setKeyboardFocus                            taken only around an open text field; never grabs
//   childGeometry / resizeChild                 a hosted editor's own window inside ours
//
// THE TWO ARE NOT RELATED BY INHERITANCE, which is deliberate and is the same choice the plug-in
// side of this project made for its IPlugView bases. Only one of the pair is ever compiled, so a
// virtual base would buy nothing at run time and would still not stop the two drifting; what stops
// that is that BOTH ARE BUILT BY THE PHASE GATE, on this machine, so the platform nobody is looking
// at today still has to compile.
//
// MOUSE COORDINATES ARE PHYSICAL PIXELS on both, and that is the SDK's own rule rather than a
// convenience: pluginterfaces/gui/iplugview.h states that for kPlatformTypeHWND and
// kPlatformTypeX11EmbedWindowID "the coordinates are expressed in physical units (pixels)", and
// only macOS differs by being logical. Dividing by the window's scale is the caller's job, because
// the scale belongs to the drawing code — see rackwindow.h on the one logical canvas the editor and
// the strip share.
//
// See eventloop.h for this seam's companion, which does the same job for the run loop.

#pragma once

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cstdint>

#if SMTG_OS_WINDOWS
#include "win32window.h"
#else
#include "x11window.h"
#endif

namespace Rations
{

#if SMTG_OS_WINDOWS
using NativeWindow = Win32Window;
// The window type a hosted plug-in's editor is attached to on this platform. Taken from the SDK's
// own constant rather than re-spelling the string, and `inline const` rather than constexpr because
// that is what the SDK declares them as — see pluginterfaces/gui/iplugview.h.
inline const Steinberg::FIDString kNativePlatformType = Steinberg::kPlatformTypeHWND;
#else
using NativeWindow = X11Window;
inline const Steinberg::FIDString kNativePlatformType = Steinberg::kPlatformTypeX11EmbedWindowID;
#endif

// The native window id, opaque above this seam: passed back in as a parent, handed to a hosted
// plug-in as the window to attach to, and compared against zero. Nothing else.
using NativeHandle = NativeWindow::Handle;

//------------------------------------------------------------------------
// A window id as the plain integer the host layer carries it as, and back.
//
// The host layer's editor-open request takes a uintptr_t precisely so that it needs neither a
// windowing system nor the SDK, and these are the two functions that cross that boundary. They are
// here rather than at the call site because THE CAST IS NOT THE SAME ONE on both platforms and
// picking the wrong one is a compile error on one and a truncation on the other: an X11 Window is
// an unsigned long, so it WIDENS, while an HWND is a pointer, so it must be reinterpreted. A single
// spelling that satisfied both would have to be a reinterpret_cast of an integer, which is
// ill-formed.
#if SMTG_OS_WINDOWS
inline uintptr_t nativeHandleToInt(NativeHandle handle)
{
    return reinterpret_cast<uintptr_t>(handle);
}
inline NativeHandle nativeHandleFromInt(uintptr_t value)
{
    return reinterpret_cast<NativeHandle>(value);
}
#else
inline uintptr_t nativeHandleToInt(NativeHandle handle)
{
    return static_cast<uintptr_t>(handle);
}
inline NativeHandle nativeHandleFromInt(uintptr_t value)
{
    return static_cast<NativeHandle>(value);
}
#endif

} // namespace Rations
