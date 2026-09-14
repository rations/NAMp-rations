// Win32Window — one HWND, and the translation of Windows' messages into this project's own.
//
// THIS IS THE WINDOWS HALF OF A PAIR; nativewindow.h is the seam that picks between them and states
// what the two have in common. Read x11window.h first: it carries the reasoning for the verb set,
// for why the four window classes above the seam were split this way, and for what Input
// distinguishes. This file records only what Windows does differently, and there are five things.
//
// 1. A CLIENT SIZE IS NOT A WINDOW SIZE. An X window's size IS its drawable area; a Win32
// top-level's
//    is that plus a frame and a caption, and CreateWindowEx is given the outer figure. Handing it
//    the editor's own size would make every page a frame's worth too short — which shows up as the
//    panel letterboxing itself, not as an error. Every size that crosses this class is the CLIENT
//    size, as on X11, and AdjustWindowRectEx converts at the boundary. Child windows have no frame,
//    so the conversion is skipped for them rather than being a no-op that happens to work.
//
// 2. THE DRAWING BUFFER IS THE BLIT SOURCE. The X11 path composes into an image surface and copies
// it
//    to an xlib surface, because the second one draws over the wire. Here the surface is created
//    over a DIB section's own pixels and BitBlt reads those bytes directly, so present() is the
//    copy and there is no second one. The format is ARGB32 rather than RGB24 deliberately: it makes
//    the bytes identical to what the X11 build composes, since painting an ARGB32 buffer with
//    CAIRO_OPERATOR_SOURCE copies the premultiplied RGB and discards the alpha — exactly what
//    BitBlt does with the unused fourth byte of a 32-bit BI_RGB DIB.
//
// 3. WM_PAINT MUST PAINT, or Windows sends it again forever. On X11 an Expose can simply set a
// dirty
//    flag and let the tick draw. Here the paint message has to be answered, because BeginPaint /
//    EndPaint is what validates the damaged region and an unvalidated window is an immediate busy
//    loop. So WM_PAINT blits THE LAST COMPOSED FRAME and separately reports a Redraw so the owner
//    recomposes on its next tick. It never draws a fresh frame from inside the message, which is
//    precisely the re-entrancy the deferred-paint discipline exists to prevent.
//
// 4. WHEEL COORDINATES ARE SCREEN-RELATIVE, alone among the mouse messages, and the delta is a
// signed
//    accumulation in WHEEL_DELTA units rather than a notch. Both are converted here so that nothing
//    above the seam has to know it. This is one of the things this project has already been caught
//    by once on this platform.
//
// 5. THERE IS NO "POINTER LEFT" MESSAGE UNLESS IT IS ASKED FOR, and the request is one-shot:
//    TrackMouseEvent has to be re-armed after every WM_MOUSELEAVE, or a hover highlight sticks the
//    second time the pointer leaves.

#pragma once

#include "windowevent.h"
#include "eventloop.h"

#include <windows.h>

#include <cairo/cairo.h>

#include <functional>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class Win32Window
{
public:
    using Handle = HWND;

    using EventCallback = std::function<void(const WindowEvent &)>;

    enum class Input {
        // Size changes and the close request, and nothing else. For a window whose interior is a
        // hosted plug-in's editor.
        StructureOnly,
        // Damage, mouse, keys and focus changes. For a window this project draws itself.
        Full,
    };

    explicit Win32Window(EventLoop &loop);
    ~Win32Window();

    Win32Window(const Win32Window &) = delete;
    Win32Window &operator=(const Win32Window &) = delete;

    //--- lifetime -------------------------------------------------------
    // `w` and `h` are the CLIENT size wanted; see note 1 at the top of this file. Hidden until
    // show().
    bool createTopLevel(const char *title, int w, int h, Input input);
    bool createChild(Handle parent, int x, int y, int w, int h, Input input);
    void destroy();

    bool isOpen() const
    {
        return mWindow != nullptr;
    }
    Handle handle() const
    {
        return mWindow;
    }
    // What IPlugView::attached wants for this platform's window type: an HWND is already a pointer,
    // so unlike the X11 sibling there is nothing to widen.
    void *systemWindow() const
    {
        return reinterpret_cast<void *>(mWindow);
    }

    void setEventCallback(EventCallback callback);

    //--- appearance and geometry ----------------------------------------
    void setTitle(const std::string &title);
    // An X11 concept with no Windows equivalent — the window manager's class hint is what pairs a
    // window with its desktop entry's icon. Accepted and ignored, so the caller does not need to
    // know which platform it is on.
    void setClassHint(const char *resName, const char *resClass);
    // Client-area limits. Zero for either maximum means "no limit". Enforced in WM_GETMINMAXINFO,
    // which asks in WINDOW units, so the frame is added back there.
    void setSizeHints(int minW, int minH, int maxW, int maxH);

    void show();
    void raise();
    void resize(int w, int h);
    void moveResize(int x, int y, int w, int h);
    void flush();
    // X11's round trip to the server. Windows has no such queue to drain, so this does nothing; it
    // exists so the callers that need it on one platform do not have to be split.
    void sync();

    //--- drawing --------------------------------------------------------
    bool createSurfaces(int w, int h);
    bool resizeSurfaces(int w, int h);
    void destroySurfaces();
    cairo_surface_t *drawingSurface() const
    {
        return mBuffer;
    }
    void present();

    //--- keyboard focus -------------------------------------------------
    void setKeyboardFocus(bool wanted);

    //--- a hosted editor's own window inside ours -----------------------
    bool childGeometry(Handle child, int &w, int &h) const;
    void resizeChild(Handle child, int w, int h);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void emit(const WindowEvent &event);
    void blit(HDC dc);
    void armMouseLeave();
    // The outer window size that yields a `w` x `h` client area, for this window's own style.
    // Returns the input unchanged for a child, which has no frame.
    void outerSize(int w, int h, int &outW, int &outH) const;

    EventLoop &mLoop;
    Handle mWindow = nullptr;
    EventCallback mCallback;
    bool mTopLevel = false;
    Input mInput = Input::Full;

    // The DIB the surface is created over, the memory DC it is selected into, and the bitmap that
    // DC held before. See note 2 at the top of this file.
    HDC mMemDC = nullptr;
    HBITMAP mDib = nullptr;
    HGDIOBJ mOldBitmap = nullptr;
    void *mDibBits = nullptr;           // mDib's pixels, shared with mBuffer
    cairo_surface_t *mBuffer = nullptr; // cairo image surface over mDibBits
    int mWidth = 0;
    int mHeight = 0;

    int mMinW = 0, mMinH = 0;
    int mMaxW = 0, mMaxH = 0;

    bool mKeyFocus = false;
    HWND mPrevFocus = nullptr;
    bool mTrackingLeave = false;
    // Wheel deltas arrive in WHEEL_DELTA units and a slow wheel sends less than one notch at a
    // time, so the remainder is carried rather than rounded away.
    int mWheelRemainder = 0;
};

} // namespace Rations
