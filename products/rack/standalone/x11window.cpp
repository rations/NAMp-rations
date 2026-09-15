// X11Window implementation. See x11window.h for the verb set and nativewindow.h for the seam.

#include "x11window.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cairo/cairo-xlib.h>

#include <cstdint>
#include <cstdio>
#include <utility>

namespace Rations
{

namespace
{
// X reports wheel notches as button presses. They are translated into their own event kind here
// rather than passed up as buttons, so that nothing above the seam has to know that X does this and
// Windows does not.
constexpr unsigned kButtonWheelUp = 4;
constexpr unsigned kButtonWheelDown = 5;

//------------------------------------------------------------------------
// Ported from the editor's own X11 view, which had to answer the same question first. The mapping
// of X modifier masks onto KeyModifier is the part worth reading twice: KeyModifier documents
// kCommandKey as "Windows: ctrl key" and kControlKey as "Windows: win key", so ControlMask is
// kCommandKey here and Mod4 (Super) is kControlKey. The other way round makes Ctrl-C read as a
// plain C.
Steinberg::int16 virtualKeyFromKeySym(KeySym sym)
{
    switch (sym) {
        case XK_BackSpace:
            return Steinberg::KEY_BACK;
        case XK_Tab:
            return Steinberg::KEY_TAB;
        case XK_Return:
            return Steinberg::KEY_RETURN;
        case XK_KP_Enter:
            return Steinberg::KEY_ENTER;
        case XK_Escape:
            return Steinberg::KEY_ESCAPE;
        case XK_Delete:
        case XK_KP_Delete:
            return Steinberg::KEY_DELETE;
        case XK_Left:
        case XK_KP_Left:
            return Steinberg::KEY_LEFT;
        case XK_Right:
        case XK_KP_Right:
            return Steinberg::KEY_RIGHT;
        case XK_Up:
        case XK_KP_Up:
            return Steinberg::KEY_UP;
        case XK_Down:
        case XK_KP_Down:
            return Steinberg::KEY_DOWN;
        case XK_Home:
        case XK_KP_Home:
            return Steinberg::KEY_HOME;
        case XK_End:
        case XK_KP_End:
            return Steinberg::KEY_END;
        case XK_Prior:
        case XK_KP_Prior:
            return Steinberg::KEY_PAGEUP;
        case XK_Next:
        case XK_KP_Next:
            return Steinberg::KEY_PAGEDOWN;
        default:
            return 0;
    }
}

//------------------------------------------------------------------------
// SubstructureNotifyMask IS INCLUDED DELIBERATELY, EVEN FOR A WINDOW THAT ASKS FOR NOTHING ELSE,
// and the filter in onXEvent is what makes it safe. Both halves have to stay, and dropping either
// is a bug that presents as an infinite resize loop rather than as an error:
//
// Events are dispatched by XEvent::xany.window, which for a ConfigureNotify is the xconfigure.EVENT
// field — "window on which event was requested in event mask" (X11/Xlib.h) — not the window that
// actually changed size. With this mask a window is also told when its CHILDREN resize, and on
// those the event field is still this window while xconfigure.window is the child and
// xconfigure.width/height are the CHILD's new size. Feeding that back as this window's size makes
// an embedded editor resize its child to fit a size that was its child's, and the two then
// oscillate for as long as the program runs. Measured, on the standalone's own top-level: 800x285
// and 748x266 alternating forever.
long inputMaskFor(X11Window::Input input)
{
    const long structure = StructureNotifyMask | SubstructureNotifyMask;
    if (input == X11Window::Input::StructureOnly)
        return structure;

    // KeyPressMask and FocusChangeMask claim no keys on their own: X routes a key to a window only
    // while it holds the input focus, and the focus is taken only around an open text field. See
    // setKeyboardFocus.
    return structure | ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
           LeaveWindowMask | KeyPressMask | FocusChangeMask;
}
} // namespace

//------------------------------------------------------------------------
X11Window::X11Window(EventLoop &loop) : mLoop(loop)
{
}

//------------------------------------------------------------------------
X11Window::~X11Window()
{
    destroy();
}

//------------------------------------------------------------------------
::Display *X11Window::display() const
{
    return mLoop.display();
}

//------------------------------------------------------------------------
bool X11Window::createTopLevel(const char *title, int w, int h, Input input)
{
    ::Display *dpy = display();
    if (!dpy || w <= 0 || h <= 0)
        return false;
    if (mWindow)
        return true;

    const int screen = DefaultScreen(dpy);
    mWindow = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, static_cast<unsigned>(w),
                                  static_cast<unsigned>(h), 0, BlackPixel(dpy, screen),
                                  BlackPixel(dpy, screen));
    if (!mWindow)
        return false;

