// Win32PlugView implementation. See win32plugview.h for the message-pump
// contract and how it differs from the X11 side.

#include "win32plugview.h"

#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace Steinberg
{

namespace
{
// ~30 Hz, matching the X11 side and the original plug-in's meter refresh.
constexpr UINT kTimerMs = 33;

// Any non-zero id works; the timer is owned by our own window, so it cannot
// collide with the host's.
constexpr UINT_PTR kTimerId = 1;

// The hook contract uses X button numbers, so that RationsEditorView's hit-testing
// is identical on both platforms.
constexpr int kButtonLeft = 1;
constexpr int kButtonMiddle = 2;
constexpr int kButtonRight = 3;

// One trace summary line per ~1 s, and the threshold above which a repaint is
// interesting enough to log on its own (i.e. the editor was quiet and then
// something woke it up).
constexpr unsigned long kSummaryTicks = 30;
constexpr unsigned long kQuietRedrawTicks = 15;

// Address of a symbol in THIS module, so GetModuleHandleEx can resolve which
// module the window class and window procedure belong to. Taking the address of
// a local function is enough.
void marker()
{
}

//------------------------------------------------------------------------
// The HINSTANCE of the module this code was linked into — NOT the host's.
//
// It matters twice over. RegisterClassEx keys a class on (name, hInstance), so
// using the host's would register our class against a module that knows nothing
// about it and would leave it behind when our DLL unloads. And a window class
// whose lpfnWndProc points into a DLL that has been unloaded is a crash the
// next time any message is dispatched.
HINSTANCE thisModule()
{
    static HINSTANCE instance = [] {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&marker), &module) == 0)
            module = nullptr;
        return reinterpret_cast<HINSTANCE>(module);
    }();
    return instance;
}

//------------------------------------------------------------------------
// The window class, registered once per module.
//
// The name carries the module handle because a host can load several raw-SDK
// plug-ins at once, and two of them sharing a class name — perfectly likely if
// this file is ever reused, which is what it is written to be — would have the
// second registration fail and its windows silently use the first one's window
// procedure.
//
// CS_DBLCLKS is deliberately NOT set. With it, the second click of a pair
// arrives as WM_LBUTTONDBLCLK instead of WM_LBUTTONDOWN, and the editor would
// simply lose that click. X11 delivers two ordinary presses; this keeps the two
// platforms behaving the same. CS_HREDRAW | CS_VREDRAW invalidate the whole
// window on a size change, so a resize can never leave a stale edge.
// `proc` is passed in rather than named here because the window procedure is a
// private member of Win32PlugView; openWindow() supplies it.
const wchar_t *windowClassName(WNDPROC proc)
{
    static wchar_t name[64];
    static const bool registered = [proc] {
        std::swprintf(name, sizeof(name) / sizeof(name[0]), L"RationsPlugView_%p",
                      static_cast<void *>(thisModule()));

        WNDCLASSEXW wcex;
        std::memset(&wcex, 0, sizeof(wcex));
        wcex.cbSize = sizeof(wcex);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = proc;
        wcex.hInstance = thisModule();
        // The host sets the cursor for its own windows; without one of our own,
        // whatever shape it last set would persist across our editor.
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr; // we paint every pixel ourselves
        wcex.lpszClassName = name;

        if (RegisterClassExW(&wcex) == 0) {
            // Already registered is not a failure: it is what a second view in
            // the same module sees, and what a re-loaded module sees if the
            // class outlived it.
            const DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                fprintf(stderr, "Rations: cannot register the editor window class (error %lu)\n",
                        static_cast<unsigned long>(err));
                return false;
            }
        }
        return true;
    }();
    return registered ? name : nullptr;
}

