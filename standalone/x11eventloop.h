// X11EventLoop — the one Linux::IRunLoop this process has, on the platform that needs one.
//
// THIS IS THE X11 HALF OF A PAIR; eventloop.h is the seam that picks between them and states what
// the two have in common. The Windows half is a message pump and is a genuinely different shape,
// which is why this is two files rather than one with #ifdefs in it — see win32eventloop.h.
//
// On Linux a VST3 plug-in owns no event loop; the host must provide one and hand it to the plug-in
// through IPlugFrame::queryInterface. That is exactly what this project's editor asks for
// (src/platform/x11plugview.cpp, attachedToParent), so a standalone has to implement it to show its
// own editor at all — and so does anything that embeds somebody else's.
//
// THE LOOP AND THE FRAME ARE DIFFERENT SCOPES, and this file exists because they are. They were one
// object while there was exactly one view to show, and the object that fused them said so:
//
//   * Linux::IRunLoop is per PROCESS. There is one X connection of ours, one select(), one timer
//     list. Every editor — ours and every hosted plug-in's — registers its descriptors and timers
//     into this one.
//   * IPlugFrame is per VIEW. resizeView() has to resize the window THAT view lives in, and the
//     frame pointer is the only context the callback gets, so a shared frame could only guess.
//     See editorframe.h for the amp's, which also carries this host's size policy.
//
// Splitting them before there is a second view is deliberate. With one editor the fused object is
// still correct, so the split can be made and PROVED against a running product; with several it is
// a refactor under a bug.
//
// X EVENTS ARE DISPATCHED BY WINDOW ID. Each top-level window registers a callback and an event
// goes to the callback whose window matches XEvent::xany.window — the window the event was reported
// relative to — and is dropped if none matches. Events for a plug-in's own child windows never
// arrive here at all: a plug-in that opens its own display connection gets them on that connection,
// through the file descriptor it registered above.
//
// Registrations may change while a callback is running — a plug-in unregisters its handler from
// inside removed(), which is called from inside a timer callback when a window closes — so every
// iteration works on a snapshot and re-checks that an entry is still live before invoking it.
//
// Reference: the SDK's own editorhost sample implements the same interface at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class X11EventLoop : public Steinberg::Linux::IRunLoop
{
public:
    using XEventCallback = std::function<void(const XEvent &)>;

    // Opens the process's one connection to the display, and closes it again in the destructor.
    //
    // THE LOOP OWNS THE CONNECTION rather than being handed one, which is what lets the standalone
    // construct its run loop identically on both platforms — the Windows sibling has no connection
    // to open and would have nothing to be given. It is also the truthful arrangement: everything
    // that reaches the display goes through this object's select(), so a connection opened
    // elsewhere would have exactly one legitimate use and one owner who was not its creator. Check
    // isValid() before use; a machine with no display server is a clean failure, not a crash.
    X11EventLoop();
    ~X11EventLoop();

    X11EventLoop(const X11EventLoop &) = delete;
    X11EventLoop &operator=(const X11EventLoop &) = delete;

    // False when the display could not be opened. The Windows sibling has the same question with a
    // different subject, so the standalone asks it the same way on both.
    bool isValid() const
    {
        return mDisplay != nullptr;
    }

    ::Display *display() const
    {
        return mDisplay;
    }

    //--- X dispatch -----------------------------------------------------
    // Registering the same window twice replaces the callback rather than adding a second one, so
    // a window that is closed and reopened cannot accumulate stale handlers.
    void addWindow(::Window window, XEventCallback callback);
    // Safe to call from inside a callback, including that window's own.
    void removeWindow(::Window window);

    void run();

    // Safe to call from a signal handler, which is what the standalone does on SIGINT and SIGTERM.
    // That is why mRunning is an atomic and not a plain bool: a handler may portably touch only a
    // volatile sig_atomic_t or a lock-free atomic, and both parents this file was ported from
    // write a plain bool here. std::atomic<bool> is lock-free on every target this builds for, and
    // the assertion beside the member is what would catch a target where it is not.
    void stop()
    {
        mRunning.store(false, std::memory_order_relaxed);
    }

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
    // Lifetime is the C++ object's: this lives on main's stack for the whole run, and every plug-in
    // that queries it holds only a borrowed pointer. A fixed count keeps a plug-in that releases
    // more times than it addRefs from taking the loop down with it.
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
    struct WindowEntry {
        ::Window window;
        XEventCallback callback;
    };

    void dispatchX(const XEvent &event);
    // Milliseconds until the soonest timer is due (0 if one is already due, -1 if there are no
    // timers at all).
    int fireDueTimersAndGetTimeout();

    ::Display *mDisplay = nullptr;
    std::vector<WindowEntry> mWindows;
    std::vector<EventEntry> mEventHandlers;
    std::vector<TimerEntry> mTimers;
    std::atomic<bool> mRunning{false};
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "stop() is called from a signal handler, which may only touch a lock-free "
                  "atomic or a volatile sig_atomic_t");
};

} // namespace Rations