    if (title)
        XStoreName(dpy, mWindow, title);
    XSelectInput(dpy, mWindow, inputMaskFor(input));

    // So a close request arrives as an event instead of the window manager killing the connection.
    mWmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, mWindow, &mWmDelete, 1);

    mWidth = w;
    mHeight = h;
    mLoop.addWindow(mWindow, [this](const XEvent &event) { onXEvent(event); });
    return true;
}

//------------------------------------------------------------------------
bool X11Window::createChild(Handle parent, int x, int y, int w, int h, Input input)
{
    ::Display *dpy = display();
    if (!dpy || !parent || w <= 0 || h <= 0)
        return false;
    if (mWindow)
        return true;

    const int screen = DefaultScreen(dpy);
    mWindow =
        XCreateSimpleWindow(dpy, parent, x, y, static_cast<unsigned>(w), static_cast<unsigned>(h),
                            0, BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    if (!mWindow)
        return false;

    XSelectInput(dpy, mWindow, inputMaskFor(input));
    mWidth = w;
    mHeight = h;
    mLoop.addWindow(mWindow, [this](const XEvent &event) { onXEvent(event); });
    return true;
}

//------------------------------------------------------------------------
void X11Window::destroy()
{
    // Never leave the focus pointed at a window that is about to stop existing.
    setKeyboardFocus(false);

    destroySurfaces();

    if (mWindow) {
        // Stop dispatching to this window BEFORE it is destroyed. removeWindow is safe to call from
        // inside a callback, including this window's own, which is what makes a window able to
        // close itself in response to its own close event.
        mLoop.removeWindow(mWindow);
        if (::Display *dpy = display()) {
            XDestroyWindow(dpy, mWindow);
            XFlush(dpy);
        }
        mWindow = 0;
    }
    mWmDelete = 0;
    mWidth = 0;
    mHeight = 0;
}

//------------------------------------------------------------------------
void X11Window::setEventCallback(EventCallback callback)
{
    mCallback = std::move(callback);
}

//------------------------------------------------------------------------
void X11Window::setTitle(const std::string &title)
{
    if (::Display *dpy = display(); dpy && mWindow)
        XStoreName(dpy, mWindow, title.c_str());
}

//------------------------------------------------------------------------
void X11Window::setClassHint(const char *resName, const char *resClass)
{
    ::Display *dpy = display();
    if (!dpy || !mWindow || !resName || !resClass)
        return;

    // XSetClassHint takes non-const char*, so the strings are copied into buffers it may write to
    // rather than casting away const on a literal.
    std::string name(resName);
    std::string cls(resClass);
    XClassHint hint = {};
    hint.res_name = name.data();
    hint.res_class = cls.data();
    XSetClassHint(dpy, mWindow, &hint);
}

//------------------------------------------------------------------------
void X11Window::setSizeHints(int minW, int minH, int maxW, int maxH)
{
    ::Display *dpy = display();
    if (!dpy || !mWindow)
        return;

    // A range rather than a fixed size, and no PAspect anywhere near it: an aspect hint plus a
    // program that resizes itself is how a window ends up oscillating between two sizes.
    XSizeHints hints = {};
    if (minW > 0 && minH > 0) {
        hints.flags |= PMinSize;
        hints.min_width = minW;
        hints.min_height = minH;
    }
    if (maxW > 0 && maxH > 0) {
        hints.flags |= PMaxSize;
        hints.max_width = maxW;
        hints.max_height = maxH;
    }
    if (hints.flags != 0)
        XSetWMNormalHints(dpy, mWindow, &hints);
}

//------------------------------------------------------------------------
void X11Window::show()
{
    if (::Display *dpy = display(); dpy && mWindow) {
        XMapWindow(dpy, mWindow);
        XFlush(dpy);
    }
}

//------------------------------------------------------------------------
void X11Window::raise()
{
    if (::Display *dpy = display(); dpy && mWindow) {
        XRaiseWindow(dpy, mWindow);
        XFlush(dpy);
    }
}

//------------------------------------------------------------------------
void X11Window::resize(int w, int h)
{
    if (::Display *dpy = display(); dpy && mWindow && w > 0 && h > 0) {
        XResizeWindow(dpy, mWindow, static_cast<unsigned>(w), static_cast<unsigned>(h));
        XFlush(dpy);
    }
}

//------------------------------------------------------------------------
void X11Window::moveResize(int x, int y, int w, int h)
{
    if (::Display *dpy = display(); dpy && mWindow && w > 0 && h > 0) {
        XMoveResizeWindow(dpy, mWindow, x, y, static_cast<unsigned>(w), static_cast<unsigned>(h));
        XFlush(dpy);
    }
}

//------------------------------------------------------------------------
void X11Window::flush()
{
    if (::Display *dpy = display())
        XFlush(dpy);
}

//------------------------------------------------------------------------
void X11Window::sync()
{
    if (::Display *dpy = display())
        XSync(dpy, False);
}

//------------------------------------------------------------------------
bool X11Window::createSurfaces(int w, int h)
{
    ::Display *dpy = display();
    if (!dpy || !mWindow || w <= 0 || h <= 0)
        return false;

    const int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    mTarget = cairo_xlib_surface_create(dpy, mWindow, visual, w, h);
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d window surface\n", w, h);
        destroySurfaces();
        return false;
    }
    mWidth = w;
    mHeight = h;
    return resizeSurfaces(w, h);
}

