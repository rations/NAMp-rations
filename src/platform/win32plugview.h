// Win32PlugView — reusable base class for HWND-embedded VST3 plug-in editors.
//
// Implements the kPlatformTypeHWND contract from pluginterfaces/gui/iplugview.h:
// the host passes an HWND (as a void*) to IPlugView::attached(); the plug-in
// creates one WS_CHILD window inside it and paints that window itself.
// Coordinates are physical pixels — the SDK says so explicitly of both
// kPlatformTypeHWND and kPlatformTypeX11EmbedWindowID, which is why the
// geometry and hit-testing above this class need no platform branch.
//
// This is the sibling of X11PlugView and honours exactly the same protected
// hook set, so RationsEditorView compiles against either without knowing which.
// The two differ in one structural way, and it is worth stating plainly because
// it is most of the difference in size between the two files:
//
//   On Linux the plug-in owns no thread and no run loop. It has to borrow the
//   host's Linux::IRunLoop, register an IEventHandler on the X connection's
//   file descriptor to be told when events are readable, register an
//   ITimerHandler for the repaint tick, and unregister both before tearing the
//   window down. On Windows all of that IS the platform: the host pumps
//   messages, the window procedure below is called with them, and the tick is
//   a SetTimer. Nothing is registered with the host and nothing has to be
//   unregistered from it. Linux::IRunLoop does not exist on this platform.
//
// What does NOT change is the deferred-paint discipline. Input messages only
// ever set a dirty flag; the actual draw happens on the next WM_TIMER. Painting
// straight out of an input message is how a plug-in re-enters the host's
// message loop, and it is the classic reason an editor works in one host and
// hangs in another. WM_PAINT blits the last composed frame and never calls
// onDraw().
//
// Drawing is double-buffered through a DIB section: onDraw() paints into a
// Cairo image surface that is backed by the DIB's own pixels, and the DIB is
// blitted to the window with one BitBlt. That is the same shape as the X11
// version's offscreen-image-plus-blit, with one copy fewer, and it is why Cairo
// needs no win32 backend here — only its image backend.
//
// RESIZING. CPluginView::canResize() returns kResultFalse and its
// checkSizeConstraint() is a stub, so a resizable editor has to override all
// three of canResize / checkSizeConstraint / onSize. The size policy itself is
// not hard-coded here — a subclass opts in through isResizable() and states its
// rule in constrainSize(), which both checkSizeConstraint() (the host asking
// "may I?") and onSize() (the host saying "I did") route through, so the two can
// never disagree. Per the SDK's own instruction, the window is resized only
// from onSize().
//
// DPI. IPlugViewContentScaleSupport is implemented here and not on Linux,
// because it exists for exactly this platform — the SDK: "This interface
// communicates the content scale factor from the host to the plug-in view on
// systems where plug-ins cannot get this information directly like Microsoft
// Windows", and "It is recommended to implement this interface on Microsoft
// Windows". A scale factor is turned into a resize of the base size and handed
// back to the host through IPlugFrame::resizeView, which is all a view whose
// layout is resolution-independent has to do.
//
// Subclasses must do all windowing-dependent setup in onAttached() rather than
// in their constructor, so that createView() stays harmless in headless hosts
// (the validator creates and destroys views without ever attaching them).

#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include <cairo/cairo.h>

