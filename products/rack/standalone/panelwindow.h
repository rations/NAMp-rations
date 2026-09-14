// PanelWindow — one top-level X window holding the generic parameter panel for one plug-in.
//
// The sibling of PluginWindow, and deliberately shaped the same: same open()/close()/idle() verbs,
// same "close() is not destruction" rule, same one-window-per-plug-in policy. The difference is who
// draws. PluginWindow hands its window to the plug-in and gets out of the way; this one draws every
// pixel itself through NampRack's GenericPanel, and is therefore what a plug-in with no editor — or
// one whose editor this host cannot show — gets instead of nothing.
//
// PAINTING FOLLOWS THE SAME DISCIPLINE AS EVERY OTHER WINDOW HERE: an event never paints. It sets a
// dirty flag; the timer composes the whole panel offscreen and presents it once, so a run loop that
// is re-entered cannot recurse into drawing.
//
// This window is NOT scaled. The rack strip and the editor share one logical canvas because they
// are one picture; a parameter panel is its own window at its own size, so its logical units are
// its pixels and there is no scale factor to divide mouse coordinates by.
//
// NOTHING HERE KNOWS WHICH WINDOWING SYSTEM IT IS ON. The window, its surfaces and the translation
// of whatever the system reported into a WindowEvent all live behind nativewindow.h; what is left
// in this file is the part that is the same everywhere — which of the panel's controls an event
// lands on.

#pragma once

#include "eventloop.h"
#include "nativewindow.h"

#include "gfx/fontstack.h"
#include "rack/genericpanel.h"

#include <cstdint>
#include <string>

namespace NAMp::host
{
class PluginBackend;
}

namespace Rations
{

//------------------------------------------------------------------------
class PanelWindow
{
public:
    PanelWindow(EventLoop &loop, NAMp::host::PluginBackend &backend);
    ~PanelWindow();

    PanelWindow(const PanelWindow &) = delete;
    PanelWindow &operator=(const PanelWindow &) = delete;

    // Same resource directory the rack and the editor use, so the panel is set in the same faces.
    void loadFonts(const std::string &resourceDir);

    bool open();
    void close();
    bool isOpen() const
    {
        return mWindow.isOpen();
    }

    // Run-loop tick: polls the plug-in for values it changed itself and repaints if anything asked.
    void idle();

    NAMp::host::PluginBackend &backend() const
    {
        return mBackend;
    }

private:
    void onEvent(const WindowEvent &event);
    void redraw();

    NAMp::host::PluginBackend &mBackend;
    FontStack mFonts;
    NAMp::rack::GenericPanel mPanel;

    NativeWindow mWindow;
    int mWidth = 0, mHeight = 0;
    bool mDirty = true;
    std::string mTitle;
};

} // namespace Rations