//------------------------------------------------------------------------
// The xlib surface wraps a window we resized, so it is told its new size in place; the offscreen
// buffer has a fixed allocation and has to be rebuilt. Same split as the plug-in's own view.
bool X11Window::resizeSurfaces(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;

    if (mTarget)
        cairo_xlib_surface_set_size(mTarget, w, h);

    if (mBuffer)
        cairo_surface_destroy(mBuffer);
    mBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(mBuffer) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "namp-rack: cannot create a %dx%d drawing buffer\n", w, h);
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

//------------------------------------------------------------------------
void X11Window::destroySurfaces()
{
    if (mBuffer) {
        cairo_surface_destroy(mBuffer);
        mBuffer = nullptr;
    }
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
}

//------------------------------------------------------------------------
// One blit, so no partially drawn frame is ever visible. CAIRO_OPERATOR_SOURCE rather than the
// default OVER: the buffer is opaque and compositing it over the previous frame would both cost
// more and leave anything the new frame did not cover.
void X11Window::present()
{
    if (!mBuffer || !mTarget)
        return;

    cairo_t *out = cairo_create(mTarget);
    if (cairo_status(out) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(out, mBuffer, 0.0, 0.0);
        cairo_paint(out);
    }
    cairo_destroy(out);

    cairo_surface_flush(mTarget);
    flush();
}

//------------------------------------------------------------------------
void X11Window::setKeyboardFocus(bool wanted)
{
    ::Display *dpy = display();
    if (!dpy || !mWindow || wanted == mKeyFocus)
        return;

    if (wanted) {
        // XSetInputFocus on a window that is not viewable is a BadMatch.
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, mWindow, &attrs) == 0 || attrs.map_state != IsViewable)
            return;
        Handle focus = 0;
        int revert = RevertToParent;
        XGetInputFocus(dpy, &focus, &revert);
        mPrevFocus = focus;
        mPrevRevert = revert;
        XSetInputFocus(dpy, mWindow, RevertToParent, CurrentTime);
        XFlush(dpy);
        mKeyFocus = true;
        return;
    }

    mKeyFocus = false;
    const Handle prev = mPrevFocus;
    mPrevFocus = 0;
    // PointerRoot and None are legal focus values in their own right and are handed back as they
    // are; a real window may have been destroyed while we held the focus, so it is probed first
    // rather than trusted. A failed probe leaves the focus here — wrong, but far better than
    // pointing it at a dead id.
    if (prev == PointerRoot || prev == None) {
        XSetInputFocus(dpy, prev, mPrevRevert, CurrentTime);
    } else if (prev != mWindow) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, prev, &attrs) != 0)
            XSetInputFocus(dpy, prev, mPrevRevert, CurrentTime);
    }
    XFlush(dpy);
}

//------------------------------------------------------------------------
bool X11Window::childGeometry(Handle child, int &w, int &h) const
{
    ::Display *dpy = display();
    if (!dpy || !child)
        return false;

    Handle root = 0;
    int x = 0, y = 0;
    unsigned cw = 0, ch = 0, border = 0, depth = 0;
    if (!XGetGeometry(dpy, child, &root, &x, &y, &cw, &ch, &border, &depth) || cw == 0 || ch == 0)
        return false;
    w = static_cast<int>(cw);
    h = static_cast<int>(ch);
    return true;
}

