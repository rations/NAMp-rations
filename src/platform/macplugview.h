// MacPlugView — reusable base class for NSView-embedded VST3 plug-in editors.
//
// Implements the kPlatformTypeNSView contract from pluginterfaces/gui/iplugview.h:
// the host passes an NSView* (as a void*) to IPlugView::attached(); the plug-in
// adds one child view to it and paints that view itself.
//
// This is the third sibling of X11PlugView and Win32PlugView and honours exactly
// the same protected hook set, so RationsEditorView compiles against any of the
// three without knowing which. Two things differ here and nowhere else, and both
// are stated plainly because they are the whole of what a reader has to hold in
// mind:
//
// 1. COORDINATES ARE LOGICAL, NOT PIXELS. The SDK is explicit: "on macOS
//    (kPlatformTypeNSView), the coordinates are expressed in logical units
//    (independent of the screen scale factor), whereas on Windows
//    (kPlatformTypeHWND) and Linux (kPlatformTypeX11EmbedWindowID), the
//    coordinates are expressed in physical units (pixels)"
//    (pluginterfaces/gui/iplugview.h:96-100). Every ViewRect crossing the host
//    boundary here — getSize, onSize, checkSizeConstraint, resizeView — is
//    therefore in POINTS, while the editor above this class thinks entirely in
//    device pixels, exactly as it does on the other two platforms.
//
//    That conversion is confined to four places and nowhere else: onSize and
//    checkSizeConstraint convert points -> pixels before calling the editor's
//    constrainSize and back afterwards; requestResize converts the editor's
//    pixels -> points; and mouse coordinates, which AppKit delivers in points,
//    are multiplied on the way in. When the backing scale is 1.0 — every
//    non-Retina Mac — all four are the identity and this file behaves exactly
//    like its Win32 sibling.
//
//    What this buys is that a Retina Mac draws the panel at 2x for free. The
//    editor's layout is one logical canvas and a single cairo_scale at compose
//    time, so handing it a buffer twice as large in each axis simply makes the
//    scale 2.0 and every glyph and every arc is resolved at device resolution.
//    Nothing in geometry.h changes, and nothing knows it happened.
//
// 2. THERE IS NO SetCapture ANALOGUE, AND NONE IS NEEDED. AppKit delivers
//    mouseDragged: to the view that received mouseDown: no matter where the
//    pointer goes, which is the implicit grab X11 gives for free and Win32 does
//    not. The Win32 sibling's mButtonsDown bookkeeping and its
//    WM_CAPTURECHANGED button-up synthesis exist only to rebuild that behaviour
//    and are deliberately NOT transliterated here — a port that copied them
//    would be carrying a workaround for a problem this platform does not have.
//
// What does NOT change is the deferred-paint discipline. Input handlers only
// ever set a dirty flag; the actual draw happens on the next timer tick and
// reaches the screen through setNeedsDisplay:. drawRect: blits the last composed
// frame and never calls onDraw(). Painting straight out of an input handler is
// how a plug-in re-enters the host's run loop, and it is the classic reason an
// editor works in one host and hangs in another.
//
// DRAWING. onDraw() paints into a Cairo image surface; drawRect: wraps that
// surface's own pixels in a CGImage and draws it with one CGContextDrawImage.
// That is the same shape as the Win32 side's DIB-plus-BitBlt and the X11 side's
// offscreen-image-plus-blit, and it is why Cairo needs no quartz backend here —
// only its image backend, which is also what keeps the rasterisation identical
// to the other two platforms and so keeps the panel pixel-diff meaningful.
//
// RESIZING. CPluginView::canResize() returns kResultFalse and its
// checkSizeConstraint() is a stub, so a resizable editor has to override all
// three of canResize / checkSizeConstraint / onSize. The size policy itself is
// not hard-coded here — a subclass opts in through isResizable() and states its
// rule in constrainSize(), which both checkSizeConstraint() (the host asking
// "may I?") and onSize() (the host saying "I did") route through, so the two can
// never disagree. Per the SDK's own instruction, the view is resized only from
// onSize().
//
// DPI. IPlugViewContentScaleSupport is deliberately NOT implemented. The SDK
// says of it: "This interface communicates the content scale factor from the
// host to the plug-in view on systems where plug-ins cannot get this information
// directly like Microsoft Windows", and "It is recommended to implement this
// interface on Microsoft Windows". macOS is not such a system — the scale is
// read from the window through backingScaleFactor and followed through
// viewDidChangeBackingProperties, which is a fact about the display rather than
// a negotiation with the host.
//
// THIS HEADER IS PLAIN C++ AND CONTAINS NO OBJECTIVE-C. rationsview.cpp is a
// .cpp and includes it through platform/plugview.h, so every AppKit pointer
// below is held as void* and every AppKit call lives in macplugview.mm. That is
// the same seam the file already draws between the editor and the window system,
// one level lower down.
//
// Subclasses must do all windowing-dependent setup in onAttached() rather than
// in their constructor, so that createView() stays harmless in headless hosts
// (the validator creates and destroys views without ever attaching them).

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

