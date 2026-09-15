// Win32EventLoop implementation. See win32eventloop.h for why the timers are SetTimer rather than a
// computed timeout, and eventloop.h for the platform seam.

#include "win32eventloop.h"

#include <algorithm>
#include <cstdio>

using namespace Steinberg;

namespace Rations
{

namespace
{
// The window class for the message-only window. Registered once for the process.
//
// Unlike the plug-in's editor — which has to resolve its own module, because a window class whose
// procedure points into a DLL that has been unloaded is a crash on the next dispatched message —
// this is an executable's own class. The standalone is never unloaded, so the process module is
// both correct and the whole answer.
const wchar_t *kClassName = L"NampRackEventLoop";
} // namespace

//------------------------------------------------------------------------
Win32EventLoop::Win32EventLoop()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Win32EventLoop::windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // A class already registered is not an error: RegisterClassEx fails with
    // ERROR_CLASS_ALREADY_EXISTS, and the class is what we wanted either way.
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "namp-rack: cannot register the run loop's window class (error %lu)\n",
                GetLastError());
        return;
    }

    // HWND_MESSAGE: a window with no screen presence at all, which exists only to receive messages.
    // The timers hang off it, so they do not depend on the top-level window that has not been
    // created yet and will be destroyed before this loop is.
    mWindow = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (!mWindow)
        fprintf(stderr, "namp-rack: cannot create the run loop's message window (error %lu)\n",
                GetLastError());
}

//------------------------------------------------------------------------
Win32EventLoop::~Win32EventLoop()
{
    // Kill the timers before the window they hang off goes, so none can fire into a half-destroyed
    // loop. DestroyWindow would take them with it, but the order is written out rather than relied
    // on: this is the same reasoning as the teardown order for a hosted editor.
    if (mWindow) {
        for (const TimerEntry &t : mTimers)
            KillTimer(mWindow, t.id);
    }
    mTimers.clear();

    if (mWindow) {
        DestroyWindow(mWindow);
        mWindow = nullptr;
    }
}

//------------------------------------------------------------------------
LRESULT CALLBACK Win32EventLoop::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        // The instance travels through CreateWindowEx's last argument and is stashed before any
        // other message can arrive.
        const CREATESTRUCTW *cs = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs ? cs->lpCreateParams : nullptr));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    Win32EventLoop *self =
        reinterpret_cast<Win32EventLoop *>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (self && message == WM_TIMER) {
        self->onTimerMessage(static_cast<UINT_PTR>(wParam));
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

//------------------------------------------------------------------------
// One WM_TIMER carries one timer id, so this fires exactly one handler and there is no snapshot to
// take — the X11 sibling needs one because it sweeps a whole list at once. What IS still needed is
// the liveness re-check: a handler unregistered by an earlier tick may still have a WM_TIMER
// sitting in the queue behind it, and calling into it would be a use-after-free of whatever owned
// it.
void Win32EventLoop::onTimerMessage(UINT_PTR id)
{
    const auto it = std::find_if(mTimers.begin(), mTimers.end(),
                                 [id](const TimerEntry &t) { return t.id == id; });
    if (it == mTimers.end())
        return;

    Linux::ITimerHandler *handler = it->handler;
    if (handler)
        handler->onTimer();
}

//------------------------------------------------------------------------
tresult Win32EventLoop::registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval ms)
{
    if (!handler || ms == 0)
        return kInvalidArgument;
    if (!mWindow)
        return kInternalError;

    const UINT_PTR id = mNextTimerId++;
    if (SetTimer(mWindow, id, static_cast<UINT>(ms), nullptr) == 0) {
        fprintf(stderr, "namp-rack: cannot create a %u ms timer (error %lu)\n",
                static_cast<unsigned>(ms), GetLastError());
        return kInternalError;
    }
    mTimers.push_back({handler, id});
    return kResultTrue;
}

//------------------------------------------------------------------------
tresult Win32EventLoop::unregisterTimer(Linux::ITimerHandler *handler)
{
    if (!handler)
        return kInvalidArgument;

    const size_t before = mTimers.size();
    for (const TimerEntry &t : mTimers) {
        if (t.handler == handler && mWindow)
            KillTimer(mWindow, t.id);
    }
    mTimers.erase(std::remove_if(mTimers.begin(), mTimers.end(),
                                 [handler](const TimerEntry &t) { return t.handler == handler; }),
                  mTimers.end());
    return mTimers.size() != before ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
void Win32EventLoop::stop()
{
    mRunning.store(false, std::memory_order_relaxed);
    // Wake the pump. GetMessage blocks until something arrives, so the flag on its own would not be
    // noticed until the next timer fired — and once the standalone has unregistered its ticks there
    // would be no next timer. WM_NULL is the documented do-nothing message and DispatchMessage
    // discards it; all it has to do is make GetMessage return.
    if (mThreadId != 0)
        PostThreadMessageW(mThreadId, WM_NULL, 0, 0);
}

//------------------------------------------------------------------------
void Win32EventLoop::run()
{
    mThreadId = GetCurrentThreadId();
    mRunning.store(true, std::memory_order_relaxed);

    while (mRunning.load(std::memory_order_relaxed)) {
        MSG msg;
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0)
            break; // WM_QUIT
        if (got == -1) {
            fprintf(stderr, "namp-rack: the message loop failed (error %lu)\n", GetLastError());
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    mRunning.store(false, std::memory_order_relaxed);
}

} // namespace Rations