//------------------------------------------------------------------------
const char *messageName(UINT msg)
{
    switch (msg) {
        case WM_NCCREATE:
            return "WM_NCCREATE";
        case WM_CREATE:
            return "WM_CREATE";
        case WM_DESTROY:
            return "WM_DESTROY";
        case WM_PAINT:
            return "WM_PAINT";
        case WM_ERASEBKGND:
            return "WM_ERASEBKGND";
        case WM_TIMER:
            return "WM_TIMER";
        case WM_SIZE:
            return "WM_SIZE";
        case WM_LBUTTONDOWN:
            return "WM_LBUTTONDOWN";
        case WM_LBUTTONUP:
            return "WM_LBUTTONUP";
        case WM_MBUTTONDOWN:
            return "WM_MBUTTONDOWN";
        case WM_MBUTTONUP:
            return "WM_MBUTTONUP";
        case WM_RBUTTONDOWN:
            return "WM_RBUTTONDOWN";
        case WM_RBUTTONUP:
            return "WM_RBUTTONUP";
        case WM_MOUSEMOVE:
            return "WM_MOUSEMOVE";
        case WM_MOUSELEAVE:
            return "WM_MOUSELEAVE";
        case WM_MOUSEWHEEL:
            return "WM_MOUSEWHEEL";
        case WM_CAPTURECHANGED:
            return "WM_CAPTURECHANGED";
        case WM_SHOWWINDOW:
            return "WM_SHOWWINDOW";
        default:
            return nullptr;
    }
}
} // namespace

//------------------------------------------------------------------------
Win32PlugView::Win32PlugView(Vst::EditController *controller, ViewRect *size)
    : Vst::EditorView(controller, size)
{
    // The 100% size. setContentScaleFactor multiplies THIS rather than the
    // current size, so repeated scale changes cannot compound.
    mBaseW = rect.getWidth();
    mBaseH = rect.getHeight();
}

Win32PlugView::~Win32PlugView()
{
    // removedFromParent() is the normal teardown path; this only covers a
    // view destroyed while still attached by a non-conforming host.
    closeWindow();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Win32PlugView::isPlatformTypeSupported(FIDString type)
{
    if (type && std::strcmp(type, kPlatformTypeHWND) == 0)
        return kResultTrue;
    return kResultFalse;
}

//------------------------------------------------------------------------
// CPluginView::attached() always reports success, which would leave a host
// believing in an editor that does not exist. Report what actually happened
// instead: a host that is told the attach failed can fall back to its generic
// parameter panel rather than showing an empty rectangle.
tresult PLUGIN_API Win32PlugView::attached(void *parent, FIDString type)
{
    const tresult result = CPluginView::attached(parent, type);
    if (result != kResultOk)
        return result;
    return isWindowOpen() ? kResultOk : kResultFalse;
}

//------------------------------------------------------------------------
void Win32PlugView::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    fputs("Rations/win: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    // The host's stderr is usually a pipe or nothing at all, so without this
    // the interesting lines are still sitting in the CRT's buffer when the user
    // hits the bug.
    fflush(stderr);
}

//------------------------------------------------------------------------
// The ground truth the message stream cannot give us: whether our window is
// actually visible, where it sits inside the host's, and how big it thinks it
// is against what we believe.
void Win32PlugView::traceWindowState(const char *when) const
{
    if (!mTrace || !mWindow)
        return;

    RECT client;
    std::memset(&client, 0, sizeof(client));
    GetClientRect(mWindow, &client);

    RECT screen = client;
    MapWindowPoints(mWindow, nullptr, reinterpret_cast<POINT *>(&screen), 2);

    trace("%s: self %p client %ldx%ld at screen %ld,%ld visible=%d dirty=%d rect=%dx%d", when,
          static_cast<void *>(mWindow), client.right - client.left, client.bottom - client.top,
          screen.left, screen.top, IsWindowVisible(mWindow) ? 1 : 0, mDirty ? 1 : 0,
          rect.getWidth(), rect.getHeight());

    const HWND parent = GetParent(mWindow);
    if (!parent)
        return;
    RECT parentClient;
    std::memset(&parentClient, 0, sizeof(parentClient));
    GetClientRect(parent, &parentClient);
    trace("%s: parent %p client %ldx%ld visible=%d", when, static_cast<void *>(parent),
          parentClient.right - parentClient.left, parentClient.bottom - parentClient.top,
          IsWindowVisible(parent) ? 1 : 0);
}

//------------------------------------------------------------------------
LRESULT CALLBACK Win32PlugView::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // WM_NCCREATE is the first message a window receives, so the instance
    // pointer is installed there rather than after CreateWindowEx returns —
    // otherwise WM_NCCREATE, WM_CREATE and the first WM_SIZE would all arrive
    // with no view to dispatch them to.
    if (msg == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        if (create && create->lpCreateParams) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto *view = reinterpret_cast<Win32PlugView *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!view || view->mWindow != hwnd)
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    return view->wndProc(msg, wParam, lParam);
}

