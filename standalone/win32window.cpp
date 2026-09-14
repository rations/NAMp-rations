// Win32Window implementation. See win32window.h for the five ways Windows differs from X11 here,
// and nativewindow.h for the seam.

#include "win32window.h"

#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM

#include <cstdio>
#include <cstring>
#include <utility>

using namespace Steinberg;

namespace Rations
{

namespace
{
const wchar_t *kClassName = L"NampRackWindow";

//------------------------------------------------------------------------
// Registered once for the process. The standalone is an executable and is never unloaded, so the
// process module is the right HINSTANCE and the whole answer — unlike the plug-in side of this same
// project, which has to resolve its own module because a window class whose procedure points into a
// DLL that has been unloaded is a crash on the next dispatched message.
bool ensureWindowClass(WNDPROC proc)
{
    static const bool ok = [proc] {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // No background brush: every pixel is painted by the blit, and letting the system erase
        // first would only produce a flash of background colour.
        wc.hbrBackground = nullptr;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            fprintf(stderr, "namp-rack: cannot register the window class (error %lu)\n",
                    GetLastError());
            return false;
        }
        return true;
    }();
    return ok;
}

//------------------------------------------------------------------------
// The mapping is the SDK's own, from the comments on KeyModifier: kCommandKey is documented as
// "Windows: ctrl key" and kControlKey as "Windows: win key", so VK_CONTROL is kCommandKey here and
// the Windows key is kControlKey. Reading these the wrong way round would make Ctrl-C look like a
// plain C.
int16 currentKeyModifiers()
{
    int16 mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        mods |= kShiftKey;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        mods |= kCommandKey;
    if (GetKeyState(VK_MENU) & 0x8000)
        mods |= kAlternateKey;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        mods |= kControlKey;
    return mods;
}

//------------------------------------------------------------------------
int16 virtualKeyFromVK(int vk)
{
    switch (vk) {
        case VK_BACK:
            return KEY_BACK;
        case VK_TAB:
            return KEY_TAB;
        case VK_RETURN:
            return KEY_RETURN;
        case VK_ESCAPE:
            return KEY_ESCAPE;
        case VK_DELETE:
            return KEY_DELETE;
        case VK_LEFT:
            return KEY_LEFT;
        case VK_RIGHT:
            return KEY_RIGHT;
        case VK_UP:
            return KEY_UP;
        case VK_DOWN:
            return KEY_DOWN;
        case VK_HOME:
            return KEY_HOME;
        case VK_END:
            return KEY_END;
        case VK_PRIOR:
            return KEY_PAGEUP;
        case VK_NEXT:
            return KEY_PAGEDOWN;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// Wide from UTF-8, for the window title. A title is cosmetic, so a string that will not convert
// yields an empty one rather than a failure.
std::wstring wideFromUtf8(const std::string &text)
{
    if (text.empty())
        return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0)
        return std::wstring();
    std::wstring wide(static_cast<size_t>(need), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                            need) <= 0)
        return std::wstring();
    return wide;
}
} // namespace

//------------------------------------------------------------------------
Win32Window::Win32Window(EventLoop &loop) : mLoop(loop)
{
    // The loop is not consulted for dispatch on this platform — Windows delivers messages to the
    // window procedure itself — but it is still the object that owns the pump this window's
    // messages arrive on, so the reference is kept for symmetry with the X11 sibling and for the
    // lifetime guarantee it documents: no window here outlives the loop.
    (void)mLoop;
}

//------------------------------------------------------------------------
Win32Window::~Win32Window()
{
    destroy();
}

//------------------------------------------------------------------------
void Win32Window::outerSize(int w, int h, int &outW, int &outH) const
{
    outW = w;
    outH = h;
    if (!mWindow || !mTopLevel)
        return;

    RECT rect = {0, 0, w, h};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(mWindow, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(mWindow, GWL_EXSTYLE));
    if (AdjustWindowRectEx(&rect, style, FALSE, exStyle)) {
        outW = rect.right - rect.left;
        outH = rect.bottom - rect.top;
    }
}