// Before <windows.h>, not after: WIN32_LEAN_AND_MEAN keeps the sockets, RPC and
// OLE headers out of every translation unit that draws the editor, and NOMINMAX
// stops the min/max macros from breaking std::min / std::max at their call
// sites. This header is included last by rationsview.h for the same reason — the
// project's own headers are parsed before any of this arrives.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Steinberg
{

//------------------------------------------------------------------------
class Win32PlugView : public Vst::EditorView, public IPlugViewContentScaleSupport
{
public:
    Win32PlugView(Vst::EditController *controller, ViewRect *size = nullptr);
    ~Win32PlugView() override;

    //---from CPluginView-------------
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
    tresult PLUGIN_API attached(void *parent, FIDString type) SMTG_OVERRIDE;
    void attachedToParent() SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

    //---from IPlugView, resizing-----
    tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    tresult PLUGIN_API checkSizeConstraint(ViewRect *rect) SMTG_OVERRIDE;
    tresult PLUGIN_API onSize(ViewRect *newSize) SMTG_OVERRIDE;

    //---from IPlugViewContentScaleSupport---
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) SMTG_OVERRIDE;

    //---Interface--------------------
    OBJ_METHODS(Win32PlugView, Vst::EditorView)
    DEFINE_INTERFACES
    DEF_INTERFACE(IPlugViewContentScaleSupport)
    END_DEFINE_INTERFACES(Vst::EditorView)
    REFCOUNT_METHODS(Vst::EditorView)

protected:
    // --- subclass hooks, all called on the host's UI thread ---

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
    // 3 = right) on this platform too: the numbering is part of the hook
    // contract that both platform bases honour, so the editor above never has
    // to know which one it is running on. Wheel steps arrive as delta +1 (up)
    // or -1 (down).
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

    // Request a repaint on the next tick. Cheap; call it freely.
    void invalidate()
    {
        mDirty = true;
    }

    bool isWindowOpen() const
    {
        return mWindow != nullptr;
    }

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT wndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    bool openWindow(HWND parent);
    void closeWindow();
    bool createSurfaces(int w, int h);
    void destroySurfaces();
    bool resizeSurfaces(int w, int h);
    void followSize(int w, int h); // adopt a size the host imposed directly
    void redraw();
    void blit(HDC dc);

    void pressButton(int button, int x, int y);
    void releaseButton(int button, int x, int y);
    void releaseAllButtons(); // capture lost: end every drag that is open
    void armMouseLeave();

    // Diagnostics, enabled by setting RATIONS_WIN_TRACE in the environment. They
    // exist for the same reason the X11 side has them: the embedding contract
    // is host-specific and only partly written down, so when an editor
    // misbehaves in one host the first question is always "which messages did
    // we actually get, and was the window visible", and that has to be
    // answerable from a user's machine.
    //
    // gnu_printf rather than printf: this builds against MinGW, whose default
    // printf attribute checks arguments against the msvcrt runtime's narrower
    // conversions and would reject the C99 ones libstdc++ enables here.
    void trace(const char *fmt, ...) const __attribute__((format(gnu_printf, 2, 3)));
    void traceWindowState(const char *when) const;

    HWND mWindow = nullptr;
    HDC mMemDC = nullptr;                // holds mDib; the source of every BitBlt
    HBITMAP mDib = nullptr;              // top-down 32-bit DIB section
    HGDIOBJ mOldBitmap = nullptr;        // whatever mMemDC held before mDib
    void *mDibBits = nullptr;            // mDib's pixels, shared with mSurface
    cairo_surface_t *mSurface = nullptr; // Cairo image surface over mDibBits

    UINT_PTR mTimerId = 0;

    // The 100% size, captured at construction, so a content scale factor is
    // applied to the design size rather than compounding on the current one.
    int mBaseW = 0;
    int mBaseH = 0;
    ScaleFactor mContentScale = 1.0f;

    // Which mouse buttons we believe are down, as a bit per button number.
    // Windows has no implicit pointer grab, so this is what SetCapture is
    // driven from, and what a lost capture has to unwind.
    unsigned mButtonsDown = 0;
    int mLastMouseX = 0;
    int mLastMouseY = 0;
    bool mTrackingLeave = false;
    // WM_MOUSEWHEEL deltas are a multiple of WHEEL_DELTA on an ordinary mouse
    // but smaller on a high-resolution one, so they accumulate into notches.
    int mWheelRemainder = 0;

    bool mDirty = true;

    bool mTrace = false;
    unsigned long mTickCount = 0;
    unsigned long mMessageCount = 0;
    unsigned long mRedrawCount = 0;
    unsigned long mTicksSinceRedraw = 0;
};

//------------------------------------------------------------------------
} // namespace Steinberg
