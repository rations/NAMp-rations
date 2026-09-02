// RunLoop — the host side of Linux::IRunLoop, for the Rations standalone.
//
// On Linux a VST3 plug-in owns no event loop; the host must provide one, and hand it to the
// plug-in through IPlugFrame::queryInterface. That is exactly what the editor asks for
// (src/platform/x11plugview.cpp, attachedToParent), so the standalone has to implement it to show
// its own editor at all.
//
// This object is therefore both:
//   * Linux::IRunLoop  — the plug-in registers file descriptors and timers;
//   * IPlugFrame       — what the host passes to IPlugView::setFrame, and the object the plug-in
//                        queries the run loop from.
//
// The loop itself is a select() over the host's X connection plus every descriptor the plug-in
// registered, with the timeout set from the nearest due timer. Registrations are allowed to change
// while a callback is running (a plug-in unregisters its handler from inside removed()), so the
// iteration works on copies.
//
// IT ALSO OWNS THE WINDOW'S SIZE, and that is the one part not shared with the sibling standalone
// this file was ported from. That editor is fixed-size, so its resizeView returns kResultFalse and
// its window carries PMinSize == PMaxSize. This one is neither: the editor is host-resizable, and
// every page change reshapes the window through IPlugFrame::resizeView — head, cabinet, pedalboard
// and settings are four different canvases. Refusing the request would leave all four page buttons
// dead-ended, so resizeView really resizes, and the window manager is told each page's own minimum
// rather than a fixed size.
//
// Reference: the SDK's own editorhost sample implements the same interfaces at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class RunLoop : public Steinberg::IPlugFrame, public Steinberg::Linux::IRunLoop
{
public:
    using XEventCallback = std::function<void(const XEvent &)>;

    explicit RunLoop(::Display *display);

    // Called for every X event on the host's own connection.
    void setXEventCallback(XEventCallback cb)
    {
        mXCallback = std::move(cb);
    }

    // The top-level window the editor was embedded into, and the view inside it. Set once, after
    // the view has attached: resizeView cannot do its job without both, and until it is called a
    // resize request is refused rather than acted on half-way.
    void setEmbedding(::Window window, Steinberg::IPlugView *view);

    // The window manager has resized the top-level. Runs the new size through the view's own
    // constraint and tells the view about it. A size we ourselves just applied is ignored, which
    // is what keeps this and resizeView from resizing each other in a loop.
    void windowConfigured(int w, int h);

    void run();
    void stop()
    {
        mRunning = false;
    }

    //---from IPlugFrame--------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) SMTG_OVERRIDE;

    //---from Linux::IRunLoop---------
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                                       Steinberg::Linux::FileDescriptor fd)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                                Steinberg::Linux::TimerInterval ms) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler)
        SMTG_OVERRIDE;

    //---from FUnknown----------------
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct EventEntry {
        Steinberg::Linux::IEventHandler *handler;
        int fd;
    };
    struct TimerEntry {
        Steinberg::Linux::ITimerHandler *handler;
        std::chrono::milliseconds interval;
        Clock::time_point next;
    };

    // Milliseconds until the soonest timer is due (0 if one is already due, -1 if there are no
    // timers at all).
    int fireDueTimersAndGetTimeout();

    // Inert unless RATIONS_STANDALONE_TRACE is set. The same env-var idiom as the editor's own
    // RATIONS_X11_TRACE, and here for the same reason: whether a window manager honours a resize
    // is a question that has to be answerable from a user's machine rather than from this one.
    void trace(const char *fmt, ...) const;

    // Resize the X window and remember the size, so the ConfigureNotify it provokes is recognised
    // as ours rather than treated as the user dragging the frame.
    void applySize(int w, int h);
    // Tell the window manager the smallest size the CURRENT page can be drawn at. The view is the
    // authority on that: checkSizeConstraint clamps whatever it is given up to the page's own
    // floor, and that floor differs per page (the settings page has a lower one).
    void updateSizeHints();

    ::Display *mDisplay = nullptr;
    ::Window mWindow = 0;
    Steinberg::IPlugView *mView = nullptr;
    int mAppliedW = 0;
    int mAppliedH = 0;

    XEventCallback mXCallback;
    std::vector<EventEntry> mEventHandlers;
    std::vector<TimerEntry> mTimers;
    bool mRunning = false;
    bool mTrace = false;
};

} // namespace Rations