//------------------------------------------------------------------------
bool Win32Window::createTopLevel(const char *title, int w, int h, Input input)
{
    if (w <= 0 || h <= 0)
        return false;
    if (mWindow)
        return true;
    if (!ensureWindowClass(&Win32Window::windowProc))
        return false;

    mTopLevel = true;
    mInput = input;

    // The client area is what the caller asked for, so the frame is added before the window is
    // created; see note 1 in the header.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect = {0, 0, w, h};
    AdjustWindowRectEx(&rect, style, FALSE, 0);

    const std::wstring wide = wideFromUtf8(title ? title : "");
    mWindow = CreateWindowExW(0, kClassName, wide.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (!mWindow) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d window (error %lu)\n", w, h,
                GetLastError());
        mTopLevel = false;
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

//------------------------------------------------------------------------
bool Win32Window::createChild(Handle parent, int x, int y, int w, int h, Input input)
{
    if (!parent || w <= 0 || h <= 0)
        return false;
    if (mWindow)
        return true;
    if (!ensureWindowClass(&Win32Window::windowProc))
        return false;

    mTopLevel = false;
    mInput = input;

    // WS_CLIPCHILDREN and WS_CLIPSIBLINGS: this window is a sibling of whatever window a hosted
    // editor made inside the same parent, and without them the two would paint over each other.
    mWindow = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, x,
                              y, w, h, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (!mWindow) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d child window (error %lu)\n", w, h,
                GetLastError());
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

//------------------------------------------------------------------------
void Win32Window::destroy()
{
    // Never leave the focus pointed at a window that is about to stop existing.
    setKeyboardFocus(false);

    if (mWindow && GetCapture() == mWindow)
        ReleaseCapture();
    mTrackingLeave = false;
    mWheelRemainder = 0;

    destroySurfaces();

    if (mWindow) {
        // Clear the back pointer FIRST: DestroyWindow dispatches WM_DESTROY synchronously, and
        // nothing in this object should be reachable from a message once teardown has begun.
        SetWindowLongPtrW(mWindow, GWLP_USERDATA, 0);
        const HWND window = mWindow;
        mWindow = nullptr;
        DestroyWindow(window);
    }
    mWidth = 0;
    mHeight = 0;
}

//------------------------------------------------------------------------
void Win32Window::setEventCallback(EventCallback callback)
{
    mCallback = std::move(callback);
}

//------------------------------------------------------------------------
void Win32Window::emit(const WindowEvent &event)
{
    if (mCallback)
        mCallback(event);
}

//------------------------------------------------------------------------
void Win32Window::setTitle(const std::string &title)
{
    if (!mWindow)
        return;
    const std::wstring wide = wideFromUtf8(title);
    SetWindowTextW(mWindow, wide.c_str());
}

//------------------------------------------------------------------------
void Win32Window::setClassHint(const char *resName, const char *resClass)
{
    // No equivalent on this platform; see the header.
    (void)resName;
    (void)resClass;
}

//------------------------------------------------------------------------
void Win32Window::setSizeHints(int minW, int minH, int maxW, int maxH)
{
    mMinW = minW;
    mMinH = minH;
    mMaxW = maxW;
    mMaxH = maxH;
}

//------------------------------------------------------------------------
void Win32Window::show()
{
    if (!mWindow)
        return;
    ShowWindow(mWindow, SW_SHOWNORMAL);
    UpdateWindow(mWindow);
}

//------------------------------------------------------------------------
void Win32Window::raise()
{
    if (!mWindow)
        return;
    // Not SetForegroundWindow: Windows refuses that from a process the user is not interacting
    // with, and a refused call would flash the taskbar button instead of raising the window.
    // Bringing it to the top of the Z order is what the X11 sibling's XRaiseWindow does.
    SetWindowPos(mWindow, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(mWindow, SW_SHOWNORMAL);
}

//------------------------------------------------------------------------
void Win32Window::resize(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return;
    int outW = w, outH = h;
    outerSize(w, h, outW, outH);
    SetWindowPos(mWindow, nullptr, 0, 0, outW, outH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

//------------------------------------------------------------------------
void Win32Window::moveResize(int x, int y, int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return;
    int outW = w, outH = h;
    outerSize(w, h, outW, outH);
    SetWindowPos(mWindow, nullptr, x, y, outW, outH, SWP_NOZORDER | SWP_NOACTIVATE);
}

//------------------------------------------------------------------------
void Win32Window::flush()
{
    // GDI batches drawing per thread, and a batched operation is not on the screen yet. The X11
    // sibling's XFlush has the same job.
    GdiFlush();
}

//------------------------------------------------------------------------
void Win32Window::sync()
{
    // Nothing to do; see the header.
}

//------------------------------------------------------------------------
bool Win32Window::createSurfaces(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return false;

    const HDC windowDC = GetDC(mWindow);
    if (!windowDC) {
        fprintf(stderr, "namp-rack: cannot obtain a device context for a window\n");
        return false;
    }
    mMemDC = CreateCompatibleDC(windowDC);
    ReleaseDC(mWindow, windowDC);
    if (!mMemDC) {
        fprintf(stderr, "namp-rack: cannot create a memory device context\n");
        return false;
    }

    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    // NEGATIVE: a DIB is bottom-up by default and cairo's image surfaces are top-down. Getting this
    // wrong produces a vertically mirrored window rather than an error.
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    mDibBits = nullptr;
    mDib = CreateDIBSection(mMemDC, &bmi, DIB_RGB_COLORS, &mDibBits, nullptr, 0);
    if (!mDib || !mDibBits) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d drawing buffer\n", w, h);
        destroySurfaces();
        return false;
    }
    mOldBitmap = SelectObject(mMemDC, mDib);

    // A 32-bit DIB's rows are 4-byte aligned by construction and cairo's ARGB32 stride is 4 bytes
    // per pixel, so these agree for every width — but the consequence of them ever disagreeing is a
    // skewed image rather than a failure, so it is checked rather than assumed.
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    if (stride != w * 4) {
        fprintf(stderr, "namp-rack: unexpected cairo stride %d for width %d (expected %d)\n",
                stride, w, w * 4);
        destroySurfaces();
        return false;
    }

    mBuffer = cairo_image_surface_create_for_data(static_cast<unsigned char *>(mDibBits),
                                                  CAIRO_FORMAT_ARGB32, w, h, stride);
    if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d drawing surface\n", w, h);
        destroySurfaces();
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

//------------------------------------------------------------------------
void Win32Window::destroySurfaces()
{
    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mMemDC) {
        // The DIB must be deselected before it can be deleted, and the DC must hold its original
        // bitmap again before it is.
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
bool Win32Window::resizeSurfaces(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return false;

    // Keep the old buffer if the new one cannot be made: drawing at the previous size is wrong but
    // survivable, whereas no buffer would stop the window painting at all for the rest of the
    // session.
    cairo_surface_t *oldSurface = mBuffer;
    HDC oldMemDC = mMemDC;
    HBITMAP oldDib = mDib;
    HGDIOBJ oldOldBitmap = mOldBitmap;

    mBuffer = nullptr;
    mMemDC = nullptr;
    mDib = nullptr;
    mOldBitmap = nullptr;
    mDibBits = nullptr;

    if (!createSurfaces(w, h)) {
        fprintf(stderr, "namp-rack: cannot resize a drawing buffer to %dx%d\n", w, h);
        mBuffer = oldSurface;
        mMemDC = oldMemDC;
        mDib = oldDib;
        mOldBitmap = oldOldBitmap;
        if (mBuffer)
            mDibBits = cairo_image_surface_get_data(mBuffer);
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
void Win32Window::blit(HDC dc)
{
    if (!mBuffer || !mMemDC || !dc)
        return;
    // Anything cairo still holds must reach the DIB's pixels before GDI reads them.
    cairo_surface_flush(mBuffer);
    BitBlt(dc, 0, 0, cairo_image_surface_get_width(mBuffer),
           cairo_image_surface_get_height(mBuffer), mMemDC, 0, 0, SRCCOPY);
}

//------------------------------------------------------------------------
void Win32Window::present()
{
    if (!mWindow || !mBuffer)
        return;
    const HDC dc = GetDC(mWindow);
    if (!dc)
        return;
    blit(dc);
    ReleaseDC(mWindow, dc);
}

//------------------------------------------------------------------------
void Win32Window::setKeyboardFocus(bool wanted)
{
    if (!mWindow || wanted == mKeyFocus)
        return;

    if (wanted) {
        // A window that is not visible cannot usefully hold the focus, and taking it would move it
        // away from something the user can actually see.
        if (!IsWindowVisible(mWindow))
            return;
        mPrevFocus = GetFocus();
        SetFocus(mWindow);
        mKeyFocus = true;
        return;
    }

    mKeyFocus = false;
    HWND prev = mPrevFocus;
    mPrevFocus = nullptr;
    // The window that had the focus may have been destroyed while we held it, so it is probed
    // rather than trusted. A failed probe leaves the focus here, which is wrong but is far better
    // than calling SetFocus on a dead handle.
    if (prev && prev != mWindow && IsWindow(prev))
        SetFocus(prev);
}

//------------------------------------------------------------------------
bool Win32Window::childGeometry(Handle child, int &w, int &h) const
{
    if (!child || !IsWindow(child))
        return false;
    RECT rect = {};
    if (!GetClientRect(child, &rect))
        return false;
    w = rect.right - rect.left;
    h = rect.bottom - rect.top;
    return w > 0 && h > 0;
}

//------------------------------------------------------------------------
void Win32Window::resizeChild(Handle child, int w, int h)
{
    if (!child || !IsWindow(child) || w <= 0 || h <= 0)
        return;
    SetWindowPos(child, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

//------------------------------------------------------------------------
void Win32Window::armMouseLeave()
{
    if (mTrackingLeave || !mWindow)
        return;
    TRACKMOUSEEVENT tme = {};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = mWindow;
    if (TrackMouseEvent(&tme))
        mTrackingLeave = true;
}

//------------------------------------------------------------------------
LRESULT CALLBACK Win32Window::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *cs = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs ? cs->lpCreateParams : nullptr));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    Win32Window *self = reinterpret_cast<Win32Window *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self || self->mWindow != window)
        return DefWindowProcW(window, message, wParam, lParam);

    return self->handleMessage(message, wParam, lParam);
}

//------------------------------------------------------------------------
// Nothing here paints a fresh frame, and nothing here decides what an event means. WM_PAINT is the
// one message that touches the screen, and it only re-blits what was composed last — see note 3 in
// the header for why it cannot simply set a flag the way an X11 Expose does.
LRESULT Win32Window::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    const bool full = mInput == Input::Full;

    switch (message) {
        case WM_ERASEBKGND:
            // Every pixel is painted by the blit, so letting the system erase first would only
            // produce a flash of background colour.
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(mWindow, &ps);
            if (dc) {
                blit(dc);
                EndPaint(mWindow, &ps);
            }
            // And ask for a fresh compose on the next tick, which is what the X11 sibling's Expose
            // does and all it does.
            WindowEvent out;
            out.kind = WindowEvent::Kind::Redraw;
            emit(out);
            return 0;
        }

        case WM_SIZE: {
            const int w = static_cast<int>(LOWORD(lParam));
            const int h = static_cast<int>(HIWORD(lParam));
            if (w <= 0 || h <= 0)
                return 0; // minimised
            mWidth = w;
            mHeight = h;
            WindowEvent out;
            out.kind = WindowEvent::Kind::Resize;
            out.width = w;
            out.height = h;
            emit(out);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            // Asked in WINDOW units, while the limits held here are client ones, so the frame goes
            // back on. This arrives before WM_NCCREATE for the window's own creation, when mWindow
            // is still null and there is no style to measure — the defaults Windows supplies are
            // right for that one call.
            if (!mWindow)
                break;
            MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
            if (mMinW > 0 && mMinH > 0) {
                int w = 0, h = 0;
                outerSize(mMinW, mMinH, w, h);
                mmi->ptMinTrackSize.x = w;
                mmi->ptMinTrackSize.y = h;
            }
            if (mMaxW > 0 && mMaxH > 0) {
                int w = 0, h = 0;
                outerSize(mMaxW, mMaxH, w, h);
                mmi->ptMaxTrackSize.x = w;
                mmi->ptMaxTrackSize.y = h;
            }
            return 0;
        }

        case WM_CLOSE: {
            // Reported, never acted on: whoever owns this window decides whether it stops existing,
            // exactly as the X11 sibling reports a WM_DELETE_WINDOW client message.
            WindowEvent out;
            out.kind = WindowEvent::Kind::Close;
            emit(out);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            if (!full)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::MouseDown;
            out.x = static_cast<float>(GET_X_LPARAM(lParam));
            out.y = static_cast<float>(GET_Y_LPARAM(lParam));
            out.button = (message == WM_LBUTTONDOWN)
                             ? kButtonLeft
                             : (message == WM_MBUTTONDOWN ? kButtonMiddle : kButtonRight);
            emit(out);
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP: {
            if (!full)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::MouseUp;
            out.x = static_cast<float>(GET_X_LPARAM(lParam));
            out.y = static_cast<float>(GET_Y_LPARAM(lParam));
            out.button = (message == WM_LBUTTONUP)
                             ? kButtonLeft
                             : (message == WM_MBUTTONUP ? kButtonMiddle : kButtonRight);
            emit(out);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!full)
                break;
            // Windows sends no "the pointer left" message unless it is asked to, and the request is
            // one-shot: it has to be re-armed after every WM_MOUSELEAVE.
            armMouseLeave();
            WindowEvent out;
            out.kind = WindowEvent::Kind::MouseMove;
            out.x = static_cast<float>(GET_X_LPARAM(lParam));
            out.y = static_cast<float>(GET_Y_LPARAM(lParam));
            // Already coalesced by the system — a WM_MOUSEMOVE is only generated when the queue has
            // none pending, so the X11 side's explicit motion compression has no counterpart here.
            emit(out);
            return 0;
        }

        case WM_MOUSELEAVE: {
            mTrackingLeave = false;
            if (!full)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::MouseLeave;
            emit(out);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!full)
                break;
            // Unlike every other mouse message, these coordinates are SCREEN relative.
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(mWindow, &pt);

            mWheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            // One message can be worth several notches, and a notch is allowed to close this window
            // — a click on a hosted plug-in's cross does exactly that. So the loop stops the moment
            // the window is gone rather than reporting into a closed one.
            while (mWheelRemainder >= WHEEL_DELTA && mWindow) {
                mWheelRemainder -= WHEEL_DELTA;
                WindowEvent out;
                out.kind = WindowEvent::Kind::Wheel;
                out.x = static_cast<float>(pt.x);
                out.y = static_cast<float>(pt.y);
                out.delta = 1;
                emit(out);
            }
            while (mWheelRemainder <= -WHEEL_DELTA && mWindow) {
                mWheelRemainder += WHEEL_DELTA;
                WindowEvent out;
                out.kind = WindowEvent::Kind::Wheel;
                out.x = static_cast<float>(pt.x);
                out.y = static_cast<float>(pt.y);
                out.delta = -1;
                emit(out);
            }
            return 0;
        }

        case WM_GETDLGCODE:
            // Ask for the keys a dialog manager would otherwise eat on our behalf. Only claimed
            // while a text field is actually open, so outside that the host's own key commands —
            // the space bar above all — are untouched.
            if (full && mKeyFocus)
                return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
            break;

        case WM_KEYDOWN: {
            if (!full)
                break;
            // The virtual keys only. A printable character arrives separately as WM_CHAR, which is
            // where the keyboard layout and the shift state have already been applied for us;
            // decoding one from a VK here would be re-implementing ToUnicode badly.
            const int16 virt = virtualKeyFromVK(static_cast<int>(wParam));
            if (virt == 0)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::Key;
            out.virtualKey = virt;
            out.modifiers = currentKeyModifiers();
            emit(out);
            return 0;
        }

        case WM_CHAR: {
            if (!full)
                break;
            // ASCII only, matching the X11 side: the one text field this reaches cannot store
            // anything else, and a control character is a key that arrived as WM_KEYDOWN already.
            const unsigned code = static_cast<unsigned>(wParam);
            if (code < 0x20 || code >= 0x7F)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::Key;
            out.character = static_cast<char16>(code);
            out.modifiers = currentKeyModifiers();
            emit(out);
            return 0;
        }

        case WM_KILLFOCUS: {
            // The focus can be taken away at any moment. Let the claim follow reality, or a later
            // release would hand focus somewhere it no longer is and steal it from whoever holds it
            // now.
            mKeyFocus = false;
            mPrevFocus = nullptr;
            if (!full)
                break;
            WindowEvent out;
            out.kind = WindowEvent::Kind::FocusLost;
            emit(out);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(mWindow, message, wParam, lParam);
}

} // namespace Rations
