// X11PlugView — reusable base class for X11-embedded VST3 plug-in editors.
//
// Implements the kPlatformTypeX11EmbedWindowID contract from
// pluginterfaces/gui/iplugview.h: the host passes an X11 Window ID (as a
// void*) to IPlugView::attached(); the plug-in creates one child window inside
// it and paints that window itself. Coordinates are physical pixels.
//
// The structural point of this class is the RUN LOOP. On Linux the plug-in owns
// NO thread at all — the SDK's own comment on Linux::IRunLoop says it plainly:
// "On Linux the host has to provide this interface to the plug-in as there's no
// global event run loop defined as on other platforms." So this class:
//
//   * asks IPlugFrame for Linux::IRunLoop;
//   * registers itself as a Linux::IEventHandler on the X connection's file
//     descriptor, so the host calls onFDIsSet() when X events are readable;
//   * registers itself as a Linux::ITimerHandler at ~30 Hz, which is what
//     drives meter decay and repaints;
//   * unregisters both in removedFromParent() BEFORE tearing the window down.
//
// Painting is deliberately deferred: X events only ever set a dirty flag, and
// the actual draw happens on the next timer tick. Painting straight out of an
// event handler is how a plug-in ends up re-entering the host's run loop, and
// it is the classic reason an editor works in one host and hangs in another.
//
// Drawing is double-buffered: subclasses paint into an offscreen image surface
// through onDraw(), and the result is blitted to the window in one step.
//
// RESIZING. CPluginView::canResize() returns kResultFalse and its
// checkSizeConstraint() is a stub, so a resizable editor has to override all
// three of canResize / checkSizeConstraint / onSize. The size policy itself is
// not hard-coded here — a subclass opts in through isResizable() and states its
// rule in constrainSize(), which both checkSizeConstraint() (the host asking
// "may I?") and onSize() (the host saying "I did") route through, so the two can
// never disagree. Per the SDK's own instruction, the X window is resized only
// from onSize().
//
// Subclasses must do all windowing-dependent setup in onAttached() rather than
// in their constructor, so that createView() stays harmless in headless hosts
// (the validator creates and destroys views without ever attaching them).

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cairo/cairo.h>

#include <X11/Xlib.h>

namespace Steinberg
{

//------------------------------------------------------------------------
class X11PlugView : public Vst::EditorView, public Linux::IEventHandler, public Linux::ITimerHandler
{
public:
    X11PlugView(Vst::EditController *controller, ViewRect *size = nullptr);
    ~X11PlugView() override;

    //---from CPluginView-------------
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API attached(void *parent, FIDString type) SMTG_OVERRIDE;
    void attachedToParent() SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

    //---from IPlugView, resizing-----
    tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    tresult PLUGIN_API checkSizeConstraint(ViewRect *rect) SMTG_OVERRIDE;
    tresult PLUGIN_API onSize(ViewRect *newSize) SMTG_OVERRIDE;

    //---from Linux::IEventHandler----
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor fd) SMTG_OVERRIDE;

    //---from Linux::ITimerHandler----
    void PLUGIN_API onTimer() SMTG_OVERRIDE;

    //---Interface--------------------
    OBJ_METHODS(X11PlugView, Vst::EditorView)
    DEFINE_INTERFACES
    DEF_INTERFACE(Linux::IEventHandler)
    DEF_INTERFACE(Linux::ITimerHandler)
    END_DEFINE_INTERFACES(Vst::EditorView)
    REFCOUNT_METHODS(Vst::EditorView)

protected:
    // --- subclass hooks, all called on the host's run-loop thread ---

    // The window and its drawing surfaces now exist. Load resources here.
    virtual void onAttached()
    {
    }
    // The window is about to go away. Release anything tied to it.
    virtual void onRemoved()
    {
    }

    // Paint the whole editor. `cr` targets the offscreen buffer.
    virtual void onDraw(cairo_t *cr) = 0;

    // Pointer input. Buttons are X button numbers (1 = left, 2 = middle,
    // 3 = right). Wheel steps arrive as delta +1 (up) or -1 (down).
    virtual void onMouseDown(int x, int y, int button)
    {
        (void)x, (void)y, (void)button;
    }
    virtual void onMouseUp(int x, int y, int button)
    {
        (void)x, (void)y, (void)button;
    }
    virtual void onMouseMove(int x, int y)
    {
        (void)x, (void)y;
    }
    virtual void onMouseLeave()
    {
    }
    virtual void onMouseWheel(int x, int y, int delta)
    {
        (void)x, (void)y, (void)delta;
    }

