// X11EventLoop implementation. See x11eventloop.h, and eventloop.h for the platform seam.

#include "x11eventloop.h"

#include <sys/select.h>

#include <algorithm>
#include <cerrno>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
X11EventLoop::X11EventLoop() : mDisplay(XOpenDisplay(nullptr))
{
}

//------------------------------------------------------------------------
X11EventLoop::~X11EventLoop()
{
    // Every window created against this connection is destroyed before this runs: they are declared
    // after the loop and so are torn down before it. Closing the connection first would destroy
    // their windows underneath them.
    if (mDisplay) {
        XCloseDisplay(mDisplay);
        mDisplay = nullptr;
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11EventLoop::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Linux::IRunLoop *>(this);
        addRef();
        return kResultOk;
    }

    // NOT IPlugFrame, and the omission is the point of this class existing. A plug-in that asks a
    // run loop for a frame is asking the wrong object: the frame is per view and knows which window
    // to resize, and answering here would hand every view the same one.
    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
void X11EventLoop::addWindow(::Window window, XEventCallback callback)
{
    if (!window)
        return;
    for (WindowEntry &e : mWindows) {
        if (e.window == window) {
            e.callback = std::move(callback);
            return;
        }
    }
    mWindows.push_back({window, std::move(callback)});
}

//------------------------------------------------------------------------
void X11EventLoop::removeWindow(::Window window)
{
    mWindows.erase(std::remove_if(mWindows.begin(), mWindows.end(),
                                  [window](const WindowEntry &e) { return e.window == window; }),
                   mWindows.end());
}

//------------------------------------------------------------------------
void X11EventLoop::dispatchX(const XEvent &event)
{
    // xany.window is the window the event was reported relative to, which is what every registered
    // window matched against. Copied out before the callback runs: a callback may remove windows,
    // and the entry it was found in can be gone by the time it returns.
    const ::Window target = event.xany.window;
    for (const WindowEntry &e : mWindows) {
        if (e.window != target)
            continue;
        XEventCallback callback = e.callback;
        if (callback)
            callback(event);
        return;
    }
    // No match: an event for a window we do not own, or one closed between the event being queued
    // and being read. Dropping it is correct in both cases.
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11EventLoop::registerEventHandler(Linux::IEventHandler *handler,
                                                      Linux::FileDescriptor fd)
{
    if (!handler || fd < 0)
        return kInvalidArgument;
    mEventHandlers.push_back({handler, fd});
    return kResultTrue;
}

tresult PLUGIN_API X11EventLoop::unregisterEventHandler(Linux::IEventHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mEventHandlers.size();
    mEventHandlers.erase(
        std::remove_if(mEventHandlers.begin(), mEventHandlers.end(),
                       [handler](const EventEntry &e) { return e.handler == handler; }),
        mEventHandlers.end());
    return mEventHandlers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API X11EventLoop::registerTimer(Linux::ITimerHandler *handler,
                                               Linux::TimerInterval ms)
{
    if (!handler || ms == 0)
        return kInvalidArgument;
    const std::chrono::milliseconds interval(ms);
    mTimers.push_back({handler, interval, Clock::now() + interval});
    return kResultTrue;
}

tresult PLUGIN_API X11EventLoop::unregisterTimer(Linux::ITimerHandler *handler)
{
    if (!handler)
        return kInvalidArgument;
    const size_t before = mTimers.size();
    mTimers.erase(std::remove_if(mTimers.begin(), mTimers.end(),
                                 [handler](const TimerEntry &t) { return t.handler == handler; }),
                  mTimers.end());
    return mTimers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
int X11EventLoop::fireDueTimersAndGetTimeout()
{
    if (mTimers.empty())
        return -1;

    const Clock::time_point now = Clock::now();

    // Fire from a snapshot: a handler may register or unregister timers, which would otherwise
    // invalidate the iteration.
    std::vector<Linux::ITimerHandler *> due;
    for (TimerEntry &t : mTimers) {
        if (t.next <= now) {
            due.push_back(t.handler);
            // Skip missed firings rather than trying to catch up in a burst.
            t.next = now + t.interval;
        }
    }
    for (Linux::ITimerHandler *handler : due) {
        // The handler may have been unregistered by an earlier callback.
        const bool live =
            std::any_of(mTimers.begin(), mTimers.end(),
                        [handler](const TimerEntry &t) { return t.handler == handler; });
        if (live)
            handler->onTimer();
    }

    if (mTimers.empty())
        return -1;

    Clock::time_point soonest = mTimers.front().next;
    for (const TimerEntry &t : mTimers)
        soonest = std::min(soonest, t.next);

    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(soonest - Clock::now()).count();
    return delta < 0 ? 0 : static_cast<int>(delta);
}

//------------------------------------------------------------------------
void X11EventLoop::run()
{
    mRunning.store(true, std::memory_order_relaxed);
    const int xFd = mDisplay ? ConnectionNumber(mDisplay) : -1;

    while (mRunning.load(std::memory_order_relaxed)) {
        // Anything already queued on our own connection is handled first: select() would not
        // report the fd as readable for events Xlib has already buffered.
        if (mDisplay) {
            while (XPending(mDisplay)) {
                XEvent event;
                XNextEvent(mDisplay, &event);
                dispatchX(event);
                if (!mRunning.load(std::memory_order_relaxed))
                    return;
            }
            XFlush(mDisplay);
        }

        const int timeoutMs = fireDueTimersAndGetTimeout();
        if (!mRunning.load(std::memory_order_relaxed))
            return;

        fd_set readSet;
        FD_ZERO(&readSet);
        int maxFd = -1;
        if (xFd >= 0) {
            FD_SET(xFd, &readSet);
            maxFd = xFd;
        }
        const std::vector<EventEntry> handlers = mEventHandlers; // snapshot
        for (const EventEntry &e : handlers) {
            FD_SET(e.fd, &readSet);
            maxFd = std::max(maxFd, e.fd);
        }
        if (maxFd < 0)
            break; // nothing left to wait on

        timeval tv;
        timeval *tvp = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvp = &tv;
        }

        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, tvp);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue; // timeout: loop round and fire the timers

        for (const EventEntry &e : handlers) {
            if (!FD_ISSET(e.fd, &readSet))
                continue;
            // Still registered? A previous callback may have removed it.
            const bool live =
                std::any_of(mEventHandlers.begin(), mEventHandlers.end(),
                            [&e](const EventEntry &c) { return c.handler == e.handler; });
            if (live)
                e.handler->onFDIsSet(e.fd);
            if (!mRunning.load(std::memory_order_relaxed))
                return;
        }
    }
}

} // namespace Rations
