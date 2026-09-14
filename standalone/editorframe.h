// EditorFrame — the IPlugFrame for the amp's own editor, and the top-level window's size policy.
//
// A view is given a frame in IPlugView::setFrame, and everything it needs from the host afterwards
// it gets through that one pointer: the run loop by queryInterface, and a resize request by
// resizeView. Both halves have to be right or the editor either never ticks or draws into the wrong
// rectangle.
//
// ONE PER VIEW, NOT ONE PER PROCESS. resizeView() must resize the window THIS view is embedded in,
// and the frame is the only context the callback gets — a shared frame would have to guess which of
// several open editors was asking. The run loop it hands back, by contrast, is deliberately the one
// shared EventLoop: there is one of those for the whole process. See eventloop.h for why the two
// are separate objects at all, and why on Windows there is no run loop to hand out.
//
// THIS IS THE AMP'S FRAME SPECIFICALLY, which is why it owns a window rather than taking a resize
// callback. A hosted plug-in's editor lives in a top-level of its own with no policy attached to
// its width, and gets a frame of its own when there is one to give it. The policy below belongs to
// this window and to no other.
//
// THE POLICY IS THIS PROJECT'S OWN AND NOT ANYTHING THE SDK ASKS FOR:
// WIDTH BELONGS TO THE WINDOW, HEIGHT BELONGS TO THE PAGE.
//
// The editor is host-resizable and a page change reshapes the window through
// IPlugFrame::resizeView, so refusing the request outright would leave the page button dead-ended —
// that is what the sibling standalone this file was ported from does, and its editor is fixed-size.
// This one grants the request, but grants HEIGHT ONLY and pins the width to whatever the window
// already had. The reason is that the top-level window is not the editor's alone: a strip listing
// the hosted plug-in chain sits directly below it as a sibling child window, sharing the same
// width, and a page change that narrowed the window would drag that strip sideways and back. So the
// width is set once, when the window is created, and a page change moves the bottom edge and
// nothing else.
//
// THAT IS THIS HOST'S POLICY AND NOT A PROPERTY OF THE EDITOR. A DAW may grant any size it likes,
// including a width the editor never asked for, and the editor's own letterbox fallback handles
// that unchanged. Nothing here is baked into the drawing code.
//
// THE STRIP IS PART OF THE WINDOW'S SHAPE, so it is worked out here and nowhere else. The top-level
// is the editor with the rack under it, and the two are one logical canvas at one scale — so the
// strip's height is not free either: it follows the WIDTH, which is the only degree of freedom the
// pair has. Every place a size arrives — a page change, the window manager, the initial embedding —
// goes through the same three lines: the window is editorH + stripHeightFor(width) tall, the editor
// gets the top of it, the strip gets the rest. Splitting that across two files is how the strip
// ends up a few pixels adrift at one scale and not another.
//
// Reference: the SDK's own editorhost sample implements the same interface at
// public.sdk/samples/vst-hosting/editorhost/source/platform/linux/.

#pragma once

#include "eventloop.h"
#include "nativewindow.h"

#include "pluginterfaces/gui/iplugview.h"

#include <functional>

namespace Rations
{

//------------------------------------------------------------------------
class EditorFrame : public Steinberg::IPlugFrame
{
public:
    explicit EditorFrame(EventLoop &loop);

    // Where the strip below the editor should be put, in PIXELS, plus the logical-to-pixel factor
    // it shares with the editor. Called on every size change and once at embedding time.
    using StripPlacement = std::function<void(int x, int y, int w, int h, double scale)>;
    // `canvasW` is the width both the editor and the strip are laid out in — one logical canvas —
    // and `stripH` the strip's height in those same units. Set before setEmbedding(); leaving it
    // unset means there is no strip and the window is the editor's alone, which is what the
    // offline tools and a bundle under a DAW get.
    void setStrip(int canvasW, int stripH, StripPlacement place);

    // The top-level window the editor was embedded into, and the view inside it. Set once, after
    // the view has attached: resizeView cannot do its job without both, and until it is called a
    // resize request is refused rather than acted on half-way. The window is BORROWED — it outlives
    // this frame, and nothing here destroys it.
    void setEmbedding(NativeWindow &window, Steinberg::IPlugView *view);

    // The strip's height at a given window width, in pixels. Public because the caller has to
    // create the top-level at the right size before there is a frame to ask, and a second spelling
    // of this arithmetic somewhere else is exactly what the note at the top of this file is about.
    int stripHeightFor(int windowW) const;

    // The window manager has resized the top-level. Runs the new size through the view's own
    // constraint and tells the view about it. A size we ourselves just applied is ignored, which
    // is what keeps this and resizeView from resizing each other in a loop.
    void windowConfigured(int w, int h);

    // The width every page is shown at, set once when the window is created and never changed by
    // a page change. Zero until setEmbedding() runs, which is what makes resizeView fall back to
    // granting whatever it was asked for before there is a window to have a width.
    int lockedWidth() const
    {
        return mLockedW;
    }

    //---from IPlugFrame--------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) SMTG_OVERRIDE;

    //---from FUnknown----------------
    // Lifetime is the owning window's, not the reference count's; see the note in eventloop.h.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    // Inert unless NAMPRACK_STANDALONE_TRACE is set. The same env-var idiom as the editor's own
    // RATIONS_X11_TRACE, and here for the same reason: whether a window manager honours a resize
    // is a question that has to be answerable from a user's machine rather than from this one.
    void trace(const char *fmt, ...) const;

    // Resize the window and remember the size, so the resize event it provokes is recognised as
    // ours rather than treated as the user dragging the frame. Both are WINDOW sizes: the strip is
    // inside them.
    void applySize(int w, int h);
    // Put the strip under the editor at whatever the window is now. Harmless when there is none.
    void placeStrip();
    // Run `rect` through the view's own size constraint until the answer stops changing. See the
    // definition for why one pass is not enough.
    bool constrainToFixedPoint(Steinberg::ViewRect &rect) const;
    // Tell the window manager the smallest size the CURRENT page can be drawn at. The view is the
    // authority on that: checkSizeConstraint clamps whatever it is given up to the page's own
    // floor, and that floor differs per page — the page that scrolls has a much shorter one,
    // because its window is allowed to be shorter than the page it shows.
    void updateSizeHints();

    EventLoop &mLoop;
    NativeWindow *mWindow = nullptr;
    Steinberg::IPlugView *mView = nullptr;
    // The WINDOW size we last asked for — editor plus strip — and the editor's own height inside
    // it. The editor's width is the window's; that is the whole policy above.
    int mAppliedW = 0;
    int mAppliedH = 0;
    int mEditorH = 0;
    // The one width, per the policy at the top of this file. Taken from the window at
    // setEmbedding() time and thereafter changed only by the user dragging the frame — never by a
    // page change, which is the whole point.
    int mLockedW = 0;
    // The last shape this frame asked the window manager for, so a target is requested once and
    // then accepted however it comes back. -1 means nothing is outstanding. See windowConfigured.
    int mRequestedW = -1;
    int mRequestedH = -1;
    // The strip. Zero height means there is none.
    int mCanvasW = 0;
    int mStripH = 0;
    StripPlacement mPlaceStrip;
    bool mTrace = false;
};

} // namespace Rations