//------------------------------------------------------------------------
LRESULT Win32PlugView::wndProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    ++mMessageCount;
    if (mTrace && msg != WM_TIMER && msg != WM_MOUSEMOVE) {
        if (const char *name = messageName(msg))
            trace("msg %s w=0x%llx l=0x%llx", name, static_cast<unsigned long long>(wParam),
                  static_cast<unsigned long long>(lParam));
    }

    switch (msg) {
        case WM_ERASEBKGND:
            // Every pixel is painted by the blit, so letting the system erase
            // first would only produce a flash of background colour.
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(mWindow, &ps);
            if (dc) {
                // The last composed frame, NOT a fresh onDraw(): drawing from
                // inside a paint message is exactly the re-entrancy the
                // deferred-paint discipline exists to prevent.
                blit(dc);
                EndPaint(mWindow, &ps);
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kTimerId) {
                ++mTickCount;
                ++mTicksSinceRedraw;
                if (mTrace && mTickCount % kSummaryTicks == 0) {
                    trace("tick %lu: messages=%lu redraws=%lu dirty=%d buttons=0x%x", mTickCount,
                          mMessageCount, mRedrawCount, mDirty ? 1 : 0, mButtonsDown);
                    traceWindowState("tick");
                }
                onTick();
                if (mDirty)
                    redraw();
                return 0;
            }
            break;

        case WM_SIZE:
            // A host that resizes our window directly rather than routing
            // through onSize() still reaches us here — the counterpart of the
            // X11 side following ConfigureNotify.
            followSize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_LBUTTONDOWN:
            pressButton(kButtonLeft, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MBUTTONDOWN:
            pressButton(kButtonMiddle, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONDOWN:
            pressButton(kButtonRight, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            releaseButton(kButtonLeft, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MBUTTONUP:
            releaseButton(kButtonMiddle, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONUP:
            releaseButton(kButtonRight, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            mLastMouseX = GET_X_LPARAM(lParam);
            mLastMouseY = GET_Y_LPARAM(lParam);
            // Windows sends no "the pointer left" message unless it is asked
            // to, and the request is one-shot: it has to be re-armed after
            // every WM_MOUSELEAVE.
            armMouseLeave();
            // Already coalesced by the system — a WM_MOUSEMOVE is only
            // generated when the queue has none pending, so the X11 side's
            // explicit motion compression has no counterpart here.
            onMouseMove(mLastMouseX, mLastMouseY);
            return 0;

        case WM_MOUSELEAVE:
            mTrackingLeave = false;
            onMouseLeave();
            return 0;

        case WM_MOUSEWHEEL: {
            // Unlike every other mouse message, these coordinates are SCREEN
            // relative.
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(mWindow, &pt);

            mWheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            while (mWheelRemainder >= WHEEL_DELTA) {
                mWheelRemainder -= WHEEL_DELTA;
                onMouseWheel(pt.x, pt.y, 1);
            }
            while (mWheelRemainder <= -WHEEL_DELTA) {
                mWheelRemainder += WHEEL_DELTA;
                onMouseWheel(pt.x, pt.y, -1);
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Someone else took the capture — a host opening a modal dialog
            // mid-drag, an Alt-Tab. Without this the editor would still believe
            // the button is down, and for a knob drag that means beginEdit()
            // was called and endEdit() never will be, leaving the host's
            // automation latched on that parameter for the rest of the session.
            releaseAllButtons();
            return 0;

        case WM_DESTROY:
            // The host destroyed our window without going through removed().
            // Drop the handle so nothing below tries to use it.
            SetWindowLongPtrW(mWindow, GWLP_USERDATA, 0);
            mWindow = nullptr;
            return 0;

        default:
            break;
    }

    return DefWindowProcW(mWindow, msg, wParam, lParam);
}

//------------------------------------------------------------------------
// SetCapture is the single most important line in this file.
//
// X11 gives an implicit pointer grab for the duration of a button press, so a
// knob drag that wanders outside the editor keeps delivering motion events.
// Windows gives no such thing: without an explicit capture, the moment the
// pointer crosses our window's edge the drag stops receiving WM_MOUSEMOVE and
// the knob freezes. Capture is taken on the first button down and released only
// when the last one comes up.
void Win32PlugView::pressButton(int button, int x, int y)
{
    mLastMouseX = x;
    mLastMouseY = y;
    if (mButtonsDown == 0)
        SetCapture(mWindow);
    mButtonsDown |= 1u << button;
    onMouseDown(x, y, button);
}

void Win32PlugView::releaseButton(int button, int x, int y)
{
    mLastMouseX = x;
    mLastMouseY = y;
    const unsigned bit = 1u << button;
    if ((mButtonsDown & bit) == 0)
        return; // a stray up (pressed elsewhere, released over us)
    mButtonsDown &= ~bit;
    if (mButtonsDown == 0 && GetCapture() == mWindow)
        ReleaseCapture();
    onMouseUp(x, y, button);
}

void Win32PlugView::releaseAllButtons()
{
    if (mButtonsDown == 0)
        return;
    const unsigned held = mButtonsDown;
    mButtonsDown = 0;
    // Synthesised at the last position we saw, which is what a real button-up
    // would have carried, so a drag ends where it visually stopped.
    for (int button = kButtonLeft; button <= kButtonRight; ++button)
        if (held & (1u << button))
            onMouseUp(mLastMouseX, mLastMouseY, button);
}

//------------------------------------------------------------------------
void Win32PlugView::armMouseLeave()
{
    if (mTrackingLeave || !mWindow)
        return;
    TRACKMOUSEEVENT tme;
    std::memset(&tme, 0, sizeof(tme));
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = mWindow;
    if (TrackMouseEvent(&tme))
        mTrackingLeave = true;
}

//------------------------------------------------------------------------
// The drawing buffer is a DIB section, and the Cairo surface is created over
// the DIB's own pixels rather than over a separate allocation. onDraw()
// therefore paints straight into what BitBlt will read, which removes the copy
// the X11 path needs between its image surface and its xlib surface.
//
// The format is deliberately CAIRO_FORMAT_ARGB32 rather than RGB24, so that the
// bytes this produces are identical to what the X11 build composes: there, the
// ARGB32 buffer is painted onto the window with CAIRO_OPERATOR_SOURCE, which
// copies the premultiplied RGB and discards the alpha — exactly what BitBlt
// does with the unused fourth byte of a 32-bit BI_RGB DIB.
bool Win32PlugView::createSurfaces(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return false;

    const HDC windowDC = GetDC(mWindow);
    if (!windowDC) {
        fprintf(stderr, "Rations: cannot obtain a device context for the editor\n");
        return false;
    }
    mMemDC = CreateCompatibleDC(windowDC);
    ReleaseDC(mWindow, windowDC);
    if (!mMemDC) {
        fprintf(stderr, "Rations: cannot create the editor's memory device context\n");
        return false;
    }

    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    // NEGATIVE: a DIB is bottom-up by default, and Cairo's image surfaces are
    // top-down. Getting this wrong produces a vertically mirrored editor rather
    // than an error.
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    mDibBits = nullptr;
    mDib = CreateDIBSection(mMemDC, &bmi, DIB_RGB_COLORS, &mDibBits, nullptr, 0);
    if (!mDib || !mDibBits) {
        fprintf(stderr, "Rations: cannot create the editor's %dx%d drawing buffer\n", w, h);
        destroySurfaces();
        return false;
    }
    mOldBitmap = SelectObject(mMemDC, mDib);

    // A 32-bit DIB's rows are 4-byte aligned by construction and Cairo's ARGB32
    // stride is 4 bytes per pixel, so these agree for every width — but the
    // consequence of them ever disagreeing is a skewed image rather than a
    // failure, so it is checked rather than assumed.
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    if (stride != w * 4) {
        fprintf(stderr, "Rations: unexpected Cairo stride %d for width %d (expected %d)\n", stride, w,
                w * 4);
        destroySurfaces();
        return false;
    }

    mSurface = cairo_image_surface_create_for_data(static_cast<unsigned char *>(mDibBits),
                                                   CAIRO_FORMAT_ARGB32, w, h, stride);
    if (cairo_surface_status(mSurface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Rations: cannot create the editor drawing surface\n");
        destroySurfaces();
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void Win32PlugView::destroySurfaces()
{
    if (mSurface) {
        cairo_surface_destroy(mSurface);
        mSurface = nullptr;
    }
    if (mMemDC) {
        // The DIB must be deselected before it can be deleted, and the DC must
        // hold its original bitmap again before it is.
        if (mOldBitmap)
            SelectObject(mMemDC, mOldBitmap);
        mOldBitmap = nullptr;
        DeleteDC(mMemDC);
        mMemDC = nullptr;
    }
    if (mDib) {
        DeleteObject(mDib);
        mDib = nullptr;
    }
    mDibBits = nullptr;
}

//------------------------------------------------------------------------
bool Win32PlugView::resizeSurfaces(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return false;

    // Keep the old buffer if the new one cannot be made: drawing at the
    // previous size is wrong but survivable, whereas no buffer would stop the
    // editor painting at all for the rest of the session.
    cairo_surface_t *oldSurface = mSurface;
    HDC oldMemDC = mMemDC;
    HBITMAP oldDib = mDib;
    HGDIOBJ oldOldBitmap = mOldBitmap;
    void *oldBits = mDibBits;

    mSurface = nullptr;
    mMemDC = nullptr;
    mDib = nullptr;
    mOldBitmap = nullptr;
    mDibBits = nullptr;

    if (!createSurfaces(w, h)) {
        fprintf(stderr, "Rations: cannot resize the editor buffer to %dx%d\n", w, h);
        mSurface = oldSurface;
        mMemDC = oldMemDC;
        mDib = oldDib;
        mOldBitmap = oldOldBitmap;
        mDibBits = oldBits;
        return false;
    }

    // The replacement is in place, so the previous set can go.
    if (oldSurface)
        cairo_surface_destroy(oldSurface);
    if (oldMemDC) {
        if (oldOldBitmap)
            SelectObject(oldMemDC, oldOldBitmap);
        DeleteDC(oldMemDC);
    }
    if (oldDib)
        DeleteObject(oldDib);
    return true;
}

//------------------------------------------------------------------------
bool Win32PlugView::openWindow(HWND parent)
{
    mTrace = std::getenv("RATIONS_WIN_TRACE") != nullptr;

    const wchar_t *className = windowClassName(&Win32PlugView::wndProcThunk);
    if (!className)
        return false;

    const int width = rect.getWidth();
    const int height = rect.getHeight();

    // WS_CHILD is the whole embedding contract on this platform: no XEmbed
    // handshake, no _XEMBED_INFO property, no waiting to see whether the
    // embedder maps us, and no per-host workaround for the ones that never do.
    // WS_CLIPSIBLINGS and WS_CLIPCHILDREN keep our painting inside our own
    // rectangle when the host stacks other windows over the same container.
    //
    // `this` travels through lpCreateParams and is installed by the thunk on
    // WM_NCCREATE, so the very first message already finds its view.
    mWindow = CreateWindowExW(0, className, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                              width, height, parent, nullptr, thisModule(), this);
    if (!mWindow) {
        fprintf(stderr, "Rations: the host rejected the editor window (parent %p, error %lu)\n",
                static_cast<void *>(parent), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    trace("openWindow: created %p (%dx%d) in parent %p", static_cast<void *>(mWindow), width,
          height, static_cast<void *>(parent));

    if (!createSurfaces(width, height)) {
        closeWindow();
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void Win32PlugView::closeWindow()
{
    if (mTimerId) {
        if (mWindow)
            KillTimer(mWindow, mTimerId);
        mTimerId = 0;
    }
    if (mWindow && GetCapture() == mWindow)
        ReleaseCapture();
    mButtonsDown = 0;
    mTrackingLeave = false;
    mWheelRemainder = 0;

    destroySurfaces();

    if (mWindow) {
        trace("closeWindow: destroying %p", static_cast<void *>(mWindow));
        // Clear the back pointer first: DestroyWindow dispatches WM_DESTROY
        // synchronously, and nothing in this object should be reached from a
        // message once teardown has begun.
        SetWindowLongPtrW(mWindow, GWLP_USERDATA, 0);
        const HWND window = mWindow;
        mWindow = nullptr;
        DestroyWindow(window);
    }
}

//------------------------------------------------------------------------
void Win32PlugView::attachedToParent()
{
    const HWND parent = static_cast<HWND>(systemWindow);
    if (!parent)
        return;

    mTrace = std::getenv("RATIONS_WIN_TRACE") != nullptr;
    trace("attachedToParent: parent=%p existing window=%p", static_cast<void *>(parent),
          static_cast<void *>(mWindow));

    if (!mWindow && !openWindow(parent))
        return;

    // The repaint tick. On Linux this is Linux::IRunLoop::registerTimer and the
    // host can refuse it; here it is ours, and the only way it fails is running
    // out of timers.
    mTimerId = SetTimer(mWindow, kTimerId, kTimerMs, nullptr);
    if (mTimerId == 0)
        fprintf(stderr, "Rations: cannot start the editor's repaint timer; "
                        "the editor will not animate\n");

    onAttached();
    // The subclass caches art at device resolution, so it needs the current
    // size before the first paint — attaching at a restored, non-default size
    // is otherwise drawn once at the wrong scale.
    onResized(rect.getWidth(), rect.getHeight());
    mDirty = true;

    // Notify the controller (EditorView::attachedToParent -> editorAttached)
    // only once the window exists, so it may immediately push values in.
    Vst::EditorView::attachedToParent();

    redraw();
    traceWindowState("attached");
}

//------------------------------------------------------------------------
void Win32PlugView::removedFromParent()
{
    trace("removedFromParent: window=%p", static_cast<void *>(mWindow));

    // Controller first, so it stops touching this view before anything is
    // torn down.
    Vst::EditorView::removedFromParent();

    onRemoved();
    closeWindow();
}

//------------------------------------------------------------------------
tresult PLUGIN_API Win32PlugView::canResize()
{
    return isResizable() ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
// The host asking whether a size is acceptable, typically once per mouse move
// during a window drag. Answer by rewriting the rect through the subclass's
// rule; kResultTrue means "I changed it", kResultFalse means "it was already
// fine", and the SDK expects the adjusted rect either way.
tresult PLUGIN_API Win32PlugView::checkSizeConstraint(ViewRect *proposed)
{
    if (!proposed)
        return kInvalidArgument;
    if (!isResizable())
        return kResultFalse;

    int w = proposed->getWidth();
    int h = proposed->getHeight();
    constrainSize(w, h);
    proposed->right = proposed->left + w;
    proposed->bottom = proposed->top + h;
    // kResultTrue WHETHER OR NOT the rect was changed, which is what VSTGUI does
    // (vstgui4/vstgui/plug-in-bindings/vst3editor.cpp, VST3Editor::checkSizeConstraint and
    // AspectRatioVST3Editor::checkSizeConstraint: both write the adjusted rect and then return
    // kResultTrue unconditionally). The SDK header only says "check if the view can be resized to
    // the given rect, if not adjust the rect to the allowed size", which reads either way, so the
    // reference implementation is the tie-breaker: every host is tested against VSTGUI plug-ins,
    // so behaving exactly like one is what keeps a host from surprising us.
    //
    // Reporting "unchanged" as kResultFalse looks more informative and buys nothing — the host
    // has the rect either way and can compare it itself.
    return kResultTrue;
}

//------------------------------------------------------------------------
// The host telling us it has resized the parent. This is the ONLY place the
// window is resized, per the SDK's "please only resize the platform
// representation of the view when onSize() is called".
tresult PLUGIN_API Win32PlugView::onSize(ViewRect *newSize)
{
    if (!newSize)
        return kInvalidArgument;

    int w = newSize->getWidth();
    int h = newSize->getHeight();
    if (isResizable())
        constrainSize(w, h);

    const bool changed = (w != rect.getWidth() || h != rect.getHeight());
    rect = *newSize;
    rect.right = rect.left + w;
    rect.bottom = rect.top + h;

    if (changed && isWindowOpen()) {
        // SWP_NOZORDER/NOACTIVATE: our position inside the host's container and
        // the host's focus are the host's business, not ours.
        SetWindowPos(mWindow, nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        resizeSurfaces(w, h);
        onResized(w, h);
        mDirty = true;
        // Repaint now rather than on the next tick: during a drag the host
        // shows our window at its new size immediately, and waiting up to
        // 33 ms for the timer shows a stale or torn frame at every step.
        redraw();
    }
    return kResultTrue;
}

//------------------------------------------------------------------------
// A size the host imposed on our window directly, rather than announcing
// through onSize(). Following it keeps the buffer from ever disagreeing with
// the window it is painting.
void Win32PlugView::followSize(int w, int h)
{
    if (w <= 0 || h <= 0 || !isWindowOpen())
        return;
    if (w == rect.getWidth() && h == rect.getHeight())
        return;

    rect.right = rect.left + w;
    rect.bottom = rect.top + h;
    resizeSurfaces(w, h);
    onResized(w, h);
    mDirty = true;
}

//------------------------------------------------------------------------
// The host reporting the display scaling it wants the editor drawn at.
//
// The editor's layout is resolution-independent by construction — one logical
// canvas and a single cairo_scale at compose time — so there is nothing to do
// here beyond asking for a window of the right size and letting the existing
// resize path notice. The scale is applied to the BASE size, not the current
// one, so repeated calls cannot compound.
tresult PLUGIN_API Win32PlugView::setContentScaleFactor(ScaleFactor factor)
{
    // A host is allowed to call this at any time the view is valid, including
    // before the frame is set, so an implausible value has to be refused rather
    // than turned into a window of zero or absurd size.
    if (!(factor > 0.0f) || factor > 16.0f || mBaseW <= 0 || mBaseH <= 0)
        return kResultFalse;
    if (factor == mContentScale)
        return kResultTrue;
    mContentScale = factor;

    int w = static_cast<int>(mBaseW * factor + 0.5f);
    int h = static_cast<int>(mBaseH * factor + 0.5f);
    if (isResizable())
        constrainSize(w, h);
    trace("setContentScaleFactor: %.2f -> %dx%d", static_cast<double>(factor), w, h);

    ViewRect scaled(rect.left, rect.top, rect.left + w, rect.top + h);

    // With a frame, ask the host to resize us and let it call back into
    // onSize(); that is the sequence the SDK documents. Without one, the SDK
    // requires getSize() to already report the new size, so write it here.
    if (plugFrame)
        return plugFrame->resizeView(this, &scaled) == kResultTrue ? kResultTrue : kResultFalse;

    rect = scaled;
    return kResultTrue;
}

//------------------------------------------------------------------------
void Win32PlugView::blit(HDC dc)
{
    if (!dc || !mMemDC || !mSurface)
        return;
    // Everything Cairo has drawn must have reached the DIB's pixels before GDI
    // reads them.
    cairo_surface_flush(mSurface);
    BitBlt(dc, 0, 0, cairo_image_surface_get_width(mSurface),
           cairo_image_surface_get_height(mSurface), mMemDC, 0, 0, SRCCOPY);
}

//------------------------------------------------------------------------
void Win32PlugView::redraw()
{
    if (!mSurface || !mWindow)
        return;
    // A repaint after a long quiet spell is the interesting one: it is what
    // "the editor came back" looks like in the log.
    if (mTicksSinceRedraw >= kQuietRedrawTicks)
        trace("redraw after %lu quiet ticks", mTicksSinceRedraw);
    ++mRedrawCount;
    mTicksSinceRedraw = 0;
    mDirty = false;

    // Compose into the DIB-backed surface...
    cairo_t *cr = cairo_create(mSurface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS)
        onDraw(cr);
    cairo_destroy(cr);

    // ...then blit it to the window in one operation, so no partially drawn
    // frame is ever visible.
    const HDC dc = GetDC(mWindow);
    if (dc) {
        blit(dc);
        ReleaseDC(mWindow, dc);
    }
}

//------------------------------------------------------------------------
} // namespace Steinberg
