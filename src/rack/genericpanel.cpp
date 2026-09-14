// GenericPanel implementation. See genericpanel.h for why this exists and what it deliberately is
// not.

#include "genericpanel.h"

#include "host/pluginbackend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace NAMp::rack
{

using namespace panelgeo;

using Rations::Font;

namespace
{

// The same palette the rack strip uses, so a panel opened from it does not look like another
// program.
constexpr uint32_t kBackground = 0x14161a;
constexpr uint32_t kHeaderText = 0xe6e9ee;
constexpr uint32_t kRowText = 0xb9c0cb;
constexpr uint32_t kRowTextHot = 0xe6e9ee;
constexpr uint32_t kRowFill = 0x1c2027;
constexpr uint32_t kRowFillHot = 0x232833;
constexpr uint32_t kTrack = 0x2b313c;
constexpr uint32_t kFill = 0x4fa36b;
constexpr uint32_t kThumb = 0xdfe4ec;
constexpr uint32_t kScrollThumb = 0x3a414f;

} // namespace

//------------------------------------------------------------------------
void GenericPanel::setBackend(host::PluginBackend *backend)
{
    mBackend = backend;
    mScroll = 0;
    mHover = -1;
    mDrag = -1;
    rebuildRows();
}

//------------------------------------------------------------------------
// Which parameters get a row, and which do not.
//
// A MIDI CONTROLLER DESTINATION IS NOT A SETTING. A plug-in that accepts MIDI declares its
// destinations as parameters, because that is the only vocabulary the format has for saying so —
// but each one is an INPUT PORT shaped like a knob, written by incoming MIDI and by nothing else.
// A slider on one sets a value the next message overwrites, which is a control that does not
// control anything.
//
// It matters here more than anywhere: a plug-in that maps the whole controller range declares 128
// of them, and one measured on this machine has 134 parameters of which 128 are destinations. A
// list of everything is then a list of five real controls followed by six screens of rows nobody
// can use, and the scrollbar says the panel is mostly empty.
//
// The same fact already decides what goes into a saved state, and it is one flag rather than two
// spellings of the same test.
void GenericPanel::rebuildRows()
{
    mRows.clear();
    mHidden = 0;
    mCount = mBackend ? mBackend->paramCount() : 0;
    for (uint32_t i = 0; i < mCount; ++i) {
        host::ParamInfo info;
        if (!mBackend->paramInfo(i, info))
            continue;
        if (info.isMidiMapped) {
            ++mHidden;
            continue;
        }
        mRows.push_back(i);
    }
    clampScroll();
}

//------------------------------------------------------------------------
void GenericPanel::setSize(float w, float h)
{
    mWidth = w > 0.0f ? w : kPanelW;
    mHeight = h > 0.0f ? h : kDefaultPanelH;
    clampScroll();
}

//------------------------------------------------------------------------
int32_t GenericPanel::visibleRows() const
{
    const float room = mHeight - kHeaderH - 2.0f * kMargin;
    const int32_t rows = static_cast<int32_t>(room / (kRowH + kRowGap));
    return rows > 0 ? rows : 1;
}

//------------------------------------------------------------------------
void GenericPanel::clampScroll()
{
    const int32_t maxScroll = rowCount() - visibleRows();
    mScroll = std::clamp(mScroll, 0, maxScroll > 0 ? maxScroll : 0);
}

//------------------------------------------------------------------------
// Which ROW a y coordinate falls on — not which parameter, which is what paramFor() turns it into.
// Returns -1 above the first row, below the last, or in a gap: a gap is not the nearest row, it is
// nothing, and treating it as a row makes a slider jump when a click lands a pixel high.
int32_t GenericPanel::rowAt(float y) const
{
    const float top = kMargin + kHeaderH;
    if (y < top)
        return -1;
    const float pitch = kRowH + kRowGap;
    const int32_t visual = static_cast<int32_t>((y - top) / pitch);
    if (visual < 0 || visual >= visibleRows())
        return -1;
    if ((y - top) - static_cast<float>(visual) * pitch > kRowH)
        return -1; // in the gap between two rows

    const int32_t row = mScroll + visual;
    return row < rowCount() ? row : -1;
}

//------------------------------------------------------------------------
int32_t GenericPanel::paramFor(int32_t row) const
{
    if (row < 0 || row >= rowCount())
        return -1;
    return static_cast<int32_t>(mRows[static_cast<size_t>(row)]);
}

//------------------------------------------------------------------------
Rect GenericPanel::sliderRectFor(int32_t visualRow) const
{
    const float top = kMargin + kHeaderH + static_cast<float>(visualRow) * (kRowH + kRowGap);
    const float left = kMargin + kLabelW;
    const float right = mWidth - kMargin - kValueW - kScrollW - 6.0f;
    return Rect::fromLTRB(left, top, right > left ? right : left + 1.0f, top + kRowH);
}

//------------------------------------------------------------------------
void GenericPanel::draw(Canvas &canvas)
{
    canvas.setColor(kBackground);
    canvas.fillRect(Rect(0.0f, 0.0f, mWidth, mHeight));

    if (!mBackend) {
        canvas.setFont(Font::Body);
        canvas.setFontSize(13.0f);
        canvas.setColor(kRowText);
        canvas.drawString("no plug-in", kMargin, kMargin + 18.0f);
        return;
    }

    // The count can move under us: a plug-in is allowed to change its parameter list, and a panel
    // that cached it once would index off the end.
    if (mCount != mBackend->paramCount())
        rebuildRows();
    clampScroll();

    canvas.setFont(Font::Title);
    canvas.setFontSize(15.0f);
    canvas.setColor(kHeaderText);
    canvas.drawString(canvas.clipToWidth(mBackend->displayName(), mWidth - 2.0f * kMargin).c_str(),
                      kMargin, kMargin + 16.0f);

    canvas.setFont(Font::Body);
    canvas.setFontSize(11.0f);
    canvas.setColor(kRowText, 170);
    char subtitle[96] = {};
    const int32_t shownRows = rowCount();
    if (mHidden > 0)
        std::snprintf(subtitle, sizeof(subtitle), "%d parameter%s  ·  %u MIDI destination%s",
                      shownRows, shownRows == 1 ? "" : "s", mHidden, mHidden == 1 ? "" : "s");
    else
        std::snprintf(subtitle, sizeof(subtitle), "%d parameter%s", shownRows,
                      shownRows == 1 ? "" : "s");
    canvas.drawString(subtitle, kMargin, kMargin + 32.0f);

    const int32_t rows = visibleRows();
    canvas.setFontSize(12.0f);

    for (int32_t visual = 0; visual < rows; ++visual) {
        const int32_t rowIndex = mScroll + visual;
        const int32_t index = paramFor(rowIndex);
        if (index < 0)
            break;

        host::ParamInfo info;
        if (!mBackend->paramInfo(static_cast<uint32_t>(index), info))
            continue;

        const bool hot = (rowIndex == mHover) || (rowIndex == mDrag);
        const float top = kMargin + kHeaderH + static_cast<float>(visual) * (kRowH + kRowGap);
        const Rect row(kMargin, top, mWidth - 2.0f * kMargin - kScrollW - 4.0f, kRowH);

        canvas.setColor(hot ? kRowFillHot : kRowFill);
        canvas.fillRoundRect(row, 4.0f);

        canvas.setColor(hot ? kRowTextHot : kRowText);
        canvas.drawString(canvas.clipToWidth(info.name, kLabelW - 12.0f).c_str(), kMargin + 8.0f,
                          top + kRowH * 0.5f + 4.0f);

        const double value = mBackend->paramGet(static_cast<uint32_t>(index));
        const Rect slider = sliderRectFor(visual);
        const float trackY = slider.centerY() - kTrackH * 0.5f;

        canvas.setColor(kTrack);
        canvas.fillRoundRect(Rect(slider.x, trackY, slider.w, kTrackH), kTrackH * 0.5f);

        const float travel = slider.w - kThumbW;
        const float filled = static_cast<float>(value) * travel;
        canvas.setColor(kFill, info.isReadOnly ? 110 : 255);
        canvas.fillRoundRect(Rect(slider.x, trackY, filled + kThumbW * 0.5f, kTrackH),
                             kTrackH * 0.5f);

        // A read-only parameter is drawn without a grip, because there is nothing to grab.
        if (!info.isReadOnly) {
            canvas.setColor(kThumb);
            canvas.fillRoundRect(Rect(slider.x + filled, slider.y + 6.0f, kThumbW, kRowH - 12.0f),
                                 3.0f);
        }

        char shown[128] = {};
        if (!mBackend->paramDisplay(static_cast<uint32_t>(index), value, shown, sizeof(shown)))
            std::snprintf(shown, sizeof(shown), "%.3f", value);
        canvas.setColor(hot ? kRowTextHot : kRowText);
        canvas.drawString(canvas.clipToWidth(shown, kValueW - 8.0f).c_str(),
                          mWidth - kMargin - kScrollW - 4.0f - kValueW, top + kRowH * 0.5f + 4.0f);
    }

    // The scrollbar exists only when there is something to scroll, so a short list is not decorated
    // with a full-height thumb that does nothing.
    if (shownRows > rows) {
        const float top = kMargin + kHeaderH;
        const float height = static_cast<float>(rows) * (kRowH + kRowGap) - kRowGap;
        const float x = mWidth - kMargin - kScrollW;
        canvas.setColor(kTrack);
        canvas.fillRoundRect(Rect(x, top, kScrollW, height), kScrollW * 0.5f);

        const float fraction = static_cast<float>(rows) / static_cast<float>(shownRows);
        const float thumbH = std::max(18.0f, height * fraction);
        const float span = height - thumbH;
        const float offset =
            span * static_cast<float>(mScroll) / static_cast<float>(shownRows - rows);
        canvas.setColor(kScrollThumb);
        canvas.fillRoundRect(Rect(x, top + offset, kScrollW, thumbH), kScrollW * 0.5f);
    }
}

//------------------------------------------------------------------------
void GenericPanel::applyDrag(float x)
{
    if (!mBackend || mDrag < 0)
        return;

    const int32_t param = paramFor(mDrag);
    if (param < 0)
        return;

    host::ParamInfo info;
    if (!mBackend->paramInfo(static_cast<uint32_t>(param), info) || info.isReadOnly)
        return;

    const Rect slider = sliderRectFor(mDrag - mScroll);
    const float travel = slider.w - kThumbW;
    if (travel <= 0.0f)
        return;

    // The pointer positions the CENTRE of the thumb, so the value under the cursor is the value the
    // cursor is on. Positioning its left edge instead is the classic half-a-thumb offset.
    double normalized = (x - slider.x - kThumbW * 0.5f) / travel;
    normalized = std::clamp(normalized, 0.0, 1.0);

    // A stepped parameter has no values between its steps, so the drag snaps rather than sending a
    // continuous stream the backend will quantise anyway.
    if (info.stepCount > 1) {
        const double steps = static_cast<double>(info.stepCount - 1);
        normalized = std::round(normalized * steps) / steps;
    }

    mBackend->paramSetFromUi(static_cast<uint32_t>(param), normalized);
}

//------------------------------------------------------------------------
bool GenericPanel::mouseDown(float x, float y, int button)
{
    if (!mBackend || button != 1)
        return false;

    const int32_t row = rowAt(y);
    const int32_t param = paramFor(row);
    if (param < 0)
        return false;

    host::ParamInfo info;
    if (!mBackend->paramInfo(static_cast<uint32_t>(param), info) || info.isReadOnly)
        return false;

    mDrag = row;
    applyDrag(x);
    return true;
}

//------------------------------------------------------------------------
bool GenericPanel::mouseMove(float x, float y)
{
    if (mDrag >= 0) {
        // A drag keeps the pointer even outside its row. Letting go of it the moment the cursor
        // strays a pixel is what makes a slider feel broken.
        applyDrag(x);
        return true;
    }

    const int32_t index = rowAt(y);
    if (index == mHover)
        return false;
    mHover = index;
    return true;
}

//------------------------------------------------------------------------
bool GenericPanel::mouseUp(float x, float y, int button)
{
    (void)y;
    if (button != 1 || mDrag < 0)
        return false;
    applyDrag(x);
    mDrag = -1;
    return true;
}

//------------------------------------------------------------------------
bool GenericPanel::wheel(float x, float y, int delta)
{
    (void)x;
    (void)y;
    if (!mBackend || delta == 0)
        return false;

    const int32_t before = mScroll;
    mScroll -= delta * 3;
    clampScroll();
    return mScroll != before;
}

//------------------------------------------------------------------------
bool GenericPanel::poll()
{
    if (!mBackend)
        return false;

    // Drain what the plug-in published, bounded so a plug-in with a busy meter cannot own the
    // timer. The values themselves are read straight out of the backend at draw time, so all this
    // has to do is notice that something moved.
    bool changed = false;
    for (int guard = 0; guard < 32; ++guard) {
        uint32_t index = 0;
        double normalized = 0.0;
        if (!mBackend->paramPollFromRt(index, normalized))
            break;
        changed = true;
    }

    if (mCount != mBackend->paramCount()) {
        rebuildRows();
        changed = true;
    }
    return changed;
}

} // namespace NAMp::rack
