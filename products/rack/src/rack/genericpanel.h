// GenericPanel — a plug-in's parameters drawn from nothing but the backend interface.
//
// Some plug-ins have no editor at all, and some have one this host cannot show. Rather than leaving
// those unusable, the panel builds a scrollable list of labelled controls out of paramCount(),
// paramInfo(), paramGet() and paramDisplay() — which every backend implements, for every format, by
// definition. It is therefore always available and needs to know nothing about VST3 or LV2.
//
// NO ART, NO X11, NO JACK. Drawn entirely from Canvas primitives, so it renders offline in a test
// exactly as it does on screen and cannot fail to load a resource. The list, scrollbar and wheel
// behaviour follow the file browser's idiom, because a user should only have to learn one.
//
// A ROW IS A HORIZONTAL SLIDER, not a knob. A knob needs a drag gesture with an accumulator and a
// sensitivity constant to be usable; a slider is a click-to-position plus a drag, is readable at a
// glance next to its value, and is what a list of forty parameters wants. Stepped parameters snap,
// because a backend that reports a step count is telling us the intermediate values do not exist.
//
// PARAMETER EDITS GO STRAIGHT TO paramSetFromUi(), which every backend defines as an ENQUEUE. That
// is what makes it safe to drag a slider while audio is running: nothing here ever touches the
// plug-in's processing state, and nothing here runs on the audio thread.

#pragma once

#include "gfx/canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NAMp::host
{
class PluginBackend;
}

namespace NAMp::rack
{

// See rackview.h for why the amp's drawing primitives are imported into this namespace rather than
// qualified at every use.
using Rations::Canvas;
using Rations::Rect;

//------------------------------------------------------------------------
// Logical units, like every other geometry constant in this project. Nothing here is a pixel.
namespace panelgeo
{
constexpr float kMargin = 14.0f;
constexpr float kHeaderH = 40.0f;
constexpr float kRowH = 30.0f;
constexpr float kRowGap = 3.0f;
// The label column, then the slider, then the value readout.
constexpr float kLabelW = 168.0f;
constexpr float kValueW = 96.0f;
constexpr float kScrollW = 9.0f;
constexpr float kTrackH = 6.0f;
constexpr float kThumbW = 9.0f;

constexpr float kPanelW = 560.0f;
// Ten rows plus the header and both margins; the window is resizable, and this is only where it
// starts.
constexpr float kDefaultPanelH = kHeaderH + 10.0f * (kRowH + kRowGap) + 2.0f * kMargin;
} // namespace panelgeo

//------------------------------------------------------------------------
class GenericPanel
{
public:
    // The panel does not own the backend and does not outlive it. The caller guarantees that, the
    // same way it does for a PluginWindow.
    void setBackend(host::PluginBackend *backend);
    host::PluginBackend *backend() const
    {
        return mBackend;
    }

    // Logical size. Changing it re-lays out on the next draw.
    void setSize(float w, float h);

    void draw(Canvas &canvas);

    // Input, all in logical units. Each returns true when something changed that needs a repaint.
    bool mouseDown(float x, float y, int button);
    bool mouseMove(float x, float y);
    bool mouseUp(float x, float y, int button);
    bool wheel(float x, float y, int delta);

    // Called on the UI timer: picks up values the plug-in changed itself, so a panel left open next
    // to a plug-in's own automation does not show a stale number.
    bool poll();

private:
    // Which ROW a point falls on, and which parameter that row is showing. -1 for none. The two
    // are not the same number: a parameter that is a MIDI destination gets no row — see
    // rebuildRows().
    int32_t rowAt(float y) const;
    int32_t paramFor(int32_t row) const;
    void rebuildRows();
    int32_t rowCount() const
    {
        return static_cast<int32_t>(mRows.size());
    }
    Rect sliderRectFor(int32_t visualRow) const;
    void applyDrag(float x);
    void clampScroll();
    int32_t visibleRows() const;

    host::PluginBackend *mBackend = nullptr;
    float mWidth = panelgeo::kPanelW;
    float mHeight = panelgeo::kDefaultPanelH;

    // Every parameter the backend has, and the ones that get a row. mCount is kept only so a
    // parameter list that changes under the panel is noticed.
    uint32_t mCount = 0;
    std::vector<uint32_t> mRows;
    uint32_t mHidden = 0;
    int32_t mScroll = 0;
    int32_t mHover = -1;
    // The ROW being dragged, or -1. A drag continues to own the pointer even when it leaves the
    // row, which is what makes a slider usable without a steady hand.
    int32_t mDrag = -1;
};

} // namespace NAMp::rack