    // Keyboard, taken from the PLATFORM window rather than handed in by the host. Return true
    // if the key was consumed. See setKeyboardFocus() in the .cpp for why this exists alongside
    // IPlugView::onKeyDown, which the SDK names as the only route and which no host tested here
    // actually uses. `key` is an ASCII character or 0; `keyCode` is a VirtualKeyCodes value from
    // pluginterfaces/base/keycodes.h or 0; `modifiers` is a KeyModifier mask. The three arguments
    // are deliberately IPlugView::onKeyDown's own, so a subclass can route both to one handler.
    virtual bool onKeyDownNative(char16 key, int16 keyCode, int16 modifiers)
    {
        (void)key, (void)keyCode, (void)modifiers;
        return false;
    }

    // ~30 Hz, immediately before a repaint is considered. Animation (meter
    // decay) belongs here.
    virtual void onTick()
    {
    }

    // --- resize policy, supplied by the subclass ---

    // Opt in to host resizing. Default is a fixed-size editor.
    virtual bool isResizable() const
    {
        return false;
    }
    // Adjust a proposed size in place to the nearest one the editor can
    // actually draw (clamping, aspect lock, snapping). Called for both the
    // host's "may I?" and its "I did", so there is exactly one rule.
    virtual void constrainSize(int &w, int &h) const
    {
        (void)w, (void)h;
    }
    // The window and its surfaces are now `w` x `h` physical pixels. Recompute
    // anything cached at device resolution here.
    virtual void onResized(int w, int h)
    {
        (void)w, (void)h;
    }

    // Ask the host to resize the editor's window — the plug-in-initiated half of the SDK's
    // sizing contract (pluginterfaces/gui/iplugview.h): plug-in calls IPlugFrame::resizeView,
    // and the host calls back into onSize() IN THE SAME CALLSTACK, but only if the size it
    // settled on differs from the current one. Two consequences the caller must respect:
    //
    //   * Any state that constrainSize() reads must ALREADY be updated before calling this,
    //     because onSize() re-enters through constrainSize() before this function returns.
    //   * A false return means the window did not change: either there is no IPlugFrame (a
    //     host that never attached one, or the validator) or the host refused. The caller must
    //     stay usable at the size it has rather than assuming the request landed.
    //
    // Per the SDK's own instruction the native window is NOT touched here; onSize() does that.
    bool requestResize(int w, int h)
    {
        if (!plugFrame)
            return false;
        ViewRect proposed(rect.left, rect.top, rect.left + w, rect.top + h);
        return plugFrame->resizeView(this, &proposed) == kResultTrue;
    }

    // Take the X input focus, or hand it straight back to whoever had it. Called by the editor
    // around a text field, and never held for longer than one — see the .cpp.
    void setKeyboardFocus(bool wanted);

    // Request a repaint on the next tick. Cheap; call it freely.
    void invalidate()
    {
        mDirty = true;
    }

    bool isWindowOpen() const
    {
        return mWindow != 0;
    }

private:
    bool openWindow(::Window parent);
    void closeWindow();
    bool resizeSurfaces(int w, int h);
    void drainEvents();
    void redraw();
    void mapWindow();
    void ensureMapped();

    // Diagnostics, enabled by setting RATIONS_X11_TRACE in the environment. They
    // exist because the embedding contract is host-specific and only partly
    // written down: when an editor misbehaves in one host, the first question
    // is always "which X events did we actually get, and was the window
    // viewable", and that has to be answerable from a user's machine.
    void trace(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void traceEvent(const XEvent &event) const;
    void traceWindowState(const char *when) const;

    ::Display *mDisplay = nullptr;
    ::Window mWindow = 0;
    // Only set when the parent had no colormap to share and we had to make one;
    // a borrowed parent colormap must not be freed.
    Colormap mOwnedColormap = None;
    Atom mXEmbedInfoAtom = None;
    cairo_surface_t *mTarget = nullptr; // xlib surface for mWindow
    cairo_surface_t *mBuffer = nullptr; // offscreen image surface

    IPtr<Linux::IRunLoop> mRunLoop;
    bool mEventHandlerRegistered = false;
    bool mTimerRegistered = false;
    bool mDirty = true;
    bool mMapped = false;
    int mTicksUnmapped = 0;

    // Keyboard focus, held only while a text field is open. mPrevFocus is whatever had the focus
    // when we took it, so it can be given back rather than left wherever we put it.
    bool mKeyFocus = false;
    ::Window mPrevFocus = None;
    int mPrevRevert = RevertToParent;

    bool mTrace = false;
    unsigned long mTickCount = 0;
    unsigned long mFdCount = 0;
    unsigned long mEventCount = 0;
    unsigned long mRedrawCount = 0;
    unsigned long mTicksSinceRedraw = 0;
};

//------------------------------------------------------------------------
} // namespace Steinberg
