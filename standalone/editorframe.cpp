// EditorFrame implementation. See editorframe.h.

#include "editorframe.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
EditorFrame::EditorFrame(EventLoop &loop) : mLoop(loop)
{
    mTrace = std::getenv("NAMPRACK_STANDALONE_TRACE") != nullptr;
}

//------------------------------------------------------------------------
void EditorFrame::trace(const char *fmt, ...) const
{
    if (!mTrace)
        return;
    va_list args;
    va_start(args, fmt);
    fputs("namp-rack: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

//------------------------------------------------------------------------
// The one place the two scopes meet. A plug-in holds a frame pointer and asks IT for the run loop,
// so the frame answers for an object it does not own — which is exactly right: there is one loop
// per process and this hands out a borrowed pointer to it.
tresult PLUGIN_API EditorFrame::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

#if !SMTG_OS_WINDOWS
    // Offered on this platform only; see the same guard in plugframe.cpp for why, and eventloop.h
    // for what Windows has instead.
    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
        return mLoop.queryInterface(iid, obj);
#endif

    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        addRef();
        return kResultOk;
    }

    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
void EditorFrame::setStrip(int canvasW, int stripH, StripPlacement place)
{
    mCanvasW = canvasW > 0 ? canvasW : 0;
    mStripH = stripH > 0 ? stripH : 0;
    mPlaceStrip = std::move(place);
}

//------------------------------------------------------------------------
// The strip is laid out in the same logical units as the editor and drawn at the same scale, so its
// pixel height is its logical height times the window's scale — and the window's scale is its width
// over the canvas width. Rounded rather than truncated: a strip a pixel short leaves a line of the
// window's own background showing under it, which reads as a gap rather than as rounding.
int EditorFrame::stripHeightFor(int windowW) const
{
    if (mStripH <= 0 || mCanvasW <= 0 || windowW <= 0)
        return 0;
    return static_cast<int>(
        std::lround(static_cast<double>(mStripH) * windowW / static_cast<double>(mCanvasW)));
}

//------------------------------------------------------------------------
void EditorFrame::placeStrip()
{
    if (!mPlaceStrip || mAppliedW <= 0 || mEditorH <= 0)
        return;
    const int stripH = stripHeightFor(mAppliedW);
    if (stripH <= 0)
        return;
    mPlaceStrip(0, mEditorH, mAppliedW, stripH,
                static_cast<double>(mAppliedW) / static_cast<double>(mCanvasW));
}

//------------------------------------------------------------------------
// ASK THE VIEW UNTIL ITS ANSWER STOPS MOVING, because one question is not enough and the second
// answer is the one the window has to match.
//
// An aspect-locked page fits itself to whichever of the two axes is tighter, and the height it
// returns is ROUNDED. Feed that rounded height back in and the height axis is now fractionally the
// tighter one, so the width comes back a pixel smaller — which is exactly what happens, because the
// view re-runs the same constraint inside onSize() on whatever it is handed. Measured: a window
// dragged to 850 wide was granted 850x302 and drew itself 849 wide, leaving a one-pixel line of
// window background down the right of the editor with a full-width strip beneath it.
//
// Iterating to a fixed point makes the frame ask the same question the view will ask itself, so the
// two cannot disagree. It settles in two rounds for every page here; the cap is there because this
// runs on the window manager's events and a view is not obliged to converge.
bool EditorFrame::constrainToFixedPoint(ViewRect &rect) const
{
    if (!mView)
        return false;
    for (int round = 0; round < 4; ++round) {
        const int w = rect.getWidth();
        const int h = rect.getHeight();
        if (mView->checkSizeConstraint(&rect) != kResultTrue)
            return false;
        if (rect.getWidth() == w && rect.getHeight() == h)
            return true;
    }
    trace("constraint did not settle at %dx%d", rect.getWidth(), rect.getHeight());
    return true;
}

//------------------------------------------------------------------------
void EditorFrame::setEmbedding(NativeWindow &window, IPlugView *view)
{
    mWindow = &window;
    mView = view;

    ViewRect current = {};
    if (mView && mView->getSize(&current) == kResultTrue) {
        mAppliedW = current.getWidth();
        mEditorH = current.getHeight();
        mAppliedH = mEditorH + stripHeightFor(mAppliedW);
    }
    // The width the window will keep. Taken from the view's OWN opening size rather than from a
    // constant here, so this file still knows nothing about pages: whatever the editor comes up
    // at is what the window is, and every later page change is height-only.
    mLockedW = mAppliedW;
    updateSizeHints();
    placeStrip();
}

//------------------------------------------------------------------------
void EditorFrame::applySize(int w, int h)
{
    if (!mWindow || w <= 0 || h <= 0)
        return;
    // Recorded BEFORE the request, because the resize event it provokes may be dispatched before we
    // return here and must already be recognisable as ours.
    mAppliedW = w;
    mAppliedH = h;
    mWindow->resize(w, h);
}

//------------------------------------------------------------------------
void EditorFrame::updateSizeHints()
{
    if (!mWindow || !mView)
        return;

    // Ask the view rather than deciding here: this file knows nothing about pages, and does not
    // need to. checkSizeConstraint clamps whatever it is given into what the CURRENT page can be
    // drawn at, so a 1x1 rect comes back as that page's floor and an absurdly large one as its
    // ceiling.
    int minW = 1, minH = 1, maxW = 0, maxH = 0;
    ViewRect small(0, 0, 1, 1);
    if (mView->checkSizeConstraint(&small) == kResultTrue) {
        minW = small.getWidth();
        minH = small.getHeight();
    }
    ViewRect large(0, 0, 1 << 15, 1 << 15);
    if (mView->checkSizeConstraint(&large) == kResultTrue) {
        maxW = large.getWidth();
        maxH = large.getHeight();
    }

    // The strip rides along with the editor, so the WINDOW's limits are the editor's limits plus
    // the strip at each of them. Leaving the editor-only numbers here would let the window manager
    // offer a height with the bottom of the strip clipped off.
    int hintMinW = minW;
    int hintMinH = minH + stripHeightFor(minW);
    // Zero means "no limit" to the window, which is what an unusable answer from the view has to
    // become: a maximum below the minimum would pin the window to nonsense.
    int hintMaxW = 0;
    int hintMaxH = 0;
    if (maxW >= minW && maxH >= minH) {
        hintMaxW = maxW;
        hintMaxH = maxH + stripHeightFor(maxW);
    }
    // A view that cannot be resized is pinned at the size it has, which is what a host would do
    // with canResize() == kResultFalse.
    if (mView->canResize() != kResultTrue && mAppliedW > 0 && mAppliedH > 0) {
        hintMinW = hintMaxW = mAppliedW;
        hintMinH = hintMaxH = mAppliedH;
    }
    mWindow->setSizeHints(hintMinW, hintMinH, hintMaxW, hintMaxH);
    trace("hints: %d..%d wide, %d..%d tall", hintMinW, hintMaxW, hintMinH, hintMaxH);
}

//------------------------------------------------------------------------
// The plug-in asking for a different window, which here means a page change: the two pages are two
// canvases and the editor calls this on both of them.
//
// HEIGHT IS GRANTED, WIDTH IS PINNED. See the policy at the top of editorframe.h — the top-level
// window is shared with a strip below the editor, so its width is set once and a page change moves
// only the bottom edge. The view is then told the size it really got, not the size it asked for,
// which is what a host does with any request it cannot grant exactly.
//
// In practice the two agree: both pages are the same number of logical units wide, so at a given
// scale the editor asks for the width it already has and the pin changes nothing. That is the
// point. The pin is what makes it stay true — of a page added later, of a scale that rounds a
// pixel differently, and of the editor's own letterbox fallback, none of which this file wants to
// have to reason about.
//
// The SDK's sequence (pluginterfaces/gui/iplugview.h): the plug-in calls resizeView, the host
// resizes the window, and the host calls back into onSize IN THE SAME CALLSTACK. Doing it in that
// order matters — the editor's onSize re-enters its own constrainSize before this returns.
tresult PLUGIN_API EditorFrame::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize)
        return kInvalidArgument;
    // Before the editor has attached there is no window to resize, and saying kResultTrue would
    // leave the view believing in a size nothing is drawn at.
    if (view != mView || !mWindow)
        return kResultFalse;

    const int wanted = newSize->getWidth();
    const int h = newSize->getHeight();
    if (wanted <= 0 || h <= 0)
        return kResultFalse;

    const int w = mLockedW > 0 ? mLockedW : wanted;
    if (w != wanted)
        trace("resizeView: the editor asked for %dx%d; width is pinned to %d", wanted, h, w);
    else
        trace("resizeView: the editor asked for %dx%d (we were at %dx%d)", w, h, mAppliedW,
              mAppliedH);

    // HINTS FIRST, THEN THE RESIZE, and the order is not cosmetic. The window still carries the
    // OUTGOING page's minimum, and a page change can ask for a window outside it — in height now
    // rather than in width, since the width no longer moves. A window manager that honours
    // PMinSize, which most do, clamps the request back and the editor is handed a size nobody
    // asked for. Measured that way round first in the parent project, before the width was
    // shared: the editor asked for 640x524 and got 748x524. Publishing the incoming page's
    // minimum before the request is what makes the request grantable.
    updateSizeHints();
    // `h` is the EDITOR's height; the window is that plus the strip. The editor asked for a page,
    // not for a window, and the strip below it is not the editor's business.
    mEditorH = h;
    applySize(w, h + stripHeightFor(w));

    // The view is told what it GOT. Writing the granted width back into the caller's rect is not
    // optional: the editor reads this rect after the call returns, and leaving the width it asked
    // for in there would have it lay out for a window that does not exist.
    newSize->right = newSize->left + w;
    mView->onSize(newSize);
    placeStrip();
    return kResultTrue;
}