#include <cairo/cairo.h>

namespace Steinberg
{

//------------------------------------------------------------------------
class MacPlugView : public Vst::EditorView
{
public:
    MacPlugView(Vst::EditController *controller, ViewRect *size = nullptr);
    ~MacPlugView() override;

    //---from CPluginView-------------
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API attached(void *parent, FIDString type) SMTG_OVERRIDE;
    void attachedToParent() SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

    //---from IPlugView, resizing-----
    tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    tresult PLUGIN_API checkSizeConstraint(ViewRect *rect) SMTG_OVERRIDE;
    tresult PLUGIN_API onSize(ViewRect *newSize) SMTG_OVERRIDE;

    //---Interface--------------------
    OBJ_METHODS(MacPlugView, Vst::EditorView)
    DEFINE_INTERFACES
    END_DEFINE_INTERFACES(Vst::EditorView)
    REFCOUNT_METHODS(Vst::EditorView)

    // --- called by the Objective-C view, which is the only other thing that may ---
    //
    // Public because macplugview.mm's NSView subclass is a separate class rather
    // than a friend: Objective-C has no access control that C++ recognises, so a
    // protected hook could not be reached from a method body. They are not part of
    // the subclass contract and nothing in src/ outside this pair calls them.
    void macTick();
    void macDraw(void *cgContext);
    void macMouseDown(double x, double y, int button);
    void macMouseUp(double x, double y, int button);
    void macMouseMove(double x, double y);
    void macMouseLeave();
    void macMouseWheel(double x, double y, double deltaY);
    bool macKeyDown(char16 key, int16 keyCode, int16 modifiers);
    void macFocusLost();
    void macFrameChanged(double pointW, double pointH);
    void macBackingScaleChanged(double scale);

protected:
    // --- subclass hooks, all called on the host's UI thread ---

    // The view and its drawing surfaces now exist. Load resources here.
    virtual void onAttached()
    {
    }
    // The view is about to go away. Release anything tied to it.
    virtual void onRemoved()
    {
    }

    // Paint the whole editor. `cr` targets the offscreen buffer.
    virtual void onDraw(cairo_t *cr) = 0;

    // Pointer input, in DEVICE PIXELS — see the note at the top of this file.
    // Buttons are X button numbers (1 = left, 2 = middle, 3 = right) on this
    // platform too: the numbering is part of the hook contract that all three
    // platform bases honour, so the editor above never has to know which one it
    // is running on. Wheel steps arrive as delta +1 (up) or -1 (down).
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

