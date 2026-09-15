// Win32EventLoop — the standalone's run loop on the platform that already has one.
//
// THIS IS THE WINDOWS HALF OF A PAIR; eventloop.h is the seam that picks between them. The two are
// genuinely different shapes rather than one file with #ifdefs in it, and the difference is worth
// stating because it is the whole reason this phase needed a window layer at all:
//
//   * X11EventLoop IS the event loop. A VST3 plug-in on Linux owns none, so the host must provide
//     one and hand it over as Steinberg::Linux::IRunLoop — an X connection, a select(), and a timer
//     list this project wrote itself.
//   * Windows already has that loop. Messages are dispatched to a window procedure by the system,
//     timers are a kernel service, and there is NO IRunLoop to implement: the plug-in side of this
//     same project says so in as many words (see the platform seam that picks the editor's base),
//     and the SDK's own Win32 editor-host sample is a plain GetMessage / DispatchMessage pump that
//     never provides one.
//
// SO THIS CLASS DELIBERATELY DOES NOT IMPLEMENT Linux::IRunLoop. Advertising an interface whose
// registerEventHandler half cannot work — it takes a file descriptor to select() on, and there are
// none here — would be worse than not offering it: a cross-platform plug-in that queried
// defensively would get a loop that accepts its timer and silently drops its event handler. On this
// platform the frame hands back nothing, and a plug-in uses its own timers, which is what every
// Windows plug-in does anyway.
//
// IT STILL TAKES Linux::ITimerHandler, and that is a deliberate reuse rather than an oversight.
// Measured rather than assumed: pluginterfaces/gui/iplugview.h declares that namespace with NO
// platform guard, so the type exists in every build. Using it here as this loop's own callback
// interface is what lets the standalone's seven timer classes — the feedback pump, the rack tick,
// the chain collector and the rest — be written once and compile on both platforms instead of
// twice. The name reads oddly in a Windows file; a second interface that differed only in its name
// would read worse.
//
// TIMERS ARE SetTimer, NOT A COMPUTED select() TIMEOUT, and that is the one place this file
// deliberately departs from its X11 sibling's shape. Two reasons, in order of weight:
//
//   1. A Win32 timer keeps firing inside the system's own MODAL message loops — while the user
//      drags the window frame, and while a hosted plug-in has a modal dialog open. A loop that
//      waited on its own computed timeout would stop ticking for as long as either lasted, and a
//      plug-in's file browser can last a while. Audio is unaffected either way (it is on the
//      device's own thread), so what stalls is only painting and meters — but a rack strip frozen
//      behind somebody else's open dialog looks like a hang.
//   2. It is what this project's own Win32 editor base already does, at this very interval. One
//      idiom for timers across the plug-in and the standalone is worth more than a shared shape
//      with the X11 file.
//
// The cost is resolution: a Win32 timer is quantised to the system tick, nominally 15.6 ms, so a
// 33 ms request fires at 31.2 or 46.8. That is invisible in a 30 Hz repaint and is the same
// quantisation the plug-in's editor has always run at.
//
// THE TIMERS LIVE ON A MESSAGE-ONLY WINDOW, so they do not depend on any visible window existing.
// The standalone registers its ticks before the top-level is created and unregisters them after it
// is gone, and a timer whose window had been destroyed underneath it would simply stop.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class Win32EventLoop
{
public:
    Win32EventLoop();
    ~Win32EventLoop();

    Win32EventLoop(const Win32EventLoop &) = delete;
    Win32EventLoop &operator=(const Win32EventLoop &) = delete;

    // True if the message-only window that carries the timers exists. A loop without it still runs
    // and still dispatches window messages; it just cannot hold a timer.
    bool isValid() const
    {
        return mWindow != nullptr;
    }

    void run();

    // Safe to call from a signal handler, which is what the standalone does on SIGINT and SIGTERM.
    // See the note on the member for why the flag is an atomic, and why setting it is not enough on
    // its own.
    void stop();

    //--- timers ---------------------------------------------------------
    // Same semantics as the X11 sibling's, deliberately: registering one handler twice yields two
    // independent timers, and unregistering removes every timer that handler holds.
    Steinberg::tresult registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                     Steinberg::Linux::TimerInterval ms);
    Steinberg::tresult unregisterTimer(Steinberg::Linux::ITimerHandler *handler);

private:
    struct TimerEntry {
        Steinberg::Linux::ITimerHandler *handler;
        UINT_PTR id;
    };

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void onTimerMessage(UINT_PTR id);

    HWND mWindow = nullptr;
    std::vector<TimerEntry> mTimers;
    // Ids are ours to choose and are never reused, so a WM_TIMER still in the queue for a timer
    // that has just been killed cannot be mistaken for a live one.
    UINT_PTR mNextTimerId = 1;
    // The thread that called run(), so stop() can wake the pump from another one. GetMessage blocks
    // indefinitely when there is nothing to do, so setting the flag alone would not be noticed
    // until the next timer fired — and a loop with no timers left would never notice at all.
    DWORD mThreadId = 0;
    std::atomic<bool> mRunning{false};
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "stop() is called from a signal handler, which may only touch a lock-free "
                  "atomic or a volatile sig_atomic_t");
};

} // namespace Rations