//------------------------------------------------------------------------
// The window manager telling us the window is now some size - the user dragging its frame, or a
// step on the way to a size we asked for ourselves.
//
// WHATEVER IT SAYS IS ACCEPTED, and the view is told. It is NOT pushed back on, and the first
// version of this file was wrong to: a resize we requested arrives as more than one configure on
// a reparenting window manager, so re-resizing to the constrained size turned an intermediate
// step into a new request and the two sides then argued. Measured - a page change back to the
// head page went out as 1133x403, an intermediate 748x460 came back, this function "corrected" it
// to 748x266, and the window stuck there while every later page change was overridden.
//
// A host does not negotiate this way either. It resizes its window and tells the view; the view's
// own onSize constrains what it DRAWS, centring the page when the window is not the shape the page
// wants. That is the same degradation the editor falls back to under a host that offers no
// IPlugFrame at all, where it cannot ask for a size and has to live in whatever it is given — so
// it is a path that is exercised rather than a theoretical one. What keeps a window manager from
// offering a size the page cannot use is the size hints above, which is the mechanism X11 has for
// exactly that.
//
// THIS IS ALSO WHERE THE WIDTH LOCK IS RE-TAKEN. Pinning the width across a page change does not
// mean the window cannot be resized: the user dragging the frame is the one thing that legitimately
// changes it, and whatever they drag it to becomes the new locked width. The distinction is between
// the user asking for a width and the EDITOR asking for one — the first is granted, the second is
// what would move the strip below.
void EditorFrame::windowConfigured(int w, int h)
{
    if (!mView || w <= 0 || h <= 0)
        return;
    if (w == mAppliedW && h == mAppliedH) {
        trace("configure: %dx%d, which is the size we are already at - ignored", w, h);
        return;
    }

    // The strip comes off the bottom before the editor is told anything: what the window manager
    // gave us is the WHOLE window, and the editor's share of it is the rest. A window too short to
    // hold both leaves the editor at one pixel rather than at a negative height, and the strip is
    // then simply not placed — the size hints above are what stops that being reachable, and this
    // is what stops it being a crash if a window manager ignores them.
    const int stripH = stripHeightFor(w);
    const int editorH = h - stripH > 0 ? h - stripH : 1;

    // WHAT THE EDITOR WILL ACTUALLY DRAW IN THAT SPACE, which is not always what it was offered.
    // The pages differ, and the difference is the reason this is asked rather than assumed: the
    // head page is aspect-locked and shrinks to fit whatever rectangle it is given, while the page
    // that scrolls keeps the width and clamps the height to what it can use. Either way the
    // leftover is unpainted window between the editor and the strip — a black band separating two
    // things that have to look joined — so the window is corrected to the shape the editor fills.
    //
    // The two questions are asked in that order because only the first one's ANSWER says which page
    // this is. A width that comes back unchanged means the height was acceptable and that is the
    // size. A width that comes back smaller means the page is aspect-locked and the window is too
    // short for the width the user chose — so it is asked again with an unbounded height, which
    // gives the height that goes WITH that width. Deriving the height from the width, never the
    // other way round, is what keeps a short wide drag from narrowing the window and taking the
    // strip with it.
    ViewRect fit(0, 0, w, editorH);
    if (!constrainToFixedPoint(fit))
        fit = ViewRect(0, 0, w, editorH);
    else if (fit.getWidth() != w) {
        ViewRect tall(0, 0, w, 1 << 15);
        if (constrainToFixedPoint(tall))
            fit = tall;
    }
    const int fitW = fit.getWidth() > 0 ? fit.getWidth() : w;
    const int fitH = fit.getHeight() > 0 ? fit.getHeight() : editorH;
    const int wantH = fitH + stripHeightFor(fitW);

    mAppliedW = w;
    mAppliedH = h;
    mEditorH = fitH;
    mLockedW = fitW;
    trace("configure: the window is now %dx%d - editor %dx%d, strip %d tall", w, h, fitW, fitH,
          stripHeightFor(fitW));

    // THE CORRECTION IS ASKED FOR AT MOST ONCE PER TARGET, and that latch is the whole reason this
    // is safe to do at all. A resize we requested arrives as more than one configure on a
    // reparenting window manager, and an earlier version of this file re-requested on every
    // mismatch: an intermediate step became a new request and the two sides argued forever.
    // Measured that way — a page change back to the head page went out as 1133x403, an intermediate
    // 748x460 came back, this function "corrected" it to 748x266, and the window stuck there.
    // Asking once for a given target and then accepting whatever comes back is what makes the
    // handshake terminate whatever the window manager does.
    if (fitW == w && wantH == h) {
        mRequestedW = -1;
        mRequestedH = -1;
    } else if (fitW != mRequestedW || wantH != mRequestedH) {
        mRequestedW = fitW;
        mRequestedH = wantH;
        trace("configure: correcting %dx%d to %dx%d, which is the shape the editor fills", w, h,
              fitW, wantH);
        applySize(fitW, wantH);
    }

    ViewRect actual(0, 0, fitW, fitH);
    mView->onSize(&actual);
    placeStrip();
}

} // namespace Rations