//------------------------------------------------------------------------
void X11Window::resizeChild(Handle child, int w, int h)
{
    if (::Display *dpy = display(); dpy && child && w > 0 && h > 0) {
        XResizeWindow(dpy, child, static_cast<unsigned>(w), static_cast<unsigned>(h));
        XFlush(dpy);
    }
}

//------------------------------------------------------------------------
// Nothing here paints, and nothing here decides what an event means. Every branch turns one XEvent
// into one WindowEvent and hands it on; the deferred-paint discipline lives in the window classes
// above, which set a dirty flag and draw on the timer tick.
void X11Window::onXEvent(const XEvent &event)
{
    if (!mCallback)
        return;

    WindowEvent out;

    switch (event.type) {
        case Expose:
            out.kind = WindowEvent::Kind::Redraw;
            break;

        case ConfigureNotify:
            // THE FILTER IS LOAD-BEARING; see the note on inputMaskFor. An event reported against
            // this window may be describing a CHILD's new size, and passing that up as ours is a
            // measured infinite resize loop.
            if (event.xconfigure.window != mWindow)
                return;
            out.kind = WindowEvent::Kind::Resize;
            out.width = event.xconfigure.width;
            out.height = event.xconfigure.height;
            mWidth = out.width;
            mHeight = out.height;
            break;

        case ClientMessage:
            if (mWmDelete == 0 || static_cast<Atom>(event.xclient.data.l[0]) != mWmDelete)
                return;
            out.kind = WindowEvent::Kind::Close;
            break;

        case FocusOut:
            // The focus can be taken away by the window manager at any moment. Let the claim follow
            // reality, or a later release would hand focus somewhere it no longer is and steal it
            // from whoever holds it now.
            mKeyFocus = false;
            mPrevFocus = 0;
            out.kind = WindowEvent::Kind::FocusLost;
            break;

        case LeaveNotify:
            out.kind = WindowEvent::Kind::MouseLeave;
            break;

        case MotionNotify:
            out.kind = WindowEvent::Kind::MouseMove;
            out.x = static_cast<float>(event.xmotion.x);
            out.y = static_cast<float>(event.xmotion.y);
            break;

        case KeyPress: {
            // XLookupString applies the shift and lock state and yields the Latin-1 byte, which is
            // why the character comes from it rather than from the keysym by hand. One byte is
            // asked for because the field is ASCII only; a longer answer is a multi-byte character
            // the field cannot store.
            XKeyEvent ke = event.xkey;
            char text[8] = {0};
            KeySym sym = NoSymbol;
            const int n = XLookupString(&ke, text, sizeof(text) - 1, &sym, nullptr);
            const unsigned char byte = (n >= 1) ? static_cast<unsigned char>(text[0]) : 0;

            out.kind = WindowEvent::Kind::Key;
            out.character =
                (byte >= 0x20 && byte < 0x7F) ? static_cast<Steinberg::char16>(byte) : 0;
            out.virtualKey = virtualKeyFromKeySym(sym);
            if (ke.state & ShiftMask)
                out.modifiers |= Steinberg::kShiftKey;
            if (ke.state & ControlMask)
                out.modifiers |= Steinberg::kCommandKey;
            if (ke.state & Mod1Mask)
                out.modifiers |= Steinberg::kAlternateKey;
            if (ke.state & Mod4Mask)
                out.modifiers |= Steinberg::kControlKey;
            break;
        }

        case ButtonPress: {
            const unsigned button = event.xbutton.button;
            out.x = static_cast<float>(event.xbutton.x);
            out.y = static_cast<float>(event.xbutton.y);
            if (button == kButtonWheelUp || button == kButtonWheelDown) {
                out.kind = WindowEvent::Kind::Wheel;
                out.delta = (button == kButtonWheelUp) ? 1 : -1;
                break;
            }
            out.kind = WindowEvent::Kind::MouseDown;
            out.button = static_cast<int>(button);
            break;
        }

        case ButtonRelease:
            out.kind = WindowEvent::Kind::MouseUp;
            out.x = static_cast<float>(event.xbutton.x);
            out.y = static_cast<float>(event.xbutton.y);
            out.button = static_cast<int>(event.xbutton.button);
            break;

        default:
            return;
    }

    mCallback(out);
}

} // namespace Rations
