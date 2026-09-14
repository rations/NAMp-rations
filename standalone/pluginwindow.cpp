// PluginWindow implementation. See pluginwindow.h for the teardown-order contract.

#include "pluginwindow.h"

#include <cstdio>

using namespace Steinberg;

namespace Rations
{

namespace
{

// Provisional size for the window between creation and the editor reporting its own. The window is
// created unmapped and resized before it is ever shown, so this is never seen.
constexpr unsigned kInitialSize = 64;

} // namespace

//------------------------------------------------------------------------
PluginWindow::PluginWindow(EventLoop &loop, NAMp::host::PluginBackend &backend)
    : mBackend(backend), mFrame(loop), mWindow(loop)
{
    mTitle = backend.displayName();
    mFrame.setResizeCallback(
        [this](IPlugView *view, ViewRect *rect) { return onViewResize(view, rect); });
}

//------------------------------------------------------------------------
PluginWindow::~PluginWindow()
{
    close();
}

//------------------------------------------------------------------------
bool PluginWindow::open()
{
    if (mOpen) {
        if (mWindow.isOpen())
            mWindow.raise();
        else
            mBackend.editorShow();
        return true;
    }

    const NAMp::host::EditorKind kind = mBackend.editorKind();
    if (kind == NAMp::host::EditorKind::NoEditor)
        return false;

    // An LV2 ui:showInterface editor opens its own top-level window. We create nothing, map nothing
    // and set no hints — anything we made here would be a second, empty frame beside the real one.
    if (kind == NAMp::host::EditorKind::Lv2ShowInterface) {
        mOwnsWindow = false;

        NAMp::host::EditorOpenRequest request;
        request.parentWindow = 0;
        request.plugFrame = nullptr;
        request.windowTitle = mTitle.c_str();

        NAMp::host::EditorSurface surface;
        if (!mBackend.editorOpen(request, surface)) {
            fprintf(stderr, "namp-standalone: %s could not open its own editor window\n",
                    mTitle.c_str());
            return false;
        }
        mBackend.editorShow();
        mOpen = true;
        return true;
    }

    // Input::StructureOnly: the plug-in draws every pixel of its own child window and handles its
    // own input, exactly as this project's own editor does in the main window. What we need to know
    // about is the window manager resizing or closing us.
    if (!mWindow.createTopLevel(mTitle.c_str(), static_cast<int>(kInitialSize),
                                static_cast<int>(kInitialSize), NativeWindow::Input::StructureOnly))
        return false;

    // Registered before the editor is opened: a view may call resizeView from inside attached(),
    // and that path needs the window already able to report.
    mWindow.setEventCallback([this](const WindowEvent &event) { onEvent(event); });

    // A ROUND TRIP, NOT A FLUSH, and it is load-bearing on the platform that has the distinction. A
    // plug-in embeds into this window from its OWN connection to the display, so the window has to
    // exist on the SERVER before attached() is called, not merely be queued on ours. Nothing above
    // guarantees that: XInternAtom round-trips the first time and is answered from Xlib's cache
    // every time after, so the second open of a window would sail past with the create still
    // sitting in our output buffer, and the plug-in's own XCreateWindow would fail with BadWindow
    // against a parent that does not exist yet. Once per window open. Windows has no such queue and
    // this is inert there.
    mWindow.sync();

    NAMp::host::EditorOpenRequest request;
    request.parentWindow = nativeHandleToInt(mWindow.handle());
    request.plugFrame = static_cast<IPlugFrame *>(&mFrame);
    request.windowTitle = mTitle.c_str();

    NAMp::host::EditorSurface surface;
    if (!mBackend.editorOpen(request, surface)) {
        fprintf(stderr, "namp-standalone: %s has no embeddable editor\n", mTitle.c_str());
        mWindow.destroy();
        return false;
    }

    // An LV2 UI created a child window inside ours and told us its id. It has a natural size and no
    // opinion about ours, so ours takes its size and it is kept filling ours from then on. A VST3
    // view reports no child at all: it drives its own through onSize().
    mChild = nativeHandleFromInt(surface.childWindow);
    if (mChild && surface.width <= 0 && surface.height <= 0) {
        // The UI never called ui:resize, so ask how big the widget it made actually is. Without
        // this the window keeps the provisional size and the editor is drawn into a 64x64 corner.
        int w = 0, h = 0;
        if (mWindow.childGeometry(mChild, w, h)) {
            surface.width = static_cast<int32_t>(w);
            surface.height = static_cast<int32_t>(h);
        }
    }

    mWidth = surface.width > 0 ? surface.width : static_cast<int32_t>(kInitialSize);
    mHeight = surface.height > 0 ? surface.height : static_cast<int32_t>(kInitialSize);
    mResizable = surface.resizable;

    applySizeHints(mWidth, mHeight);
    mWindow.resize(mWidth, mHeight);
    // The editor was attached to a window of the provisional size, so it has to be told the real
    // one. A view that ignores this is no worse off than before; one that honours it lays out
    // correctly the first time rather than on the first user resize.
    mBackend.editorSetSize(mWidth, mHeight);

    if (mChild)
        mWindow.resizeChild(mChild, mWidth, mHeight);

    mWindow.show();
    mOpen = true;
    return true;
}

//------------------------------------------------------------------------
void PluginWindow::close()
{
    if (!mOpen)
        return;
    mOpen = false;

    // Nothing of ours to take down: the editor's window is the editor's.
    if (!mOwnsWindow) {
        mBackend.editorClose();
        mOwnsWindow = true;
        return;
    }

    // Step 1: the editor first. IPlugView::removed() unregisters the view's own event handler and
    // timers from the run loop, so the loop and this frame must both still be alive here.
    mBackend.editorClose();

    // Steps 2 and 3: stop dispatching to a window that is about to stop existing, and only then
    // destroy it. Both are inside destroy(), in that order.
    mWindow.destroy();
    // Synced for the same cross-connection reason as in open(): the destroy has to have reached the
    // server before an id can be reused by the next open.
    mWindow.sync();
    mChild = {};
    mWidth = 0;
    mHeight = 0;
    mResizable = false;
}

//------------------------------------------------------------------------
void PluginWindow::idle()
{
    if (!mOpen)
        return;

    mBackend.editorIdle();

    // A showInterface editor may have closed itself; the backend notices that inside editorIdle()
    // and there is no window of ours to reconcile, so there is nothing further to do here.
    if (!mWindow.isOpen())
        return;

    int32_t w = 0;
    int32_t h = 0;
    if (!mBackend.editorTakeResizeRequest(w, h))
        return;
    if (w <= 0 || h <= 0)
        return;

    // A latched request, applied here rather than where it was raised: a plug-in UI may call its
    // resize callback from a thread we do not control, and no windowing call may be made from
    // inside a plug-in callback. This is the timer, so it is ours.
    mWidth = w;
    mHeight = h;
    applySizeHints(mWidth, mHeight);
    mWindow.resize(mWidth, mHeight);
    if (mChild)
        mWindow.resizeChild(mChild, mWidth, mHeight);
    mBackend.editorSetSize(mWidth, mHeight);
}

//------------------------------------------------------------------------
void PluginWindow::constrain(int32_t &w, int32_t &h) const
{
    int32_t askedW = w;
    int32_t askedH = h;
    if (!mBackend.editorCheckSize(askedW, askedH))
        return;
    if (askedW > 0 && askedH > 0) {
        w = askedW;
        h = askedH;
    }
}

//------------------------------------------------------------------------
// A non-resizable editor is pinned by making the minimum and the maximum the same size, which is
// what stops a window manager offering a drag the editor will only refuse. A resizable one is asked
// for its own limits the way the main window asks NAMp's editor: constrain an impossibly small and
// an impossibly large rectangle and see what comes back.
void PluginWindow::applySizeHints(int32_t w, int32_t h)
{
    if (!mWindow.isOpen())
        return;

    if (mResizable) {
        int32_t minW = 1, minH = 1, maxW = 20000, maxH = 20000;
        constrain(minW, minH);
        constrain(maxW, maxH);
        mWindow.setSizeHints(minW, minH, maxW, maxH);
    } else {
        mWindow.setSizeHints(w, h, w, h);
    }
}

//------------------------------------------------------------------------
bool PluginWindow::onViewResize(IPlugView *view, ViewRect *rect)
{
    if (!mWindow.isOpen() || !view || !rect)
        return false;

    mWidth = rect->getWidth();
    mHeight = rect->getHeight();
    applySizeHints(mWidth, mHeight);
    mWindow.resize(mWidth, mHeight);
    // The SDK requires onSize() in the same callstack as the window resize.
    return view->onSize(rect) == kResultTrue;
}

//------------------------------------------------------------------------
void PluginWindow::onEvent(const WindowEvent &event)
{
    if (event.kind == WindowEvent::Kind::Close) {
        // Closing an editor window closes that editor and nothing else. This returns while the
        // dispatch that called it is still on the stack, which is safe precisely because close()
        // does not destroy this object.
        close();
        return;
    }

    // This window asked for structure only, so a resize is the one other thing it can be told.
    if (event.kind != WindowEvent::Kind::Resize)
        return;

    const int32_t reportedW = event.width;
    const int32_t reportedH = event.height;
    if (reportedW <= 0 || reportedH <= 0)
        return;

    // The user dragged the frame: ask the editor what it will accept, correct the window if that
    // differs, and only then tell the editor. Correcting the window re-enters here once with a size
    // the editor already accepts, which is where the handshake stops.
    int32_t w = reportedW;
    int32_t h = reportedH;
    constrain(w, h);
    if (w != reportedW || h != reportedH)
        mWindow.resize(w, h);

    if (w == mWidth && h == mHeight)
        return;
    mWidth = w;
    mHeight = h;
    // A child window does not track its parent's size, so a resized window would leave an LV2 UI
    // drawn at its old size in the corner. VST3 has no child here and handles it through
    // editorSetSize.
    if (mChild)
        mWindow.resizeChild(mChild, w, h);
    mBackend.editorSetSize(w, h);
}

} // namespace Rations
