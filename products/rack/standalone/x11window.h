// X11Window — one X window, and the translation of X's events into this project's own.
//
// THIS IS THE X11 HALF OF A PAIR; nativewindow.h is the seam that picks between them and states
// what the two have in common. Everything above the seam — the rack strip, the parameter panel, the
// window a hosted editor is embedded in, and the standalone's own top-level — is written once
// against the verb set below and never learns which windowing system is underneath it.
//
// WHAT THIS CLASS IS FOR, AND WHY IT WAS SPLIT OUT OF THE WINDOWS ABOVE IT. Before the Windows port
// there was one platform, so each window class opened its own X window inline and read XEvents in
// its own handler. Four classes did that, and every one of them mixed two entirely different kinds
// of code: which X call creates a window, and what a click on the third gear icon MEANS. The second
// kind is identical on every platform and is most of the code; the first kind is none of it. So the
// first kind moved here and the second stayed where it was — which is why the Windows port adds one
// window implementation rather than four.
//
// THE INTERIOR MAY BELONG TO SOMEBODY ELSE, which is what Input distinguishes. A window this
// project draws itself wants damage, mouse and (around an open text field, and only then) keys. A
// window whose interior is handed to a hosted plug-in's editor wants none of that: the plug-in
// opens its own connection to the display and receives events for its own child windows there, so
// asking for them here would at best duplicate what it already gets and at worst take input away
// from it.
//
// PAINTING IS TWO SURFACES AND ONE BLIT, and the reason is the deferred-paint discipline every
// window in this project follows: an event never paints, it sets a dirty flag, and the timer tick
// composes and presents. drawingSurface() is an offscreen image surface, present() puts it on the
// screen in one operation, and no partially drawn frame is ever visible. The X11 path needs a real
// copy for that, because an xlib surface draws over the wire; the Windows path composes straight
// into the bytes its blit will read and so needs none. That difference is exactly the sort of thing
// this seam exists to keep out of the drawing code.

#pragma once

#include "windowevent.h"
#include "eventloop.h"

#include <X11/Xlib.h>

#include <cairo/cairo.h>

#include <functional>
#include <string>

namespace Rations
{

//------------------------------------------------------------------------
class X11Window
{
public:
    // The X id of a window. Opaque above the seam: it is passed back in as a parent, handed to a
    // hosted plug-in as the window to attach to, and compared against zero. Nothing else.
    using Handle = ::Window;

    using EventCallback = std::function<void(const WindowEvent &)>;

    // What the window asks the system for. See the note at the top of this file.
    enum class Input {
        // Size changes and the close request, and nothing else. For a window whose interior is a
        // hosted plug-in's editor.
        StructureOnly,
        // Damage, mouse, keys and focus changes. For a window this project draws itself.
        Full,
    };

    explicit X11Window(EventLoop &loop);
    ~X11Window();

    X11Window(const X11Window &) = delete;
    X11Window &operator=(const X11Window &) = delete;

    //--- lifetime -------------------------------------------------------
    // A top-level window, unmapped: call show() when its surfaces are ready. Registers the close
    // protocol, so a Close event arrives instead of the window manager killing the connection.
    bool createTopLevel(const char *title, int w, int h, Input input);
    // A child of `parent` at `x`, `y`. Also unmapped. The rack strip is one of these: a sibling of
    // whatever window the editor made inside the same top-level, so the two input paths never have
    // to be told apart.
    bool createChild(Handle parent, int x, int y, int w, int h, Input input);
    // Idempotent, and safe to call from inside this window's own event callback.
    void destroy();

    bool isOpen() const
    {
        return mWindow != 0;
    }
    Handle handle() const
    {
        return mWindow;
    }
    // What IPlugView::attached wants for this platform's window type. An X id is not a pointer, so
    // it is widened through an integer rather than cast from one — the two are different sizes on a
    // 32-bit build and reinterpreting the id directly would truncate it.
    void *systemWindow() const
    {
        return reinterpret_cast<void *>(static_cast<uintptr_t>(mWindow));
    }

    // Replaces any previous callback. Events stop arriving once the window is destroyed.
    void setEventCallback(EventCallback callback);

    //--- appearance and geometry ----------------------------------------
    void setTitle(const std::string &title);
    // So the desktop entry's StartupWMClass matches and the window gets the right icon. Meaningful
    // on X11 only; the Windows sibling has no equivalent and ignores it.
    void setClassHint(const char *resName, const char *resClass);
    // Zero for either maximum means "no limit". Called again whenever the floor moves, which it
    // does on a page change: the page that scrolls has a much shorter one.
    void setSizeHints(int minW, int minH, int maxW, int maxH);

    void show();
    void raise();
    void resize(int w, int h);
    void moveResize(int x, int y, int w, int h);
    void flush();
    // Flush and wait for the server to have processed it. Used where the next thing to happen is
    // somebody else's code touching this window, and a request still in our output buffer would
    // mean they touched a window that does not exist yet.
    void sync();

    //--- drawing --------------------------------------------------------
    bool createSurfaces(int w, int h);
    bool resizeSurfaces(int w, int h);
    void destroySurfaces();
    // Where the window's contents are composed: a top-down ARGB32 image surface at the size last
    // passed to createSurfaces/resizeSurfaces. Null when the window has none, which is the normal
    // state for a window whose interior belongs to a hosted editor.
    cairo_surface_t *drawingSurface() const
    {
        return mBuffer;
    }
    // Put what has been composed on the screen, in one operation.
    void present();

    //--- keyboard focus -------------------------------------------------
    // Taken only while a text field is open, and handed back to whoever held it the moment the
    // field closes. NEVER GRABS: a focus request the window manager declines simply leaves the
    // field untyped. Idempotent. A FocusLost event drops the claim, so the focus is never handed
    // back after it has already been taken away.
    void setKeyboardFocus(bool wanted);

    //--- a hosted editor's own window inside ours -----------------------
    // The size a plug-in's editor made its own child window, which is what the window around it is
    // then sized to. False if it cannot be read, which includes the window having gone away.
    bool childGeometry(Handle child, int &w, int &h) const;
    void resizeChild(Handle child, int w, int h);

private:
    void onXEvent(const XEvent &event);
    ::Display *display() const;

    EventLoop &mLoop;
    Handle mWindow = 0;
    Atom mWmDelete = 0;
    EventCallback mCallback;

    // The window itself, drawn over the wire, and the offscreen buffer composed into. See the
    // painting note at the top of this file.
    cairo_surface_t *mTarget = nullptr;
    cairo_surface_t *mBuffer = nullptr;
    int mWidth = 0;
    int mHeight = 0;

    // Focus bookkeeping. mPrevFocus is whatever held the focus when this window took it, so it can
    // be handed straight back; PointerRoot and None are legal values in their own right.
    bool mKeyFocus = false;
    Handle mPrevFocus = 0;
    int mPrevRevert = RevertToParent;
};

} // namespace Rations
