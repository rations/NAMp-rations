// PluginWindow — one top-level window holding one hosted plug-in's editor.
//
// Every hosted plug-in gets its own window rather than sharing one: a shared window would have to
// tear an editor down and build another every time the user switched plug-in, and a plug-in editor
// is the single most expensive and most fragile thing to create and destroy repeatedly. Separate
// windows also let the window manager do the work — stacking, iconifying, remembering positions —
// instead of this host reimplementing it.
//
// LIFETIME. close() is not destruction. It tears the editor and the window down and leaves this
// object alive and reusable, so open()/close() can cycle any number of times. That is not just
// tidiness: close() is reached from inside the window's own event callback when the user clicks the
// title bar's close button, and destroying the object there would pull the ground out from under
// the callback that is running. The object is destroyed later, by whoever owns it.
//
// TEARDOWN ORDER IS LOAD-BEARING and is the reason this is a class rather than two loose functions:
//
//   1. backend.editorClose()  — the view unregisters its own run-loop event handler and timers from
//                               inside IPlugView::removed(), so this must happen while the run loop
//                               and the frame are both still alive;
//   2. stop dispatching       — no further events reach a window that is going away;
//   3. destroy the window     — only now, once nothing can still be drawing into it.
//
// Steps 2 and 3 are both inside NativeWindow::destroy(), in that order and for this reason; step 1
// is this class's and has to happen before it is called.
//
// Getting 1 and 3 the wrong way round leaves a plug-in's timer firing against a destroyed window,
// which is a crash inside the plug-in with our stack nowhere in the backtrace.
//
// NOT EVERY EDITOR NEEDS A WINDOW FROM US. An LV2 ui:showInterface UI opens and owns its own
// top-level window; the host's whole job there is to call show(), drive idle() and call hide().
// This class therefore has two modes, and `mOwnsWindow` is which one — creating a window for a
// showInterface UI would leave an empty stray frame on screen next to the real editor. That mode is
// reachable on Linux only, since LV2 is not hosted on Windows at all.
//
// The editor-kind enumerator in the host layer is spelled NoEditor rather than None because Xlib
// defines None, True and False as macros, and this window's Linux half is built against it.

#pragma once

#include "eventloop.h"
#include "nativewindow.h"
#include "plugframe.h"

#include "host/pluginbackend.h"

#include <cstdint>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class PluginWindow
{
public:
    PluginWindow(EventLoop &loop, NAMp::host::PluginBackend &backend);
    ~PluginWindow();

    PluginWindow(const PluginWindow &) = delete;
    PluginWindow &operator=(const PluginWindow &) = delete;

    // Creates the window, opens the editor into it and maps it. Reopening an already-open window
    // just raises it. False means the plug-in has no embeddable editor — the caller falls back to
    // the generic panel, which is P4's job — or that there is no display.
    bool open();
    void close();

    bool isOpen() const
    {
        return mOpen;
    }

    // Called from the UI timer: drives formats that need an idle callback and applies any resize
    // the editor latched from a thread we do not control.
    void idle();

    NAMp::host::PluginBackend &backend() const
    {
        return mBackend;
    }

private:
    void onEvent(const WindowEvent &event);
    // The IPlugFrame half of the SDK resize contract: resize the window, then onSize() in the same
    // callstack.
    bool onViewResize(Steinberg::IPlugView *view, Steinberg::ViewRect *rect);
    // Ask the editor what it will accept nearest to w x h.
    void constrain(int32_t &w, int32_t &h) const;
    void applySizeHints(int32_t w, int32_t h);

    NAMp::host::PluginBackend &mBackend;
    PlugFrame mFrame;

    NativeWindow mWindow;
    // The window the plug-in created INSIDE ours (LV2 ui:X11UI). suil hands it back as the widget;
    // it is the plug-in's to draw and ours to keep the same size as its parent, because a child
    // window does not follow its parent's size on its own. VST3 leaves this zero: an IPlugView
    // manages its own child through onSize().
    NativeHandle mChild = {};
    // False when the editor owns its own top-level window; see the note at the top of this file.
    bool mOwnsWindow = true;
    bool mOpen = false;
    int32_t mWidth = 0;
    int32_t mHeight = 0;
    bool mResizable = false;
    std::string mTitle;
};

} // namespace Rations