    // Keyboard, taken from the PLATFORM view rather than handed in by the host. Return true if
    // the key was consumed. See setKeyboardFocus() in the .mm for why this exists alongside
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
    // Adjust a proposed size in place to the nearest one the editor can actually
    // draw (clamping, aspect lock, snapping). In DEVICE PIXELS, as on the other
    // two platforms. Called for both the host's "may I?" and its "I did", so
    // there is exactly one rule.
    virtual void constrainSize(int &w, int &h) const
    {
        (void)w, (void)h;
    }
    // The view's drawing surface is now `w` x `h` DEVICE PIXELS. Recompute
    // anything cached at device resolution here.
    virtual void onResized(int w, int h)
    {
        (void)w, (void)h;
    }

    // Ask the host to resize the editor's view — the plug-in-initiated half of the SDK's sizing
    // contract (pluginterfaces/gui/iplugview.h): plug-in calls IPlugFrame::resizeView, and the
    // host calls back into onSize() IN THE SAME CALLSTACK, but only if the size it settled on
    // differs from the current one. Two consequences the caller must respect:
    //
    //   * Any state that constrainSize() reads must ALREADY be updated before calling this,
    //     because onSize() re-enters through constrainSize() before this function returns.
    //   * A false return means the view did not change: either there is no IPlugFrame (a host
    //     that never attached one, or the validator) or the host refused. The caller must stay
    //     usable at the size it has rather than assuming the request landed.
    //
    // `w` and `h` are DEVICE PIXELS, as everywhere else in the hook set; they are divided by the
    // backing scale on the way out, because what the host is asked for is points. Per the SDK's
    // own instruction the native view is NOT resized here; onSize() does that.
    bool requestResize(int w, int h);

    // Take the keyboard focus, or hand it straight back to whoever had it. Called by the editor
    // around a text field, and never held for longer than one — see the .mm.
    void setKeyboardFocus(bool wanted);

    // Request a repaint on the next tick. Cheap; call it freely.
    void invalidate()
    {
        mDirty = true;
    }

    bool isWindowOpen() const
    {
        return mView != nullptr;
    }

public:
    // Read by the Objective-C view's -acceptsFirstResponder, which is how RULES.md section 4's
    // "outside that window the view holds no focus and claims no keys" is actually enforced:
    // while this is false AppKit will not even offer the focus on a click, so the host's own key
    // commands are untouched. Public for the same reason the mac* hooks above are.
    bool hasKeyboardFocus() const
    {
        return mKeyFocus;
    }

private:
    bool openView(void *parent);
    void closeView();
    bool createSurface(int w, int h);
    void destroySurface();
    bool resizeSurface(int w, int h);
    void redraw();

    // points <-> device pixels. One place each, so the two can never disagree.
    int toPixels(double points) const;
    double toPoints(int pixels) const;

    // Diagnostics, enabled by setting RATIONS_MAC_TRACE in the environment. They exist for the
    // same reason the other two platforms have them: the embedding contract is host-specific and
    // only partly written down, so when an editor misbehaves in one host the first question is
    // always "which events did we actually get, and how big did we think we were", and that has
    // to be answerable from a user's machine.
    void trace(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));

    void *mView = nullptr;      // NAMpRationsPlugView* (an NSView subclass), retained by us
    void *mTimer = nullptr;     // NSTimer*, retained by the run loop while it is valid
    void *mPrevFocus = nullptr; // NSResponder* that held the focus when we took it; not retained

    cairo_surface_t *mSurface = nullptr; // the offscreen buffer, in device pixels

    // 1.0 on every non-Retina Mac, 2.0 on a Retina one. Followed through
    // viewDidChangeBackingProperties, because a window dragged between a Retina
    // display and an external one changes it mid-session.
    double mBackingScale = 1.0;

    // The drawing surface's size in device pixels, which is the point size times
    // mBackingScale. Kept rather than re-derived so a backing-scale change can
    // tell whether the buffer actually has to be rebuilt.
    int mPixelW = 0;
    int mPixelH = 0;

    bool mKeyFocus = false;
    bool mDirty = true;

    bool mTrace = false;
    unsigned long mTickCount = 0;
    unsigned long mRedrawCount = 0;
};

//------------------------------------------------------------------------
} // namespace Steinberg
