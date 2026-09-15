// PanelWindow implementation. See panelwindow.h for the painting discipline.

#include "panelwindow.h"

#include "host/pluginbackend.h"

#include <cstdio>

namespace Rations
{

namespace
{

// Below this the list is unreadable rather than merely cramped, and above it the rows just stretch.
constexpr int kMinWidth = 360;
constexpr int kMinHeight = 140;
constexpr int kMaxWidth = 1400;
constexpr int kMaxHeight = 1400;

} // namespace

//------------------------------------------------------------------------
PanelWindow::PanelWindow(EventLoop &loop, NAMp::host::PluginBackend &backend)
    : mBackend(backend), mWindow(loop)
{
    mTitle = std::string(backend.displayName()) + " - parameters";
    mPanel.setBackend(&backend);
}

//------------------------------------------------------------------------
PanelWindow::~PanelWindow()
{
    close();
}

//------------------------------------------------------------------------
void PanelWindow::loadFonts(const std::string &resourceDir)
{
    // As in RackWindow::loadFonts: an empty directory means "use the built-in faces", not "fail".
    if (!mFonts.load(resourceDir))
        fprintf(stderr, "namp-standalone: the parameter panel fell back to generic fonts\n");
}

//------------------------------------------------------------------------
bool PanelWindow::open()
{
    if (mWindow.isOpen()) {
        mWindow.raise();
        return true;
    }

    mWidth = static_cast<int>(NAMp::rack::panelgeo::kPanelW);
    mHeight = static_cast<int>(NAMp::rack::panelgeo::kDefaultPanelH);

    // Input::Full because we are the ones drawing it — unlike PluginWindow, whose interior belongs
    // to somebody else's editor.
    if (!mWindow.createTopLevel(mTitle.c_str(), mWidth, mHeight, NativeWindow::Input::Full))
        return false;

    // A range rather than a fixed size: the list is the point, and a taller window shows more of
    // it.
    mWindow.setSizeHints(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    mWindow.setEventCallback([this](const WindowEvent &event) { onEvent(event); });

    if (!mWindow.createSurfaces(mWidth, mHeight)) {
        fprintf(stderr, "namp-standalone: cannot create the parameter panel's surfaces\n");
        close();
        return false;
    }

    mPanel.setSize(static_cast<float>(mWidth), static_cast<float>(mHeight));
    mWindow.show();
    mDirty = true;
    return true;
}

//------------------------------------------------------------------------
void PanelWindow::close()
{
    mWindow.destroy();
}

//------------------------------------------------------------------------
void PanelWindow::idle()
{
    if (!mWindow.isOpen())
        return;
    if (mPanel.poll())
        mDirty = true;
    if (mDirty)
        redraw();
}

//------------------------------------------------------------------------
void PanelWindow::redraw()
{
    cairo_surface_t *surface = mWindow.drawingSurface();
    if (!surface)
        return;
    mDirty = false;

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        Canvas canvas(cr, &mFonts, static_cast<float>(mWidth), static_cast<float>(mHeight));
        mPanel.draw(canvas);
    }
    cairo_destroy(cr);

    mWindow.present();
}

//------------------------------------------------------------------------
void PanelWindow::onEvent(const WindowEvent &event)
{
    // Nothing here paints; every branch sets the dirty flag and lets the timer do it.
    switch (event.kind) {
        case WindowEvent::Kind::Close:
            close();
            return;

        case WindowEvent::Kind::Redraw:
            mDirty = true;
            return;

        case WindowEvent::Kind::Resize:
            if (event.width != mWidth || event.height != mHeight) {
                mWidth = event.width;
                mHeight = event.height;
                mWindow.resizeSurfaces(mWidth, mHeight);
                mPanel.setSize(static_cast<float>(mWidth), static_cast<float>(mHeight));
            }
            mDirty = true;
            return;

        case WindowEvent::Kind::MouseLeave:
            // Off-canvas rather than a kind of its own: the panel's hit-testing already treats a
            // position outside itself as "nothing is hovered", so the two answers are the same one.
            if (mPanel.mouseMove(-1.0f, -1.0f))
                mDirty = true;
            return;

        case WindowEvent::Kind::MouseMove:
            if (mPanel.mouseMove(event.x, event.y))
                mDirty = true;
            return;

        case WindowEvent::Kind::Wheel:
            if (mPanel.wheel(event.x, event.y, event.delta))
                mDirty = true;
            return;

        case WindowEvent::Kind::MouseDown:
            if (event.button == kButtonLeft && mPanel.mouseDown(event.x, event.y, event.button))
                mDirty = true;
            return;

        case WindowEvent::Kind::MouseUp:
            if (event.button == kButtonLeft && mPanel.mouseUp(event.x, event.y, kButtonLeft))
                mDirty = true;
            return;

        case WindowEvent::Kind::Key:
        case WindowEvent::Kind::FocusLost:
            // The panel has no text field, so it claims no keys and never holds the focus.
            return;
    }
}

} // namespace Rations
