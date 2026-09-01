// Rations native editor implementation.
//
// Four pages, each on its own window: the amp head (a photographic faceplate, eight rotated-bitmap
// dials, five bat toggles and their LEDs, a bypass toggle, level meters on both edges, Input and
// Output dials under the meters, two page buttons and a gear), the cabinet (the cab, a blend dial
// drawn over the knob painted into the art, and two impulse-response loader rows), a pedalboard
// placeholder, and the MIDI settings page. Every art load is checked with a flat-colour fallback,
// so a missing Resources directory degrades rather than crashes.
//
// Threading: everything here runs on the host's run-loop thread (the embedding contract); the
// controller is called on the same thread, so value reads/writes and ParamChanged() need no
// locking.

#include "rationsview.h"
#include "rationscontroller.h"
#include "rationsids.h"
#include "platform/respath.h"

#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/base/ustring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Steinberg;

namespace Rations
{

// The settings page's MIDI section is laid out in geometry.h and populated from midilearn.h, and
// this is the only file that includes both, so it is where the two spellings of "how many rows"
// are made to agree. A pedal added to kPedalParams grows both; a mismatch would draw eight rows
// over nine bindings and silently lose one.
static_assert(geo::kMidiRowCount == kMidiLearnRowCount,
              "the settings page must draw exactly the rows the learn table has");

namespace
{

// The longest name a channel may be given. Not a buffer limit - the string grows - but a limit on
// what can be typed into a field that is 170 logical units wide and clipped: past this the user is
// entering text nothing will ever show them, and a state blob is not the place to keep it.
constexpr size_t kRenameMaxChars = 64;

// Pixels of vertical drag for a dial's full range, in LOGICAL units — so the feel is the same at
// every window size.
constexpr float kKnobDragRange = 200.0f;

// The dial is the one layer that is rotated after being scaled, so rotation resamples it a second
// time. Caching it at exactly its on-screen size loses visible contrast in the thin gold pointer;
// caching it at 2x does not. The measurement behind this constant is the parent plug-in's, and it
// is the same art at the same sizes here.
constexpr double kDialSupersample = 2.0;

// Meter ballistics at the 33 ms tick: instant attack, ~15 dB/s release, and a peak marker that
// holds for a second before falling.
constexpr float kMeterRelease = 0.944f;
constexpr float kPeakRelease = 0.985f;
constexpr int kPeakHoldTicks = 30;

// On-screen height the row icons are rasterised at.

// Capability polling: every 15 ticks (~500 ms), and never more than this many times.
constexpr int kCapsPollTicks = 15;
constexpr int kCapsMaxPolls = 40;

// MIDI table polling, on the same tick rate. Unbounded, unlike the capability poll: a row stays
// armed until the player presses something or clicks the button again, and there is no count at
// which giving up would be the right answer. It only runs with the settings page open and a row
// waiting, so the steady state is still no traffic at all.
constexpr int kMidiPollTicks = 15;

// Strip a trailing ".nam" so a capture reads as the amp's own marking.
std::string captureLabel(const std::string &name)
{
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && strcasecmp(name.c_str() + dot + 1, "nam") == 0)
        return name.substr(0, dot);
    return name;
}

// Just the part that distinguishes one capture from its neighbours. A bank is named for the amp
// and channel with the knob position last ("MyAmp - crunch - GAIN 4"), and every entry shares
// everything but that trailing token — so the token alone is what the dial is actually selecting,
// and it is short enough to sit under one.
std::string shortCaptureLabel(const std::string &name)
{
    const std::string full = captureLabel(name);
    const size_t sep = full.rfind(" - ");
    return sep == std::string::npos ? full : full.substr(sep + 3);
}

double clampNorm(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

} // namespace

//------------------------------------------------------------------------
RationsEditorView::RationsEditorView(RationsController *controller)
    : NativePlugView(static_cast<Vst::EditController *>(controller)), mController(controller)
{
    ViewRect size(0, 0, geo::kWinW, geo::kWinH);
    setRect(size);
}

RationsEditorView::~RationsEditorView()
{
    releaseBackground();
}

//------------------------------------------------------------------------
// Art and fonts are loaded here rather than in the constructor: createView() must stay harmless in
// headless hosts, which create and destroy views without ever attaching them.
void RationsEditorView::onAttached()
{
    if (mResourcesLoaded)
        return;
    const std::string &res = resourceDir();
    mFonts.load(res);
    mImages.setResourceDir(res);
    mSvgs.setResourceDir(res);
    mResourcesLoaded = true;
    recomputeLayout();
    rebuildBackground();
}

void RationsEditorView::onRemoved()
{
    releaseBackground();
}

//------------------------------------------------------------------------
// Pages
//------------------------------------------------------------------------
// The page swap. mPage is assigned BEFORE the resize is requested because the host calls back into
// onSize() — and so into constrainSize(), which reads mPage — inside requestResize(), not after
// it. Assigning afterwards would size the new page's window from the old page's aspect.
void RationsEditorView::setPage(geo::Page page)
{
    if (page == mPage)
        return;

    // Carry the scale across, so the window changes SHAPE and not apparent size.
    const double scale = mScale;
    mPage = page;
    // A page is entered at its top. Carrying a scroll position across would mean arriving at the
    // settings page part-way down it, which is neither where the user left it (the page they left
    // was a different one) nor where anything they are looking for is.
    mScrollY = 0.0;
    mScrollMax = 0.0;
    mScrollDrag = false;

    // The browser belongs to a page's rows; leaving it open across a page change would draw a
    // picker over a panel with nothing to pick. A half-typed channel name is the same kind of
    // thing, and is KEPT rather than dropped, for the same reason a click away from it keeps it.
    mBrowser.close();
    commitRename();
    // A drag in progress belongs to a control that is no longer on screen. Ending the edit rather
    // than dropping it matters: beginEdit without endEdit leaves the host's automation latched.
    if (mDragParam && mController) {
        mController->endEdit(mDragParam);
        mDragParam = 0;
    }

    const geo::PageSize ps = geo::pageSize(page);
    const int w = static_cast<int>(std::lround(ps.w * scale));
    const int h = static_cast<int>(std::lround(ps.h * scale));
    requestResize(w, h);

    // Unconditionally, whatever the host did. A successful resize has already run onResized() and
    // this repeats it harmlessly; a refusal, an absent IPlugFrame, or a host that skipped onSize()
    // because the pixel size happened not to change all leave the layout describing the previous
    // page, and that is the case this catches.
    recomputeLayout();
    rebuildBackground();
    invalidate();
}

//------------------------------------------------------------------------
// Fit the current page inside the window and centre it. One rule for both cases: after a
// successful resize the remainder is zero and this is a pure scale, and in a host that would not
// resize it letterboxes the page in the window that exists.
//
// A SCROLLING PAGE TAKES ITS SCALE FROM THE WIDTH ALONE, because its height is not a constraint
// any more — whatever does not fit is scrolled to. Fitting it by height as well is what used to
// drive the scale under the floor, and clamping back up to the floor is what put mOffY negative
// and cut the ends off the page.
void RationsEditorView::recomputeLayout()
{
    const geo::PageSize ps = geo::pageSize(mPage);
    if (mDevW <= 0 || mDevH <= 0 || ps.w <= 0 || ps.h <= 0)
        return;

    const bool scrolls = geo::pageScrolls(mPage);
    double s = mDevW / static_cast<double>(ps.w);
    if (!scrolls)
        s = std::min(s, mDevH / static_cast<double>(ps.h));
    s = std::min(std::max(s, geo::pageScaleMin(mPage)), geo::kScaleMax);
    mScale = s;
    mOffX = (mDevW - ps.w * s) * 0.5;

    // How much of the page the window cannot show, in logical units. Zero — and so no scrollbar
    // and no offset anywhere — on every other page, and on this one at any height that fits.
    const double viewH = mDevH / s;
    mScrollMax = scrolls ? std::max(0.0, ps.h - viewH) : 0.0;
    mScrollY = std::min(std::max(mScrollY, 0.0), mScrollMax);

    // A scrolled page is pinned to the top of its window: centring it would move the content by
    // half the remainder and then the scroll would move it again, which is two rules for one
    // number. When it fits, the ordinary letterbox centring applies exactly as before.
    mOffY = mScrollMax > 0.0 ? 0.0 : (mDevH - ps.h * s) * 0.5;

    // The browser card is sized to the page, and on a short window the page is taller than the
    // window. Re-bound it here rather than only at open time, so a window dragged shorter with
    // the picker up does not leave a card hanging out of the bottom of it.
    if (mBrowser.isOpen() && scrolls)
        boundCaptureBrowser();
}

//------------------------------------------------------------------------
// The visible height of the page, in logical units.
double RationsEditorView::viewportH() const
{
    const geo::PageSize ps = geo::pageSize(mPage);
    if (mScale <= 0.0)
        return ps.h;
    return std::min(static_cast<double>(ps.h), mDevH / mScale);
}

bool RationsEditorView::setScroll(double y)
{
    const double next = std::min(std::max(y, 0.0), mScrollMax);
    if (next == mScrollY)
        return false;
    mScrollY = next;
    invalidate();
    return true;
}

//------------------------------------------------------------------------
// Aspect-locked to the CURRENT page. Fitting INSIDE whatever box the host proposes (min of the two
// ratios) is what makes dragging any edge or corner behave — dragging the bottom edge shrinks the
// width to match instead of being ignored.
void RationsEditorView::constrainSize(int &w, int &h) const
{
    const geo::PageSize ps = geo::pageSize(mPage);
    const double byW = w / static_cast<double>(ps.w);

    // A scrolling page is WIDTH-LOCKED AND FREE IN HEIGHT: the scale comes from the width, and
    // the height is then anything between the shortest useful viewport and the whole page. That
    // is the whole of the resize contract that makes the scrollbar reachable — a height taken
    // from the aspect ratio would refuse the very window sizes it exists to serve.
    if (geo::pageScrolls(mPage)) {
        const double s = std::min(std::max(byW, geo::pageScaleMin(mPage)), geo::kScaleMax);
        w = static_cast<int>(std::lround(ps.w * s));
        const int minH = static_cast<int>(std::lround(geo::kSettingsMinViewH * s));
        const int maxH = static_cast<int>(std::lround(ps.h * s));
        h = std::min(std::max(h, minH), maxH);
        return;
    }

    const double byH = h / static_cast<double>(ps.h);
    double s = std::min(byW, byH);
    s = std::min(std::max(s, geo::pageScaleMin(mPage)), geo::kScaleMax);
    w = static_cast<int>(std::lround(ps.w * s));
    h = static_cast<int>(std::lround(ps.h * s));
}

//------------------------------------------------------------------------
void RationsEditorView::onResized(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    mDevW = w;
    mDevH = h;
    recomputeLayout();
    // Every cached surface was built for the old size; keeping them would draw the panel at the
    // previous scale, and keeping them per size would grow the cache by one entry per pixel of a
    // window drag.
    mImages.purgeScaled();
    rebuildBackground();
}

//------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------
// Everything on the current page that never changes. Composited once per resize or page change
// into a device-resolution surface and blitted 1:1 every frame — see the table in gfx/image.h for
// why the panel art is not drawn per frame like the rest.
void RationsEditorView::drawStaticLayer(Canvas &c)
{
    switch (mPage) {
        case geo::Page::Head:
            if (cairo_surface_t *head = mImages.get("head"))
                c.drawImage(head, Rect(0, 0, geo::kWinW, geo::kWinH));
            // The wordmark: text in Michroma, centred on the faceplate by measuring it, the way
            // the author's other plug-ins draw their own names. There is no badge asset.
            c.setFont(Font::Title);
            c.setFontSize(geo::kTitleSize);
            c.setColor(geo::kTextColor);
            c.drawString("Rations", geo::kFaceCX - c.stringWidth("Rations") * 0.5f,
                         geo::kTitleBaselineY);
            break;
        case geo::Page::Cabinet:
            if (cairo_surface_t *cab = mImages.get("cabinet"))
                c.drawImage(cab, Rect(geo::kCabX, geo::kCabY, geo::kCabW, geo::kCabH));
            break;
        case geo::Page::Pedalboard:
            drawPedalboardStatic(c);
            break;
        case geo::Page::Settings:
            break; // nothing static beyond the ground the caller already painted
    }
}

void RationsEditorView::rebuildBackground()
{
    releaseBackground();
    if (!mResourcesLoaded || mDevW <= 0 || mDevH <= 0)
        return;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mDevW, mDevH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return; // onDraw falls back to drawing the static layer inline
    }

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        // The ground is painted in DEVICE space, before the page transform, so it covers the
        // letterbox margins a page narrower than its window leaves down each side.
        cairo_set_source_rgb(cr, ((geo::kBgColor >> 16) & 0xFF) / 255.0,
                             ((geo::kBgColor >> 8) & 0xFF) / 255.0, (geo::kBgColor & 0xFF) / 255.0);
        cairo_paint(cr);
        const geo::PageSize ps = geo::pageSize(mPage);
        cairo_translate(cr, mOffX, mOffY);
        cairo_scale(cr, mScale, mScale);
        Canvas c(cr, &mFonts, static_cast<float>(ps.w), static_cast<float>(ps.h));
        drawStaticLayer(c);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    mBackground = surface;
}

void RationsEditorView::releaseBackground()
{
    if (mBackground) {
        cairo_surface_destroy(mBackground);
        mBackground = nullptr;
    }
}

//------------------------------------------------------------------------
void RationsEditorView::onDraw(cairo_t *cr)
{
    // The static layer first, as a straight 1:1 copy at device resolution. SOURCE rather than OVER
    // because this is the bottom of the stack and the ground is already composited into it.
    if (mBackground) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, mBackground, 0.0, 0.0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        // Could not allocate the cached surface: paint the ground inline, still in device space so
        // the margins are covered.
        cairo_save(cr);
        cairo_set_source_rgb(cr, ((geo::kBgColor >> 16) & 0xFF) / 255.0,
                             ((geo::kBgColor >> 8) & 0xFF) / 255.0, (geo::kBgColor & 0xFF) / 255.0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    // From here on everything is in logical units; this is the only transform.
    cairo_save(cr);
    cairo_translate(cr, mOffX, mOffY);
    cairo_scale(cr, mScale, mScale);
    const geo::PageSize ps = geo::pageSize(mPage);
    Canvas c(cr, &mFonts, static_cast<float>(ps.w), static_cast<float>(ps.h));
    if (!mBackground)
        drawStaticLayer(c);

    if (scrolling()) {
        // The page content, moved up by the scroll and clipped to the band below the fixed
        // header. The clip is what keeps a row that has scrolled past the top from drawing over
        // the back button, and it is the same rect the scroll range is computed from.
        const float top = static_cast<float>(geo::kPageContentTop);
        c.pushClip(
            Rect(0.0f, top, static_cast<float>(ps.w), static_cast<float>(viewportH()) - top));
        cairo_save(cr);
        cairo_translate(cr, 0.0, -mScrollY);
        compose(c);
        cairo_restore(cr);
        c.popClip();
    } else {
        compose(c);
    }

    // Chrome last, and outside the translate above: it does not scroll.
    composeChrome(c);
    cairo_restore(cr);
}

void RationsEditorView::compose(Canvas &c)
{
    switch (mPage) {
        case geo::Page::Head:
            composeHead(c);
            break;
        case geo::Page::Cabinet:
            composeCabinet(c);
            break;
        case geo::Page::Pedalboard:
            composePedalboard(c);
            break;
        case geo::Page::Settings:
            composeSettings(c);
            break;
    }
}

//------------------------------------------------------------------------
// Everything that stays put while the page scrolls. On a page that does not scroll this is drawn
// at exactly the coordinates it always was, so the three non-scrolling pages are unchanged.
void RationsEditorView::composeChrome(Canvas &c)
{
    // The way back, on every page but the head. Chrome rather than page content because it is the
    // exit: a scroll that can push the exit off the top of the window is a trap, and
    // kPageContentTop already reserved this band for it.
    if (mPage != geo::Page::Head)
        drawButton(c, geo::kBackButton);
    if (scrolling())
        drawScrollBar(c);
    // The browser is an overlay on the window, not a row of the page, so it neither scrolls nor
    // needs its clicks un-scrolled.
    if (mBrowser.isOpen())
        mBrowser.draw(c);
}

//------------------------------------------------------------------------
// Down the right-hand margin. Two colours and two rounded rects, which is the file browser's own
// scroll indicator at a size that can be grabbed — see geometry.h.
void RationsEditorView::drawScrollBar(Canvas &c)
{
    const Rect track = scrollTrackRect();
    c.setColor(geo::kGold, 60);
    c.fillRoundRect(track, geo::kScrollBarRadius);
    c.setColor(geo::kAccent, mScrollDrag ? 255 : 200);
    c.fillRoundRect(scrollThumbRect(), geo::kScrollBarRadius);
}

//------------------------------------------------------------------------
// The track runs the height of the scrolling band, inset top and bottom by the same margin it is
// inset from the page edge, so it reads as one shape rather than as a bar that touches the ends.
Rect RationsEditorView::scrollTrackRect() const
{
    const float inset = static_cast<float>(geo::kScrollBarInset);
    const float top = static_cast<float>(geo::kPageContentTop);
    const float bottom = static_cast<float>(viewportH()) - inset;
    return Rect::fromLTRB(static_cast<float>(geo::kScrollBarX), top,
                          static_cast<float>(geo::kScrollBarX + geo::kScrollBarW), bottom);
}

// Length proportional to the visible fraction, floored at kScrollThumbMinH so a long page still
// leaves something to catch hold of; position proportional to the scroll, against a travel that
// is the track minus the thumb rather than the whole track, or the thumb would run off the end.
Rect RationsEditorView::scrollThumbRect() const
{
    const Rect track = scrollTrackRect();
    const geo::PageSize ps = geo::pageSize(mPage);
    const double visible = viewportH();
    const float frac = ps.h > 0 ? static_cast<float>(visible / static_cast<double>(ps.h)) : 1.0f;
    const float thumbH = std::min(track.h, std::max(track.h * frac, geo::kScrollThumbMinH));
    const double pos = mScrollMax > 0.0 ? mScrollY / mScrollMax : 0.0;
    const float y = track.y + static_cast<float>(pos) * (track.h - thumbH);
    return Rect(track.x, y, track.w, thumbH);
}

//------------------------------------------------------------------------
// x and y are WINDOW coordinates: the bar is chrome and does not scroll with what it scrolls.
// Returns whether the click was the bar's, so the caller can fall through to the page's own rows
// when it was not.
bool RationsEditorView::handleScrollBarClick(float x, float y)
{
    if (!scrolling())
        return false;
    const Rect track = scrollTrackRect();
    // A few units of slack either side of a 10-unit bar, because at the 0.50 floor it is 5 device
    // pixels wide and a bar that has to be hit exactly is a bar nobody uses.
    if (!track.inset(-4.0f).contains(x, y))
        return false;

    const Rect thumb = scrollThumbRect();
    if (y < thumb.top() || y >= thumb.bottom()) {
        // Track click: page towards the pointer by one screenful, rather than jumping the thumb
        // to the click. A jump would make a mis-aimed click on a bar this narrow throw the page
        // to somewhere the user was not asking for, and there is no undo for a scroll.
        const double page = std::max(1.0, viewportH() - geo::kPageContentTop);
        setScroll(mScrollY + (y < thumb.top() ? -page : page));
        return true;
    }

    mScrollDrag = true;
    mScrollGrabY = y;
    mScrollGrabScroll = mScrollY;
    invalidate();
    return true;
}

//------------------------------------------------------------------------
void RationsEditorView::composeHead(Canvas &c)
{
    for (int i = 0; i < geo::kKnobCount; ++i)
        drawKnob(c, geo::kKnobs[i], true);

    // Five bat switches and five LEDs: one per channel, plus the gate. Exactly one channel LED is
    // ever lit, which is what kChannelId being a list parameter rather than four booleans buys.
    //
    // The switch and its lamp read DIFFERENT things, and on purpose. A bat switch on a real amp
    // moves the instant your hand moves it, so it follows the request; the lamp says which amp is
    // actually under your hands, so it follows the audio. They agree within a switch's length of
    // time, and while a channel's captures are still being built they visibly do not — which is
    // the honest report, and the reason the switch is held rather than faked.
    const int requested = requestedChannel();
    const int sounding = activeChannel();
    const bool gateOn = paramValue(kNoiseGateOnId) > 0.5;
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const bool channel = i < geo::kChannelToggleCount;
        drawToggle(c, geo::kToggles[i], channel ? (i == requested) : gateOn);
        drawLed(c, static_cast<float>(geo::kToggles[i].cx), static_cast<float>(geo::kLedCY),
                channel ? (i == sounding) : gateOn);
    }

    // The utility row: BYPASS and EQ, each with its own lamp beside it. Both lamps say the stage
    // is IN circuit, which is why the bypass one is lit when bypass is OFF: the parameter names
    // the switch's action and the lamp reports the signal path, and on this one switch those are
    // opposites. ToggleSpec::invert carries exactly the same fact for the bat.
    for (int i = 0; i < geo::kTopToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kTopToggles[i];
        const bool on = paramValue(t.id) > 0.5;
        drawToggle(c, t, on);
        drawLed(c, static_cast<float>(geo::kTopLedCX[i]), static_cast<float>(geo::kTopLedCY),
                t.invert ? !on : on);
    }

    drawMeter(c, geo::kInputMeter, mInDisp, mInPeak);
    drawMeter(c, geo::kOutputMeter, mOutDisp, mOutPeak);
    for (int i = 0; i < 2; ++i)
        drawKnob(c, geo::kIoKnobs[i], true);

    for (const geo::ButtonSpec &b : geo::kPageButtons)
        drawButton(c, b);
    drawGear(c);
}

//------------------------------------------------------------------------
void RationsEditorView::composeCabinet(Canvas &c)
{
    const bool active = blendActive();
    drawKnobAt(c, static_cast<float>(geo::kBlendCX), static_cast<float>(geo::kBlendCY),
               static_cast<float>(geo::kBlendR), paramValue(kIrBlendId));
    c.setFont(Font::Title);
    c.setFontSize(geo::kBlendLabelSize);
    c.setColor(active ? geo::kTextColor : geo::kDimColor);
    c.drawString("Blend", geo::kBlendCX - c.stringWidth("Blend") * 0.5f,
                 static_cast<float>(geo::kBlendLabelBaselineY));

    drawIrRow(c, 0);
    drawIrRow(c, 1);
}

//------------------------------------------------------------------------
void RationsEditorView::drawPedalboardStatic(Canvas &c)
{
    // The enclosures. Five blank boxes, blitted at kPedalW x kPedalH from art
    // stored at its own 468x691 — the cabinet's arrangement, not the head's,
    // because a pedal is drawn 2.46x down and would be soft if it were stored at
    // the size it is drawn. Everything on the face is drawn over this.
    for (int i = 0; i < geo::kPedalCount; ++i) {
        const geo::PedalSpec &p = geo::kPedals[i];
        if (cairo_surface_t *art = mImages.get(p.art))
            c.drawImage(art, Rect(static_cast<float>(geo::kPedalLeft(i)),
                                  static_cast<float>(p.y), static_cast<float>(geo::kPedalW),
                                  static_cast<float>(geo::kPedalH)));
    }

    // The patch cables, between neighbours within a row only. Both jacks of a
    // pair sit at the same height, so strokeConnector's horizontal S-curve would
    // draw a dead straight line; the control points go DOWN instead, which is the
    // sag a slack cable has and the thing that makes it read as a cable at all.
    c.setColor(geo::kDimColor);
    c.setPenSize(geo::kPedalCablePen);
    for (int i = 0; i + 1 < geo::kPedalCount; ++i) {
        if (geo::kPedals[i].post != geo::kPedals[i + 1].post)
            continue; // the amplifier is what sits between the two rows
        const float x0 = static_cast<float>(geo::kPedalLeft(i) + geo::kPedalW);
        const float x1 = static_cast<float>(geo::kPedalLeft(i + 1));
        const float y = static_cast<float>(geo::kPedals[i].y + geo::kPedalJackY);
        const float d = (x1 - x0) * 0.35f;
        c.strokeBezier(x0, y, x0 + d, y + geo::kPedalCableSag, x1 - d,
                       y + geo::kPedalCableSag, x1, y);
    }

    // Which half of the chain each row is. Michroma, like every other legend.
    c.setFont(Font::Title);
    c.setFontSize(geo::kPedalRowLegendSize);
    c.setColor(geo::kDimColor);
    c.drawString("PRE", static_cast<float>(geo::kPedalRowLegendX),
                 static_cast<float>(geo::kPedalRow1Y - geo::kPedalRowLegendDY));
    c.drawString("POST", static_cast<float>(geo::kPedalRowLegendX),
                 static_cast<float>(geo::kPedalRow2Y - geo::kPedalRowLegendDY));
}

void RationsEditorView::composePedalboard(Canvas &c)
{
    // The enclosures, the cables and the row legends are static and live in
    // drawPedalboardStatic, composited once per resize. Everything below reads a
    // parameter, so it is drawn every frame.
    for (int i = 0; i < geo::kPedalCount; ++i)
        drawPedal(c, i);
}

//------------------------------------------------------------------------
// The pedalboard's lettering: plain white, and that is the whole of it. Two ways of backing it up
// on the bright enclosures were built, rendered and rejected - see the SILKSCREEN note in
// geometry.h, which records what each one did and why plain white beat both.
static void drawPedalString(Canvas &c, const char *text, float x, float y)
{
    c.setColor(geo::kPedalInk);
    c.drawString(text, x, y);
}

// One pedal's face. NOTHING here is per-pedal: the knobs, their legends and the mini controls all
// come out of kPedalParams, which is what lets five different pedals share one function and what
// makes adding a control to one of them a one-line change to the table rather than a new block of
// drawing code.
//
// TWO RULES, and both are stated here because everything below is an application of them.
//
// THE SILKSCREEN DOES NOT DIM. A pedal that is switched off is drawn exactly as one that is on,
// and the LED is the only thing that reports state - which is what the object does, and which
// follows the reasoning already written at drawToggle: a legend that dims reads as "disabled", and
// nothing on this page ever is. Every control here stays live whether the pedal is in circuit or
// not, because a player dials a delay in and then stomps it, and a face that refused clicks until
// the LED was lit would mean the only way to set a pedal up is to hear it first.
//
// FILLED IS LIVE, OUTLINED IS IDLE, in white. That replaces the accent colour, which cannot be
// used on this page: kAccent is a green that vanishes on the Boost, shouts on the Chorus, and says
// nothing on any of them that a filled white plate does not say better.
void RationsEditorView::drawPedal(Canvas &c, int pedal)
{
    const geo::PedalSpec &p = geo::kPedals[pedal];
    const bool on = paramValue(kPedalOnId[pedal]) > 0.5;

    for (int k = 0, nk = pedalKnobCount(pedal); k < nk; ++k) {
        const PedalParamSpec &spec = kPedalParams[pedalKnobParam(pedal, k)];
        const geo::PedalPoint pt = geo::pedalKnobCenter(pedal, k);
        const float cx = static_cast<float>(pt.x);
        const float cy = static_cast<float>(pt.y);
        drawKnobAt(c, cx, cy, static_cast<float>(geo::kPedalKnobR), paramValue(spec.id));

        // ONE text row per knob, and the value takes it while the knob is held - the same rule the
        // head panel's dials follow, and for the same reason: there is one row of space under a
        // knob and a second one would land on the next row of knobs or on the LED.
        const bool showValue = (mDragParam == spec.id);
        const std::string text =
            showValue ? paramText(spec.id) : std::string(spec.legend ? spec.legend : "");
        c.setFont(showValue ? Font::Body : Font::Title);
        c.setFontSize(static_cast<float>(geo::kPedalLabelSize));
        const std::string fit =
            c.clipToWidth(text, static_cast<float>(geo::pedalKnobLabelAllowance(pedal, k)));
        const float w = c.stringWidth(fit.c_str());
        const float baseline = cy + geo::kPedalKnobR + geo::kPedalLabelDY;
        if (showValue) {
            // The readout is the one thing on the face that has to be seen at a glance while the
            // pointer is moving, so it inverts rather than changing colour.
            c.setColor(geo::kPedalInk);
            c.fillRoundRect(Rect(cx - w * 0.5f - 4.0f, baseline - geo::kPedalLabelSize, w + 8.0f,
                                 geo::kPedalLabelSize + 4.0f),
                            static_cast<float>(geo::kPedalMiniRadius));
            c.setColor(geo::kPedalInkPlate);
            c.drawString(fit.c_str(), cx - w * 0.5f, baseline);
        } else {
            drawPedalString(c, fit.c_str(), cx - w * 0.5f, baseline);
        }
    }

    // The mini slots. A list draws its VALUE, a toggle draws its own name, and both fill when they
    // are doing something: a sync division other than Free, or a ping-pong that is on.
    for (int m = 0, nm = pedalMiniCount(pedal); m < nm; ++m) {
        const PedalParamSpec &spec = kPedalParams[pedalMiniParam(pedal, m)];
        const geo::PedalPoint pt = geo::pedalMiniCenter(pedal, m);
        const Rect box(static_cast<float>(pt.x - geo::kPedalMiniW / 2),
                       static_cast<float>(pt.y - geo::kPedalMiniH / 2),
                       static_cast<float>(geo::kPedalMiniW), static_cast<float>(geo::kPedalMiniH));
        // A toggle is live when it is on; a list is live when it is off entry 0, which for the one
        // list that exists means the Delay is following the host's tempo rather than its own knob.
        const bool live = paramValue(spec.id) > (spec.kind == PedalParamKind::Toggle ? 0.5 : 0.0);
        c.setColor(geo::kPedalInk);
        if (live) {
            c.fillRoundRect(box, static_cast<float>(geo::kPedalMiniRadius));
        } else {
            c.setPenSize(1.0f);
            c.strokeRoundRect(box, static_cast<float>(geo::kPedalMiniRadius));
        }

        std::string text = spec.kind == PedalParamKind::Toggle ? std::string(spec.legend)
                                                              : paramText(spec.id);
        if (text.empty())
            text = spec.legend; // the controller had no text for it; name it rather than draw a gap
        c.setFont(Font::Title);
        c.setFontSize(static_cast<float>(geo::kPedalMiniSize));
        const std::string fit = c.clipToWidth(text, static_cast<float>(geo::kPedalMiniW - 6));
        const float tx = box.centerX() - c.stringWidth(fit.c_str()) * 0.5f;
        const float ty = box.centerY() + geo::kPedalMiniSize * 0.36f;
        if (live) {
            c.setColor(geo::kPedalInkPlate);
            c.drawString(fit.c_str(), tx, ty); // dark ON the white plate
        } else {
            drawPedalString(c, fit.c_str(), tx, ty);
        }
    }

    const geo::PedalPoint led = geo::pedalLedCenter(pedal);
    drawLed(c, static_cast<float>(led.x), static_cast<float>(led.y), on,
            static_cast<float>(geo::kPedalLedR));

    const geo::PedalPoint sw = geo::pedalSwitchCenter(pedal);
    drawPedalSwitch(c, static_cast<float>(sw.x), static_cast<float>(sw.y));

    // The name, in Michroma like every other legend, clipped to the body so a longer one could
    // never reach the enclosure's edge. panelrender measures all five against exactly this
    // allowance.
    c.setFont(Font::Title);
    c.setFontSize(static_cast<float>(geo::kPedalNameSize));
    const std::string name =
        c.clipToWidth(p.name, static_cast<float>(geo::kPedalFaceW));
    drawPedalString(c, name.c_str(),
                     geo::kPedalLeft(pedal) + geo::kPedalKnobCX -
                         c.stringWidth(name.c_str()) * 0.5f,
                     static_cast<float>(p.y + geo::kPedalNameY));
}

//------------------------------------------------------------------------
// The chrome footswitch cap. ONE image for both states, exactly as on the pedals
// being modelled: a stomp switch does not change appearance when it latches, and
// the LED two rows above it is what reports the state. Drawing an "on" and an
// "off" cap would be inventing a thing the object does not do.
void RationsEditorView::drawPedalSwitch(Canvas &c, float cx, float cy)
{
    const float r = static_cast<float>(geo::kPedalSwitchR);
    const Rect dest(cx - r, cy - r, 2.0f * r, 2.0f * r);
    const int px = static_cast<int>(std::lround(2.0 * r * mScale));
    if (cairo_surface_t *s = mImages.getScaled("pedal_switch", px, px)) {
        c.drawImage(s, dest);
        return;
    }
    // Degradation path: a ring and a cap, which is what the art is a photograph
    // of. Flat, but it never contradicts the thing it stands in for.
    c.setColor(0x6E7276);
    c.fillEllipse(dest);
    c.setColor(0xB6BABE);
    c.fillEllipse(cx, cy, r * 0.62f, r * 0.62f);
}

//------------------------------------------------------------------------
// The MIDI settings page. Four rows, one per channel; the gate is absent because it is
// deliberately not on the MIDI path at all.
//
// Four rows, one per channel, each showing what it is currently learned to and carrying the two
// buttons that change that. The gate is deliberately absent: it is not on the MIDI path at all
// and stays on for as long as the user has it on.
//
// The table being drawn is the CONTROLLER's copy, which is a copy: the authority is the
// processor's, because a footswitch has to keep working with this page closed and this whole view
// destroyed. That is also why a learn completes without anything here being told - the moment it
// happens is on the audio thread - so an armed row polls for the answer (see pollMidi).
void RationsEditorView::composeSettings(Canvas &c)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Settings));

    // Section 1: the capture loaders. First on the page because a channel with nothing loaded does
    // nothing, so this is the section that has to be found before any other one means anything.
    c.setFont(Font::Title);
    c.setFontSize(geo::kSettingsHeadingSize);
    c.setColor(geo::kTextColor);
    c.drawString(geo::kCaptureHeading, cx - c.stringWidth(geo::kCaptureHeading) * 0.5f,
                 static_cast<float>(geo::kCaptureHeadingY));
    for (int i = 0; i < geo::kCaptureRowCount; ++i)
        drawCaptureRow(c, i);
    c.setFont(Font::Body);
    c.setFontSize(geo::kSettingsFootnoteSize);
    c.setColor(geo::kDimColor);
    c.drawString(geo::kCaptureFootnote, cx - c.stringWidth(geo::kCaptureFootnote) * 0.5f,
                 static_cast<float>(geo::kCaptureFootnoteY));

    c.setFont(Font::Title);
    c.setFontSize(geo::kSettingsHeadingSize);
    c.setColor(geo::kTextColor);
    c.drawString(geo::kLevelHeading, cx - c.stringWidth(geo::kLevelHeading) * 0.5f,
                 static_cast<float>(geo::kLevelHeadingY));

    for (int i = 0; i < geo::kLevelRowCount; ++i)
        drawLevelRow(c, i);

    c.setFont(Font::Body);
    c.setFontSize(geo::kSettingsFootnoteSize);
    c.setColor(geo::kDimColor);
    c.drawString(geo::kLevelFootnote, cx - c.stringWidth(geo::kLevelFootnote) * 0.5f,
                 static_cast<float>(geo::kLevelFootnoteY));

    c.setFont(Font::Title);
    c.setFontSize(geo::kSettingsHeadingSize);
    c.setColor(geo::kTextColor);
    c.drawString(geo::kMidiHeading, cx - c.stringWidth(geo::kMidiHeading) * 0.5f,
                 static_cast<float>(geo::kSettingsHeadingY));

    const int armed = mController ? mController->armedMidiRow() : -1;

    for (int i = 0; i < geo::kMidiRowCount; ++i) {
        const Rect r = midiRowRect(i);
        c.setColor(0x0C0B0A);
        c.fillRoundRect(r, 4.0f);
        c.setColor(geo::kGold, 190);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, 4.0f);

        const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(c.clipToWidth(mController->midiRowLabel(i), geo::kMidiTextX - 20.0f).c_str(),
                     r.x + 12.0f, base);

        const MidiBinding binding = mController ? mController->midiBinding(i) : MidiBinding();
        // The mapping is data, not a legend, so it is the body face. An armed row says what it is
        // waiting for rather than what it was, because that is the question the user has open.
        //
        // An unlearned row says NOTHING. It used to say "not learned", and that was one label per
        // row restating what the Learn button beside it already means - four lines of text on a
        // page whose whole job is to be read at a glance, none of which told anyone anything.
        if (i == armed || binding.learned()) {
            const std::string text =
                (i == armed) ? std::string(geo::kMidiListeningText) : describeBinding(binding);
            c.setFont(Font::Body);
            c.setColor(i == armed ? geo::kDimColor : geo::kTextColor);
            c.drawString(text.c_str(), r.x + static_cast<float>(geo::kMidiTextX), base);
        }

        if (binding.learned())
            drawButton(c, midiButton(i, true));
        drawButton(c, midiButton(i, false));
    }

    c.setFont(Font::Body);
    c.setFontSize(geo::kSettingsFootnoteSize);
    c.setColor(geo::kDimColor);
    // Two lines, and the second one is not decoration. A learned CC or Program Change answers on
    // every MIDI channel, because VST3 hands those over as parameter changes with the channel
    // already discarded (see midilearn.h) - so a player whose pedal sends on channel 2 and whose
    // keyboard sends the same CC on channel 1 needs to know that before they find out by playing.
    //
    // The third is the one the pedal rows made necessary: the two halves of this list do different
    // things with a press, and nothing about a row says which half it is in.
    const char *notes[geo::kSettingsFootnoteCount] = {geo::kSettingsFootnote,
                                                      geo::kSettingsFootnote2,
                                                      geo::kSettingsFootnote3};
    const int noteY[geo::kSettingsFootnoteCount] = {geo::kSettingsFootnoteY,
                                                    geo::kSettingsFootnote2Y,
                                                    geo::kSettingsFootnote3Y};
    for (int i = 0; i < geo::kSettingsFootnoteCount; ++i)
        c.drawString(notes[i], cx - c.stringWidth(notes[i]) * 0.5f, static_cast<float>(noteY[i]));

    // Section 4: the output section. Last because it is set once when a rig is assembled.
    c.setFont(Font::Title);
    c.setFontSize(geo::kSettingsHeadingSize);
    c.setColor(geo::kTextColor);
    c.drawString(geo::kOutputHeading, cx - c.stringWidth(geo::kOutputHeading) * 0.5f,
                 static_cast<float>(geo::kOutputHeadingY));
    drawOutputSection(c);
    c.setFont(Font::Body);
    c.setFontSize(geo::kSettingsFootnoteSize);
    c.setColor(geo::kDimColor);
    c.drawString(geo::kOutputFootnote, cx - c.stringWidth(geo::kOutputFootnote) * 0.5f,
                 static_cast<float>(geo::kOutputFootnoteY));
}

//------------------------------------------------------------------------
// One capture row: the channel's name on the left, what is loaded in the middle, a clear box at
// the end, and a build-progress hairline along the bottom edge while the bank is still coming up.
//
// This is the cabinet page's IR row with a name field added, and it is drawn from the same parts
// for the same reason: it is the same act. What differs is that a channel's source may be a FOLDER
// as well as a file, and that the row's left half is a name the user can change.
Rect RationsEditorView::captureRowRect(int row)
{
    return Rect(static_cast<float>(geo::kMidiRowX),
                static_cast<float>(geo::kCaptureRowY0 + row * geo::kMidiRowPitch),
                static_cast<float>(geo::kMidiRowW), static_cast<float>(geo::kMidiRowH));
}

Rect RationsEditorView::captureNameRect(int row)
{
    const Rect r = captureRowRect(row);
    return Rect(r.x + static_cast<float>(geo::kCaptureNameX), r.y + 4.0f,
                static_cast<float>(geo::kCaptureNameW), static_cast<float>(geo::kMidiRowH) - 8.0f);
}

Rect RationsEditorView::captureClearBox(int row)
{
    const Rect r = captureRowRect(row);
    return Rect(r.right() - static_cast<float>(geo::kCaptureClearInset) -
                    static_cast<float>(geo::kCaptureClearW),
                r.y + static_cast<float>(geo::kCaptureClearInset),
                static_cast<float>(geo::kCaptureClearW),
                static_cast<float>(geo::kMidiRowH) - 2.0f * geo::kCaptureClearInset);
}

void RationsEditorView::drawCaptureRow(Canvas &c, int row)
{
    const Rect r = captureRowRect(row);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);

    const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
    const std::string path = mController ? mController->capturePath(row) : std::string();
    const bool loaded = !path.empty();

    if (cairo_surface_t *icon = mSvgs.getByHeight(loaded ? "Folder" : "File", geo::kRowIconH))
        c.drawImageCentered(icon, r.x + 8 + geo::kRowIconH * 0.5f, r.centerY());

    // The name, in the legend face, because it is a legend: it is the same string the head panel
    // paints under that channel's dial. Clipped rather than allowed to run into the path beside
    // it — a user may type anything at all here.
    const Rect nameBox = captureNameRect(row);
    const bool editing = mRenaming == row;
    if (editing) {
        // A field only while it IS one. A box around every name all the time would make four text
        // fields out of what is, the rest of the time, four legends.
        c.setColor(0x000000, 170);
        c.fillRoundRect(nameBox, 3.0f);
        c.setColor(geo::kAccent, 220);
        c.setPenSize(1.0f);
        c.strokeRoundRect(nameBox, 3.0f);
    }
    c.setFont(Font::Title);
    c.setFontSize(geo::kMidiRowTextSize);
    c.setColor(editing ? geo::kAccentBright : geo::kTextColor);
    const std::string name =
        editing ? mRenameText : (mController ? mController->channelName(row) : std::string());
    const float nameX = nameBox.x + (editing ? 4.0f : 0.0f);
    const float nameW = nameBox.w - (editing ? 8.0f : 0.0f);
    c.drawString(c.clipToWidth(name, nameW).c_str(), nameX, base);
    if (editing) {
        // The caret, placed by measuring the text in front of it. Not blinked: a blink needs a
        // timer this page would otherwise not run, and a steady caret in the accent colour is
        // unambiguous on the only field on screen.
        const std::string before =
            c.clipToWidth(name.substr(0, std::min(mRenameCaret, name.size())), nameW);
        const float caretX = std::min(nameX + c.stringWidth(before.c_str()), nameX + nameW);
        c.setColor(geo::kAccentBright);
        c.setPenSize(1.0f);
        c.strokeLine(caretX, nameBox.y + 4.0f, caretX, nameBox.bottom() - 4.0f);
    }

    // What is loaded, in the body face — a path is a variable-length string that has to stay
    // legible when clipped, which is the one thing Roboto is kept for on this panel.
    std::string text;
    if (!loaded) {
        text = geo::kCapturePlaceholder;
    } else {
        text = pathBaseName(path);
        const int count = mController ? mController->entryCount(row) : 0;
        if (!mController->captureIsDirectory(row))
            text += "  (single capture)";
        else if (count > 0)
            text += "  (" + std::to_string(count) + " captures)";
    }
    c.setFont(Font::Body);
    c.setFontSize(geo::kFileRowTextSize);
    c.setColor(loaded ? geo::kTextColor : geo::kDimColor);
    c.drawString(c.clipToWidth(text, static_cast<float>(geo::kCaptureTextW)).c_str(),
                 r.x + static_cast<float>(geo::kCaptureTextX), base);

    if (loaded) {
        const Rect clear = captureClearBox(row);
        c.setColor(geo::kDimColor);
        c.setPenSize(1.5f);
        const Rect x = clear.inset(6.0f);
        c.strokeLine(x.left(), x.top(), x.right(), x.bottom());
        c.strokeLine(x.left(), x.bottom(), x.right(), x.top());
        c.setPenSize(1.0f);
    }
}

//------------------------------------------------------------------------
// One channel-level row: the channel's name, a horizontal slider centred on 0 dB, and the trim
// in dB beside it.
//
// A slider rather than a knob, and on this page rather than the faceplate, because there is no
// room for four more knobs on a 1133x403 head and because these are set once when a rig is put
// together rather than played. The centre mark is drawn through the track so the default is
// findable by eye; the readout is there because a level a player cannot name is a level they
// cannot repeat on the other channel.
Rect RationsEditorView::levelRowRect(int row)
{
    return Rect(static_cast<float>(geo::kMidiRowX),
                static_cast<float>(geo::kLevelRowY0 + row * geo::kMidiRowPitch),
                static_cast<float>(geo::kMidiRowW), static_cast<float>(geo::kMidiRowH));
}

Rect RationsEditorView::levelSliderRect(int row)
{
    const Rect r = levelRowRect(row);
    return Rect(r.x + static_cast<float>(geo::kLevelSliderX),
                r.centerY() - static_cast<float>(geo::kLevelThumbH) * 0.5f,
                static_cast<float>(geo::kLevelSliderW), static_cast<float>(geo::kLevelThumbH));
}

void RationsEditorView::drawLevelRow(Canvas &c, int row)
{
    const Rect r = levelRowRect(row);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);

    const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
    c.setFont(Font::Title);
    c.setFontSize(geo::kMidiRowTextSize);
    c.setColor(geo::kTextColor);
    const std::string rowName = mController ? mController->channelName(row) : std::string();
    c.drawString(c.clipToWidth(rowName, geo::kLevelSliderX - 20.0f).c_str(), r.x + 12.0f, base);

    const double norm = paramValue(kChannelLevelId[row]);
    const Rect slider = levelSliderRect(row);
    const float trackY = slider.centerY() - static_cast<float>(geo::kLevelTrackH) * 0.5f;
    const Rect track(slider.x, trackY, slider.w, static_cast<float>(geo::kLevelTrackH));

    c.setColor(0x000000, 170);
    c.fillRoundRect(track, static_cast<float>(geo::kLevelTrackH) * 0.5f);
    c.setColor(geo::kGold, 90);
    c.setPenSize(1.0f);
    c.strokeRoundRect(track, static_cast<float>(geo::kLevelTrackH) * 0.5f);

    // The centre mark, and the bar from it to the thumb: a trim reads as a departure from 0 dB,
    // so what is drawn is the departure rather than a fill from one end.
    const float travelX = slider.x + static_cast<float>(geo::kLevelThumbW) * 0.5f;
    const float centreX = travelX + geo::kLevelTravel * 0.5f;
    const float thumbX = travelX + geo::kLevelTravel * static_cast<float>(norm);
    if (std::fabs(thumbX - centreX) > 1.0f) {
        const Rect bar(std::min(centreX, thumbX), trackY, std::fabs(thumbX - centreX),
                       static_cast<float>(geo::kLevelTrackH));
        c.setColor(geo::kGold, 170);
        c.fillRect(bar);
    }
    c.setColor(geo::kGold, 200);
    c.setPenSize(1.0f);
    c.strokeLine(centreX, slider.centerY() - geo::kLevelCentreTickH * 0.5f, centreX,
                 slider.centerY() + geo::kLevelCentreTickH * 0.5f);

    const Rect thumb(thumbX - static_cast<float>(geo::kLevelThumbW) * 0.5f, slider.y,
                     static_cast<float>(geo::kLevelThumbW), static_cast<float>(geo::kLevelThumbH));
    c.setColor(0x2A2724);
    c.fillRoundRect(thumb, 3.0f);
    c.setColor(geo::kGold, 230);
    c.setPenSize(1.0f);
    c.strokeRoundRect(thumb, 3.0f);

    // The readout, right-aligned so the decimal points line up down the four rows and a level
    // that is one channel louder than another is visible as a column rather than read as text.
    const double db = ranges::kLevelMin + norm * (ranges::kLevelMax - ranges::kLevelMin);
    char text[24];
    snprintf(text, sizeof(text), "%+.1f dB", db);
    c.setFont(Font::Body);
    c.setFontSize(geo::kMidiRowTextSize);
    c.setColor(std::fabs(db) < 0.05 ? geo::kDimColor : geo::kTextColor);
    c.drawString(
        text, r.right() - static_cast<float>(geo::kLevelReadoutInset) - c.stringWidth(text), base);
}

//------------------------------------------------------------------------
// The output section: three radio rows for the mode, then the input-calibration pair.
//
// This was previously nowhere. The plug-in was hard-wired to Normalized with no way to see it, let
// alone change it, while both plug-ins it descends from expose exactly these three controls — so
// what a capture actually sounds like was a decision the user was not allowed to make.
Rect RationsEditorView::outputModeRow(int index)
{
    return Rect(static_cast<float>(geo::kMidiRowX),
                static_cast<float>(geo::kOutputRowY0 + index * geo::kOutputRowPitch),
                static_cast<float>(geo::kOutputRowW), static_cast<float>(geo::kOutputRowH));
}

Rect RationsEditorView::calToggleRect()
{
    return Rect(static_cast<float>(geo::kCalToggleCX) - geo::kToggleW * 0.5f,
                static_cast<float>(geo::kCalToggleCY) - geo::kToggleH * 0.5f,
                static_cast<float>(geo::kToggleW), static_cast<float>(geo::kToggleH));
}

Rect RationsEditorView::calValueRect()
{
    return Rect(static_cast<float>(geo::kCalValueX), static_cast<float>(geo::kCalValueY),
                static_cast<float>(geo::kCalValueW), static_cast<float>(geo::kCalValueH));
}

//------------------------------------------------------------------------
// Whether an output control has anything to work from, asked of the channel that is SOUNDING.
//
// Not of the requested channel: a switch whose target is still being primed has not happened yet,
// and what this answers has to describe the audio the player is listening to. And not of all four
// at once: the compensation is applied per capture and falls back to unity where the metadata is
// absent, so "some channel could use this" would grey nothing and tell nobody anything.
//
// Raw is always available — it is the absence of a compensation, and there is no metadata absence
// that can prevent doing nothing.
bool RationsEditorView::outputModeAvailable(int mode) const
{
    if (!mController || mode == kOutputRaw)
        return true;
    const int active = static_cast<int>(channelFromNorm(paramValue(kActiveChannelId)));
    // A channel with nothing loaded says NOTHING about what its captures support, because it has
    // none. Reporting "not in this channel's captures" there is answering a question nobody asked,
    // and it lands on the commonest state the plug-in is ever in - a fresh instance - where it
    // greys out Normalized, which is both the default and the setting that is already selected.
    // The upstream plug-in draws the same conclusion by returning early from its own control
    // update when no model is loaded, leaving every control alone until there is something to say.
    if (mController->entryCount(active) <= 0)
        return true;
    return mode == kOutputNormalized ? mController->bankHasLoudness(active)
                                     : mController->bankHasOutputLevel(active);
}

bool RationsEditorView::inputCalibrationAvailable() const
{
    if (!mController)
        return false;
    const int active = static_cast<int>(channelFromNorm(paramValue(kActiveChannelId)));
    if (mController->entryCount(active) <= 0)
        return true; // nothing loaded, so nothing to disable it over
    return mController->bankHasInputLevel(active);
}

void RationsEditorView::drawOutputSection(Canvas &c)
{
    const int current = static_cast<int>(outputModeFromNorm(paramValue(kOutputModeId)));

    for (int i = 0; i < kOutputModeCount; ++i) {
        const Rect row = outputModeRow(i);
        const bool gated = !outputModeAvailable(i);
        const float dotCX = row.x + geo::kOutputDotCX;
        const float dotCY = row.centerY();

        // Selection and availability are independent facts and are drawn independently: the RING
        // says whether these captures can honour this mode, the FILL says which mode is selected.
        //
        // The fill is always the accent, never dimmed with the rest of a gated row, and that is not
        // a detail. Normalized is the default and is gated whenever the loaded captures state no
        // loudness — which includes the commonest state of all, an instance with nothing loaded yet
        // — so dimming the selected dot along with its label draws the selected option fainter than
        // the unselected one, and Raw above it reads as the live setting. The plug-in was answering
        // the question correctly and drawing the wrong answer.
        c.setColor(gated ? 0x4A4740 : geo::kAccent);
        c.setPenSize(1.5f);
        c.strokeEllipse(dotCX, dotCY, geo::kOutputDotR, geo::kOutputDotR);
        if (i == current) {
            c.setColor(geo::kAccentBright);
            c.fillEllipse(dotCX, dotCY, geo::kOutputDotFillR, geo::kOutputDotFillR);
        }
        c.setPenSize(1.0f);

        // A greyed row says WHY, rather than merely looking inert. The reason is about the
        // captures the player currently has under their hands, and there is nothing else on the
        // page that could tell them.
        std::string label(geo::kOutputModeNames[i]);
        if (gated)
            label += geo::kOutputUnsupported;
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(gated ? 0x6A6460 : geo::kTextColor);
        c.drawString(label.c_str(), row.x + static_cast<float>(geo::kOutputTextX),
                     dotCY + geo::kMidiRowTextSize * 0.36f);
    }

    // Input calibration: the toggle, its legend, and the interface level it works against. The
    // whole block dims together, because the level means nothing with the toggle off and neither
    // means anything when the captures do not state what level they were fed.
    const bool calAvailable = inputCalibrationAvailable();
    const bool calOn = paramValue(kCalibrateInputId) > 0.5;
    const Rect tog = calToggleRect();
    // Bat UP for on, the same way the gate's switch reads, and scaled to device pixels the same
    // way drawToggle does — getScaled caches by the size it is asked for, so passing logical units
    // here would fill that cache with a second entry at the wrong resolution.
    const int pw = static_cast<int>(std::lround(geo::kToggleW * mScale));
    const int ph = static_cast<int>(std::lround(geo::kToggleH * mScale));
    if (cairo_surface_t *art =
            mImages.getScaled(calOn ? "switch_up_ring" : "switch_down_ring", pw, ph))
        c.drawImage(art, tog);
    else
        drawToggleFallback(c, tog, calOn);

    c.setFont(Font::Title);
    c.setFontSize(geo::kMidiRowTextSize);
    c.setColor(calAvailable ? geo::kTextColor : 0x6A6460);
    c.drawString(geo::kCalibrateLabel, static_cast<float>(geo::kCalLabelX),
                 tog.centerY() + geo::kMidiRowTextSize * 0.36f);

    const Rect value = calValueRect();
    c.setColor(0x000000, 170);
    c.fillRoundRect(value, 4.0f);
    c.setColor(geo::kGold, calAvailable ? 190 : 80);
    c.setPenSize(1.0f);
    c.strokeRoundRect(value, 4.0f);
    char dbu[24];
    snprintf(dbu, sizeof(dbu), "%+.1f dBu",
             ranges::kCalMin + paramValue(kInputCalLevelId) * (ranges::kCalMax - ranges::kCalMin));
    c.setFont(Font::Body);
    c.setFontSize(geo::kMidiRowTextSize);
    c.setColor(calAvailable ? geo::kTextColor : 0x6A6460);
    c.drawString(dbu, value.centerX() - c.stringWidth(dbu) * 0.5f,
                 value.centerY() + geo::kMidiRowTextSize * 0.36f);
}

//------------------------------------------------------------------------
// The geometry of one settings row and its buttons, in one place, so the painter and the hit test
// cannot drift apart - which for a Clear button that only exists on a learned row is not a
// theoretical risk.
Rect RationsEditorView::midiRowRect(int row)
{
    return Rect(static_cast<float>(geo::kMidiRowX), static_cast<float>(geo::midiRowY(row)),
                static_cast<float>(geo::kMidiRowW), static_cast<float>(geo::kMidiRowH));
}

geo::ButtonSpec RationsEditorView::midiButton(int row, bool clear) const
{
    const Rect r = midiRowRect(row);
    const int learnX = static_cast<int>(r.right()) - geo::kMidiLearnInset - geo::kMidiLearnW;
    const bool armed = mController && mController->armedMidiRow() == row;
    if (clear)
        return {learnX - geo::kMidiButtonGap - geo::kMidiClearW,
                static_cast<int>(r.y) + geo::kMidiLearnInset,
                geo::kMidiClearW,
                geo::kMidiRowH - 2 * geo::kMidiLearnInset,
                geo::kMidiClearLabel,
                geo::Page::Settings};
    return {learnX,
            static_cast<int>(r.y) + geo::kMidiLearnInset,
            geo::kMidiLearnW,
            geo::kMidiRowH - 2 * geo::kMidiLearnInset,
            armed ? geo::kMidiListenLabel : geo::kMidiLearnLabel,
            geo::Page::Settings};
}

//------------------------------------------------------------------------
bool RationsEditorView::handleSettingsClick(float x, float y)
{
    if (!mController)
        return false;

    // A click anywhere else ends a rename in progress, and ends it by KEEPING what was typed.
    // Clicking away from a field just filled in and having it silently discarded is the behaviour
    // nobody wants; Escape is there for the other answer.
    const int wasRenaming = mRenaming;

    // The capture rows. The name field and the clear box are tested before the row itself, because
    // both sit inside it and a row-wide test would swallow them — the same ordering the cabinet
    // page's IR rows already use for their own clear box.
    for (int i = 0; i < geo::kCaptureRowCount; ++i) {
        const Rect row = captureRowRect(i);
        if (!row.contains(x, y))
            continue;
        if (captureNameRect(i).contains(x, y)) {
            if (wasRenaming >= 0 && wasRenaming != i)
                commitRename();
            if (mRenaming != i)
                beginRename(i);
            return true;
        }
        if (wasRenaming >= 0)
            commitRename();
        if (!mController->capturePath(i).empty() && captureClearBox(i).contains(x, y)) {
            mController->setCaptureSource(i, "", false);
            invalidate();
            return true;
        }
        openCaptureBrowser(i);
        return true;
    }
    if (wasRenaming >= 0)
        commitRename();

    // The output mode. A gated row is inert rather than merely grey: the compensation it selects
    // falls back to unity for these captures, so letting it be chosen would be offering a control
    // that does nothing and looks as though it did something.
    for (int i = 0; i < kOutputModeCount; ++i) {
        if (!outputModeRow(i).contains(x, y))
            continue;
        // Selectable even when the loaded captures cannot honour it, and that is deliberate. The
        // mode is a persistent preference about how this plug-in should behave, not a statement
        // about the folder that happens to be loaded right now: refusing the click would mean a
        // player who loads a metadata-less bank cannot express "Normalized" until they load a
        // different one, and the setting they could not make is the default. The engine already
        // handles it gracefully - entryGain() falls back to unity per entry, so an unhonourable
        // mode sounds exactly like Raw for exactly the captures that cannot honour it - and the
        // greyed label beside the row is what says so. The upstream plug-in does the same: it
        // relabels the option and never disables it.
        editParam(kOutputModeId, normFromOutputMode(static_cast<OutputMode>(i)));
        invalidate();
        return true;
    }
    if (inputCalibrationAvailable()) {
        if (calToggleRect().contains(x, y)) {
            editParam(kCalibrateInputId, paramValue(kCalibrateInputId) > 0.5 ? 0.0 : 1.0);
            invalidate();
            return true;
        }
        if (calValueRect().contains(x, y)) {
            startDrag(kInputCalLevelId, x, y, false);
            invalidate();
            return true;
        }
    }

    for (int i = 0; i < geo::kLevelRowCount; ++i) {
        // The whole row is the target, not just the thumb: this is a relative drag, so where
        // inside the row it starts makes no difference, and a 12 px thumb is not something to
        // make anyone aim at.
        if (levelRowRect(i).contains(x, y)) {
            startDrag(kChannelLevelId[i], x, y, true);
            invalidate();
            return true;
        }
    }
    for (int i = 0; i < geo::kMidiRowCount; ++i) {
        if (mController->midiBinding(i).learned() &&
            buttonRect(midiButton(i, true)).contains(x, y)) {
            mController->clearMidiLearn(i);
            invalidate();
            return true;
        }
        if (buttonRect(midiButton(i, false)).contains(x, y)) {
            // Clicking the row that is already listening stops it listening. A Learn button with
            // no way out would leave the plug-in waiting to be taught by the next thing the
            // player happened to touch.
            const bool armed = mController->armedMidiRow() == i;
            mController->armMidiLearn(armed ? -1 : i);
            mMidiPollTicks = 1;
            invalidate();
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
// A dial is the bitmap rotated about its own centre. The art carries a gold pointer straight up at
// mid-travel, so nothing is drawn on top of it — a code-drawn notch or value arc would fight it.
void RationsEditorView::drawKnobAt(Canvas &c, float cx, float cy, float r, double norm)
{
    const Rect face(cx - r, cy - r, 2.0f * r, 2.0f * r);
    const int px = static_cast<int>(std::lround(2.0 * r * mScale * kDialSupersample));
    if (cairo_surface_t *dial = mImages.getScaled("dial", px, px)) {
        c.drawImageRotated(dial, face, (norm - 0.5) * geo::kKnobSweepDeg);
    } else {
        c.setColor(0x28282E);
        c.fillEllipse(face);
        c.setColor(geo::kGold);
        c.setPenSize(2.0f);
        const double a = (norm - 0.5) * geo::kKnobSweepDeg * 3.14159265358979323846 / 180.0;
        c.strokeLine(cx, cy, cx + static_cast<float>(std::sin(a)) * r * 0.8f,
                     cy - static_cast<float>(std::cos(a)) * r * 0.8f);
        c.setPenSize(1.0f);
    }
}

// Which channel a dial belongs to, or -1 for the shared controls. Decided by parameter id rather
// than by position in kKnobs: the table's order and the Channel enum's order agree today, and an
// index-based answer would keep looking correct if one of them were ever reordered.
static int channelOfKnob(const geo::KnobSpec &k)
{
    for (int c = 0; c < kChannelCount; ++c)
        if (k.id == kChannelGainId[c])
            return c;
    return -1;
}

void RationsEditorView::drawKnob(Canvas &c, const geo::KnobSpec &k, bool enabled)
{
    drawKnobAt(c, static_cast<float>(k.cx), static_cast<float>(k.cy), static_cast<float>(k.r),
               paramValue(k.id));

    const bool isIo = (k.id == kInputGainId || k.id == kOutputGainId);
    const float labelY =
        isIo ? static_cast<float>(geo::kIoLabelBaselineY) : (k.cy - geo::kKnobLabelDY);

    // ONE text row per dial, and while the dial is being dragged the readout takes it.
    //
    // Not a second row underneath, which is where a value would naturally go and where it cannot
    // go here: the band below the main row belongs to the bat switches, whose art spans 20 px
    // either side of their own centre line, and a readout there is drawn straight across the
    // levers. There is no gap to move it into — the dial's own art ends one pixel above the
    // switch art begins — so the row above is the only space that exists.
    //
    // Losing the legend for the duration costs nothing: the pointer is on the dial being held, so
    // which control it is is not in question, and the accent colour says the row is live rather
    // than silkscreen.
    const std::string readout = (mDragParam == k.id) ? knobReadout(k) : std::string();
    const bool showValue = !readout.empty();

    c.setFont(showValue ? Font::Body : Font::Title);
    c.setFontSize(showValue ? geo::kKnobValueSize
                            : (isIo ? geo::kIoLabelSize : geo::kKnobLabelSize));
    c.setColor(showValue ? geo::kAccent : (enabled ? geo::kTextColor : geo::kDimColor));
    // The four channel dials take their legend from the channel, not from this table: the user
    // names a channel by loading a folder into it or by typing over that name, and this is where
    // that shows on the faceplate. k.label stays as the fallback and as the string the art audit
    // measures — an arbitrary user name cannot be measured in advance, which is why the clip below
    // was already here and why it is what keeps a long one out of its neighbour's column.
    const std::string legend = channelOfKnob(k) >= 0 && mController
                                   ? mController->channelName(channelOfKnob(k))
                                   : std::string(k.label);
    const std::string text = c.clipToWidth(showValue ? readout : legend, geo::kKnobPitch - 6.0f);
    c.drawString(text.c_str(), k.cx - c.stringWidth(text.c_str()) * 0.5f, labelY);
}

//------------------------------------------------------------------------
std::string RationsEditorView::knobReadout(const geo::KnobSpec &k) const
{
    // A channel dial does not select a level, it selects a capture, so it names the capture. The
    // underlying 0..1 position means nothing to anyone reading an amp panel.
    for (int ch = 0; ch < kChannelCount; ++ch) {
        if (k.id != kChannelGainId[ch])
            continue;
        const int index = captureIndex(ch);
        if (index < 0 || index >= static_cast<int>(mCaptureNames[ch].size()))
            return std::string();
        return shortCaptureLabel(mCaptureNames[ch][static_cast<size_t>(index)]);
    }
    const std::string val = paramText(k.id);
    // Empty means the controller had no text for it. Return empty rather than falling back to the
    // label: echoing the label under the label reads as a rendering fault.
    if (val.empty())
        return std::string();
    return k.unit ? (val + " " + k.unit) : val;
}

//------------------------------------------------------------------------
// Degradation path for a missing switch PNG. It is a proxy for the art, so it tracks the art's
// geometry: the lever is fixed at the CENTRE and tapers out to a ball at the travelling end, so
// what moves is the ball. A crude shape here is fine; a shape that contradicts the art is not,
// because it inverts which end the eye reads as fixed.
void RationsEditorView::drawToggleFallback(Canvas &c, const Rect &dest, bool batUp)
{
    const float cx = dest.centerX();
    const float cy = dest.centerY();
    const float ballR = dest.w * 0.26f;
    const float ballY = batUp ? dest.y + ballR + 2.0f : dest.bottom() - ballR - 2.0f;
    c.setColor(batUp ? 0xE8EAEC : 0x9A9EA2);
    c.fillRect(Rect(cx - 2.0f, std::min(cy, ballY), 4.0f, std::fabs(ballY - cy)));
    c.fillEllipse(cx, ballY, ballR, ballR);
}

//------------------------------------------------------------------------
// `on` is the parameter, not the bat: ToggleSpec::invert is what turns "bypass engaged" into a bat
// pointing DOWN, which is how an amp reads.
void RationsEditorView::drawToggle(Canvas &c, const geo::ToggleSpec &t, bool on)
{
    const bool batUp = t.invert ? !on : on;
    const Rect dest(t.cx - geo::kToggleW / 2.0f, t.cy - geo::kToggleH / 2.0f, geo::kToggleW,
                    geo::kToggleH);
    const int pw = static_cast<int>(std::lround(geo::kToggleW * mScale));
    const int ph = static_cast<int>(std::lround(geo::kToggleH * mScale));
    if (cairo_surface_t *s =
            mImages.getScaled(batUp ? "switch_up_ring" : "switch_down_ring", pw, ph)) {
        c.drawImage(s, dest);
    } else {
        drawToggleFallback(c, dest, batUp);
    }

    // The legend names the switch, it does not report it. Dimming it when off would read as
    // "disabled", and for BYPASS that is backwards — bypass off is the normal state. The bat
    // carries the state.
    if (!t.label)
        return;
    c.setFont(Font::Title);
    c.setFontSize(geo::kToggleLabelSize);
    c.setColor(geo::kTextColor);
    c.drawString(t.label, t.cx - c.stringWidth(t.label) * 0.5f, t.cy + geo::kToggleLabelDY);
}

//------------------------------------------------------------------------
void RationsEditorView::drawLed(Canvas &c, float cx, float cy, bool lit, float r)
{
    const Rect dest(cx - r, cy - r, 2.0f * r, 2.0f * r);
    const int px = static_cast<int>(std::lround(2.0 * r * mScale));
    if (cairo_surface_t *led = mImages.getScaled(lit ? "led_on" : "led_off", px, px)) {
        c.drawImage(led, dest);
    } else {
        c.setColor(lit ? geo::kPeakColor : 0x141414);
        c.fillEllipse(dest);
    }
}

//------------------------------------------------------------------------
void RationsEditorView::drawMeter(Canvas &c, const geo::MeterRect &m, float level, float peak)
{
    const Rect rect(static_cast<float>(m.x), static_cast<float>(m.y), static_cast<float>(m.w),
                    static_cast<float>(m.h));
    const int pw = static_cast<int>(std::lround(m.w * mScale));
    const int ph = static_cast<int>(std::lround(m.h * mScale));
    if (cairo_surface_t *bg = mImages.getScaled("meter_track", pw, ph)) {
        c.drawImage(bg, rect);
    } else {
        c.setColor(0x0A0908);
        c.fillRoundRect(rect, 4.0f);
        c.setColor(geo::kGold);
        c.setPenSize(1.0f);
        c.strokeRoundRect(rect, 4.0f);
    }

    const Rect track = rect.inset(4.0f);
    const float lv = std::clamp(level, 0.0f, 1.0f);
    if (lv > 0.0f) {
        const float h = track.h * lv;
        const float top = track.bottom() - h;
        c.setColor(geo::kAccent);
        c.fillRect(Rect(track.x, top, track.w, h));
        // Ladder rungs, so the fill reads as segments rather than a bar.
        c.setColor(0x000000);
        for (float y = track.y + 2.0f; y < track.bottom(); y += 3.0f)
            c.fillRect(Rect(track.x, y, track.w, 1.0f));
        c.setColor(geo::kAccentBright);
        c.fillRect(Rect(track.x, top, track.w, 1.0f));
    }
    if (peak > 0.0f) {
        const float pv = std::min(peak, 1.0f);
        c.setColor(geo::kPeakColor);
        c.fillRect(Rect(track.x, track.bottom() - track.h * pv - 1.0f, track.w, 2.0f));
    }
}

//------------------------------------------------------------------------
void RationsEditorView::drawButton(Canvas &c, const geo::ButtonSpec &b, bool enabled)
{
    const Rect r = buttonRect(b);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, enabled ? 190 : 90);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);
    // Baseline from the button's centre rather than its bottom: the cap height is about 0.72 em in
    // Michroma, so half of that below the centre puts the legend optically centred at any size.
    c.setFont(Font::Title);
    c.setFontSize(geo::kPageButtonTextSize);
    c.setColor(enabled ? geo::kTextColor : geo::kDimColor);
    c.drawString(b.label, r.centerX() - c.stringWidth(b.label) * 0.5f,
                 r.centerY() + geo::kPageButtonTextSize * 0.36f);
}

//------------------------------------------------------------------------
// The parent plug-in's loader row unchanged, twice: an icon, prev/next arrows and a file name.
// Slot A alone is the normal case and must look and behave exactly as it does there.
void RationsEditorView::drawIrRow(Canvas &c, int slot)
{
    const geo::FileRow &row = irRow(slot);
    const Rect r = irRowRect(slot);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);

    std::string path;
    const bool loaded = irFile(slot, path);
    const float cy = r.centerY();

    if (cairo_surface_t *icon = mSvgs.getByHeight("File", geo::kRowIconH))
        c.drawImageCentered(icon, r.x + 8 + geo::kRowIconH * 0.5f, cy);

    // Step through the IRs in the loaded file's folder. Dimmed when nothing is loaded, because
    // with no folder to step through there is nothing for them to do — and an arrow that looks
    // live and does nothing is worse than one that says so.
    const uint8_t arrowAlpha = loaded ? 255 : 90;
    if (cairo_surface_t *left = mSvgs.getByHeight("ArrowLeft", geo::kIrArrowH))
        c.drawImageCentered(left, r.x + geo::kIrArrowPrevCX, cy, arrowAlpha);
    if (cairo_surface_t *right = mSvgs.getByHeight("ArrowRight", geo::kIrArrowH))
        c.drawImageCentered(right, r.x + geo::kIrArrowNextCX, cy, arrowAlpha);

    const float tx = r.x + geo::kIrTextDX;
    c.setFont(Font::Body);
    c.setFontSize(geo::kFileRowTextSize);
    c.setColor(loaded ? geo::kTextColor : geo::kDimColor);
    const std::string text = loaded ? pathBaseName(path) : std::string(row.placeholder);
    c.drawString(c.clipToWidth(text, r.w - (tx - r.x) - 30.0f).c_str(), tx, r.bottom() - 10.0f);

    if (loaded) {
        const Rect clear = rowClearBox(row);
        c.setColor(geo::kDimColor);
        c.setPenSize(1.5f);
        const Rect x = clear.inset(6.0f);
        c.strokeLine(x.left(), x.top(), x.right(), x.bottom());
        c.strokeLine(x.left(), x.bottom(), x.right(), x.top());
        c.setPenSize(1.0f);
    }
}

//------------------------------------------------------------------------
void RationsEditorView::drawGear(Canvas &c)
{
    if (cairo_surface_t *gear = mSvgs.getByHeight("Gear", 2 * geo::kGearR)) {
        c.drawImageCentered(gear, static_cast<float>(geo::kGearCX),
                            static_cast<float>(geo::kGearCY));
    } else {
        c.setColor(geo::kDimColor);
        c.setPenSize(1.5f);
        c.strokeEllipse(static_cast<float>(geo::kGearCX), static_cast<float>(geo::kGearCY),
                        geo::kGearR, geo::kGearR);
        c.setPenSize(1.0f);
    }
}

//------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------
double RationsEditorView::paramValue(Vst::ParamID id) const
{
    return mController ? mController->getParamNormalized(id) : 0.0;
}

std::string RationsEditorView::paramText(Vst::ParamID id) const
{
    if (!mController)
        return std::string();
    Vst::String128 str = {0};
    if (mController->getParamStringByValue(id, mController->getParamNormalized(id), str) !=
        kResultOk)
        return std::string();
    char buf[128] = "";
    UString(str, 128).toAscii(buf, sizeof(buf));
    return std::string(buf);
}

bool RationsEditorView::irFile(int slot, std::string &out) const
{
    if (!mController)
        return false;
    char buf[4096] = "";
    if (mController->getIrFile(slot, buf, sizeof(buf)) != kResultOk || buf[0] == 0)
        return false;
    out.assign(buf);
    return true;
}

// Both of these are 4-entry list values, so a normalized value is index/(count-1).
//
// They are deliberately two different parameters, and the difference is the whole of D4's "held,
// not faked": kChannelId is what the user, the host or a footswitch ASKED for, and
// kActiveChannelId is what the audio thread is actually sounding. A switch to a channel whose
// capture is still being built is held until it can be honoured, so the two disagree for as long
// as that takes. The bat switches write the request; the LEDs read the answer, which is why a
// lamp can never light over a channel that is not there yet.
int RationsEditorView::requestedChannel() const
{
    const double norm = paramValue(kChannelId);
    return std::clamp(static_cast<int>(std::lround(norm * (kChannelCount - 1))), 0,
                      kChannelCount - 1);
}

int RationsEditorView::activeChannel() const
{
    const double norm = paramValue(kActiveChannelId);
    return std::clamp(static_cast<int>(std::lround(norm * (kChannelCount - 1))), 0,
                      kChannelCount - 1);
}

// Which capture is sounding in channel `c`. The processor reports the active channel's index from
// the audio thread, but only once audio has actually run; before that (and in a stopped host, and
// for the three channels that are not sounding) the dial's own position is the honest answer, so
// the readout is never blank or wrong at index zero.
int RationsEditorView::captureIndex(int c) const
{
    if (c < 0 || c >= kChannelCount || mEntryCount[c] <= 0)
        return -1;
    if (c == activeChannel() && mActiveIndex >= 0 && mActiveIndex < mEntryCount[c])
        return mActiveIndex;
    if (mEntryCount[c] == 1)
        return 0;
    const double norm = paramValue(kChannelGainId[c]);
    return std::clamp(static_cast<int>(std::lround(norm * (mEntryCount[c] - 1))), 0,
                      mEntryCount[c] - 1);
}

// Both slots filled, or the dial is inert. With one IR that IR runs at unity and the blend does
// nothing, because a naive a*A + b*B with B silent would attenuate a one-IR user at every position
// but one.
bool RationsEditorView::blendActive() const
{
    std::string a, b;
    return irFile(0, a) && irFile(1, b);
}

bool RationsEditorView::hitCircle(float x, float y, float cx, float cy, float r)
{
    const float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

Rect RationsEditorView::buttonRect(const geo::ButtonSpec &b)
{
    return Rect(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.w),
                static_cast<float>(b.h));
}

const geo::FileRow &RationsEditorView::irRow(int slot)
{
    return slot == 0 ? geo::kIrRowA : geo::kIrRowB;
}

Rect RationsEditorView::irRowRect(int slot)
{
    const geo::FileRow &row = irRow(slot);
    return Rect(static_cast<float>(row.x), static_cast<float>(row.y), static_cast<float>(row.w),
                static_cast<float>(row.h));
}

Rect RationsEditorView::rowClearBox(const geo::FileRow &row)
{
    return Rect(row.x + row.w - 24.0f, row.y + 4.0f, 20.0f, row.h - 8.0f);
}

//------------------------------------------------------------------------
// Interaction
//------------------------------------------------------------------------
void RationsEditorView::editParam(Vst::ParamID id, double norm)
{
    if (!mController)
        return;
    mController->beginEdit(id);
    mController->setParamNormalized(id, norm);
    mController->performEdit(id, norm);
    mController->endEdit(id);
}

void RationsEditorView::nudgeParam(Vst::ParamID id, double delta)
{
    editParam(id, clampNorm(paramValue(id) + delta));
    invalidate();
}

void RationsEditorView::startDrag(Vst::ParamID id, float x, float y, bool horizontal)
{
    if (!mController)
        return;
    mDragParam = id;
    mDragStartX = x;
    mDragStartY = y;
    mDragHorizontal = horizontal;
    mDragStartNorm = paramValue(id);
    mController->beginEdit(id);
    // No explicit pointer grab is needed here: X delivers an implicit grab to the window that saw
    // the ButtonPress for as long as the button is held, and the Win32 view calls SetCapture for
    // the same reason. Both are the platform base's business, not this file's.
}

//------------------------------------------------------------------------
// One wheel click. On a channel dial that is one capture, so the wheel lands on real captures
// rather than between them — the same resting places the dial's own auto-detent glides to.
double RationsEditorView::wheelStep(Vst::ParamID id) const
{
    for (int c = 0; c < kChannelCount; ++c)
        if (id == kChannelGainId[c] && mEntryCount[c] > 1)
            return 1.0 / static_cast<double>(mEntryCount[c] - 1);
    return 0.05;
}

//------------------------------------------------------------------------
// The two boxes a pedal's non-knob controls live in, in page coordinates. File-static and shared
// by the click handler and the wheel handler so that a control's paint and its hit box cannot
// drift apart: geo::pedalMiniCenter is what drawPedal uses too.
static Rect pedalMiniRect(int pedal, int slot)
{
    const geo::PedalPoint pt = geo::pedalMiniCenter(pedal, slot);
    return Rect(static_cast<float>(pt.x - geo::kPedalMiniW / 2),
                static_cast<float>(pt.y - geo::kPedalMiniH / 2),
                static_cast<float>(geo::kPedalMiniW), static_cast<float>(geo::kPedalMiniH));
}

// A footswitch is STOMPED. The box is the enclosure's full body width and 60 units tall against a
// 44-unit cap, which is the same generosity the head's bat switches get (kToggleHitW = 60 against
// art 24 wide) and for the same reason: the thing being aimed at on the real object is a foot. It
// costs nothing here because the band it grows into holds nothing else - the mini slots stop at
// kPedalMiniY + kPedalMiniH / 2 and the name starts below the switch, both asserted in geometry.h.
static Rect pedalSwitchRect(int pedal)
{
    const geo::PedalPoint pt = geo::pedalSwitchCenter(pedal);
    return Rect(static_cast<float>(geo::kPedalLeft(pedal) + geo::kPedalBodyLeft),
                static_cast<float>(pt.y - geo::kPedalSwitchHitH / 2),
                static_cast<float>(geo::kPedalBodyRight - geo::kPedalBodyLeft),
                static_cast<float>(geo::kPedalSwitchHitH));
}

//------------------------------------------------------------------------
void RationsEditorView::onMouseDown(int x, int y, int button)
{
    // Physical pixels in, logical units out: this is the only place the page transform is undone,
    // and everything below hit-tests against geometry.h directly.
    const float fx = static_cast<float>((x - mOffX) / mScale);
    const float fy = static_cast<float>((y - mOffY) / mScale);

    // Right-click resets a channel trim to exactly 0 dB. The one gesture this editor answers on a
    // button other than the left, and it is here rather than on a double-click because neither
    // window class delivers double-clicks: the X11 view sees two ordinary presses and the Win32
    // one deliberately does not set CS_DBLCLKS, so that the two platforms behave the same.
    if (button == 3) {
        if (mPage == geo::Page::Settings && !mBrowser.isOpen() && mController) {
            const float cy = contentY(fy);
            for (int i = 0; i < geo::kLevelRowCount; ++i) {
                if (!levelRowRect(i).contains(fx, cy))
                    continue;
                const Vst::ParamID id = kChannelLevelId[i];
                mController->beginEdit(id);
                mController->setParamNormalized(id, 0.5);
                mController->performEdit(id, 0.5);
                mController->endEdit(id);
                invalidate();
                return;
            }
        }
        // On the pedalboard the second button steps a list BACKWARDS. Twelve sync divisions
        // reached by left-clicking forwards is eleven clicks to undo an overshoot; this is the
        // one gesture that makes a wrapping list usable with a mouse, and it costs nothing on the
        // controls that are not lists because nothing else answers here.
        if (mPage == geo::Page::Pedalboard && !mBrowser.isOpen() && mController) {
            for (int i = 0; i < geo::kPedalCount; ++i) {
                for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m) {
                    const PedalParamSpec &spec = kPedalParams[pedalMiniParam(i, m)];
                    if (spec.kind != PedalParamKind::List || !pedalMiniRect(i, m).contains(fx, fy))
                        continue;
                    cycleList(spec, -1);
                    return;
                }
            }
        }
        return;
    }
    if (button != 1) // left button otherwise
        return;
    mMouseX = fx;
    mMouseY = fy;

    // The file browser captures input while open.
    if (mBrowser.isOpen()) {
        const FileBrowser::Result r = mBrowser.handleClick(fx, fy);
        if (r == FileBrowser::Result::Chosen && mController) {
            if (mBrowserChannel >= 0) {
                // Which kind was picked comes from the browser, never from a fresh look at the
                // disk: a path can stop being a directory between the click and the question, and
                // what the user chose was fixed at the click.
                mController->setCaptureSource(mBrowserChannel, mBrowser.chosenPath().c_str(),
                                              mBrowser.chosenIsDirectory());
            } else {
                mController->setIrFile(mBrowserSlot, mBrowser.chosenPath().c_str());
            }
        }
        invalidate();
        return;
    }

    // The way back is on every page but the head, at the same place on all three.
    if (mPage != geo::Page::Head && buttonRect(geo::kBackButton).contains(fx, fy)) {
        setPage(geo::Page::Head);
        return;
    }

    switch (mPage) {
        case geo::Page::Head:
            handleHeadClick(fx, fy);
            return;
        case geo::Page::Cabinet:
            handleCabinetClick(fx, fy);
            return;
        case geo::Page::Settings:
            // The scrollbar is chrome, so it is hit-tested in window coordinates and gets first
            // refusal; the rows below it are page content and are asked in page coordinates.
            // Nothing above the fixed header band is content at all, which is what stops a click
            // beside the back button reaching a row that has scrolled underneath it.
            if (handleScrollBarClick(fx, fy))
                return;
            if (fy < static_cast<float>(geo::kPageContentTop))
                return;
            handleSettingsClick(fx, contentY(fy));
            return;
        case geo::Page::Pedalboard:
            handlePedalboardClick(fx, fy);
            return;
    }
}

//------------------------------------------------------------------------
bool RationsEditorView::handleHeadClick(float x, float y)
{
    if (hitCircle(x, y, geo::kGearCX, geo::kGearCY, geo::kGearR + 4)) {
        setPage(geo::Page::Settings);
        return true;
    }
    for (const geo::ButtonSpec &b : geo::kPageButtons) {
        if (buttonRect(b).contains(x, y)) {
            setPage(b.target);
            return true;
        }
    }

    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        if (hitCircle(x, y, k.cx, k.cy, k.r)) {
            startDrag(k.id, x, y);
            invalidate(); // the readout appears
            return true;
        }
    }
    for (int i = 0; i < 2; ++i) {
        const geo::KnobSpec &k = geo::kIoKnobs[i];
        if (hitCircle(x, y, k.cx, k.cy, k.r)) {
            startDrag(k.id, x, y);
            invalidate();
            return true;
        }
    }

    for (int i = 0; i < geo::kToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kToggles[i];
        // Bigger than the art, and covering the label — see kToggleHitW.
        const Rect bat(t.cx - geo::kToggleHitW / 2.0f,
                       static_cast<float>(t.cy + geo::kToggleHitTop), geo::kToggleHitW,
                       static_cast<float>(geo::kToggleHitBottom - geo::kToggleHitTop));
        if (!bat.contains(x, y))
            continue;
        if (i < geo::kChannelToggleCount) {
            // One list parameter, four views onto it. Clicking the switch that is already up is a
            // no-op: a real amp head has no all-channels-off position, so there is nothing for a
            // second click to mean.
            if (i != requestedChannel())
                editParam(kChannelId, i / static_cast<double>(kChannelCount - 1));
        } else {
            editParam(t.id, paramValue(t.id) > 0.5 ? 0.0 : 1.0);
        }
        invalidate();
        return true;
    }

    // The utility row. Both are plain booleans, so unlike the five above there is no list
    // parameter to map and no already-up case to ignore.
    for (const geo::ToggleSpec &t : geo::kTopToggles) {
        const Rect top(t.cx - geo::kToggleHitW / 2.0f,
                       static_cast<float>(t.cy + geo::kToggleHitTop), geo::kToggleHitW,
                       static_cast<float>(geo::kToggleHitBottom - geo::kToggleHitTop));
        if (!top.contains(x, y))
            continue;
        editParam(t.id, paramValue(t.id) > 0.5 ? 0.0 : 1.0);
        invalidate();
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
bool RationsEditorView::handleCabinetClick(float x, float y)
{
    if (blendActive() && hitCircle(x, y, geo::kBlendCX, geo::kBlendCY, geo::kBlendHitR)) {
        startDrag(kIrBlendId, x, y);
        invalidate();
        return true;
    }
    return handleIrRowClick(0, x, y) || handleIrRowClick(1, x, y);
}

//------------------------------------------------------------------------
// Step a list parameter by one, wrapping at both ends. Wrapping rather than clamping because
// twelve sync divisions in a 62-unit box are reached by repeated clicking, and a list that stops
// dead at "1/16T" makes the player click eleven times to get back to Free.
void RationsEditorView::cycleList(const PedalParamSpec &spec, int dir)
{
    const int n = static_cast<int>(spec.max - spec.min + 0.5) + 1;
    if (n <= 1)
        return;
    const int cur = static_cast<int>(pedalPlain(spec, paramValue(spec.id)) - spec.min + 0.5);
    const int next = ((cur + dir) % n + n) % n;
    editParam(spec.id, static_cast<double>(next) / static_cast<double>(n - 1));
    invalidate();
}

//------------------------------------------------------------------------
// Every control on this page, in one loop over kPedalParams. There is no per-pedal case and there
// is deliberately no room for one: what a pedal owns is its slice of the table, and the face was
// generated from that slice, so the hit test walks the same slice in the same order.
//
// A pedal that is switched OFF is still fully editable. That is not an oversight: a player dials
// a delay in and then stomps it, and the alternative - refusing clicks until the LED is lit -
// would mean the only way to set a pedal up is to hear it first.
bool RationsEditorView::handlePedalboardClick(float x, float y)
{
    for (int i = 0; i < geo::kPedalCount; ++i) {
        for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
            const geo::PedalPoint pt = geo::pedalKnobCenter(i, k);
            if (!hitCircle(x, y, static_cast<float>(pt.x), static_cast<float>(pt.y),
                           static_cast<float>(geo::kPedalKnobHitR)))
                continue;
            startDrag(kPedalParams[pedalKnobParam(i, k)].id, x, y);
            invalidate();
            return true;
        }
        for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m) {
            if (!pedalMiniRect(i, m).contains(x, y))
                continue;
            const PedalParamSpec &spec = kPedalParams[pedalMiniParam(i, m)];
            if (spec.kind == PedalParamKind::Toggle) {
                editParam(spec.id, paramValue(spec.id) > 0.5 ? 0.0 : 1.0);
                invalidate();
            } else {
                cycleList(spec, +1);
            }
            return true;
        }
        // Last, because its box is the generous one and would otherwise swallow a neighbour.
        if (pedalSwitchRect(i).contains(x, y)) {
            const Vst::ParamID id = kPedalOnId[i];
            editParam(id, paramValue(id) > 0.5 ? 0.0 : 1.0);
            invalidate();
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
bool RationsEditorView::handleIrRowClick(int slot, float x, float y)
{
    const geo::FileRow &row = irRow(slot);
    if (!irRowRect(slot).contains(x, y))
        return false;

    std::string path;
    const bool loaded = irFile(slot, path);

    if (loaded && rowClearBox(row).contains(x, y)) {
        if (mController)
            mController->setIrFile(slot, "");
        invalidate();
        return true;
    }

    // The arrows are tested BEFORE the fall-through that opens the browser, and they swallow the
    // click even with nothing loaded — otherwise clicking a greyed-out arrow would open the file
    // browser, which is not what an arrow looks like it does.
    const Rect prev(row.x + geo::kIrArrowPrevCX - geo::kIrArrowHitW * 0.5f, row.y + 2.0f,
                    geo::kIrArrowHitW, row.h - 4.0f);
    const Rect next(row.x + geo::kIrArrowNextCX - geo::kIrArrowHitW * 0.5f, row.y + 2.0f,
                    geo::kIrArrowHitW, row.h - 4.0f);
    if (prev.contains(x, y)) {
        if (loaded)
            stepIr(slot, -1);
        return true;
    }
    if (next.contains(x, y)) {
        if (loaded)
            stepIr(slot, +1);
        return true;
    }

    openIrBrowser(slot);
    return true;
}

//------------------------------------------------------------------------
void RationsEditorView::onMouseMove(int x, int y)
{
    mMouseX = static_cast<float>((x - mOffX) / mScale);
    // In PAGE coordinates, like every rect it is tested against. A drag reads deltas, and those
    // are the same either way, but the hover tests are not.
    mMouseY = contentY(static_cast<float>((y - mOffY) / mScale));

    if (mScrollDrag) {
        // The thumb's travel is the track minus the thumb, and the scroll's range is mScrollMax,
        // so a pixel of thumb is that ratio of page. Computed from the CURRENT thumb rather than
        // a value cached at grab time, because nothing can change its size mid-drag and reading
        // it here is one fewer thing that can go stale.
        const Rect track = scrollTrackRect();
        const float travel = track.h - scrollThumbRect().h;
        const float dy = static_cast<float>((y - mOffY) / mScale) - mScrollGrabY;
        setScroll(travel > 0.0f ? mScrollGrabScroll + dy * (mScrollMax / travel)
                                : mScrollGrabScroll);
        return;
    }

    if (!mDragParam || !mController)
        return;
    double norm;
    if (mDragHorizontal) {
        // Relative, never absolute: a slider that jumped to wherever it was clicked would let a
        // mis-click throw a channel's level by the whole range, and this control's whole job is
        // small corrections. The drag also snaps to exactly 0 dB near the centre, because a trim
        // has to be returnable to its default by hand.
        norm = clampNorm(mDragStartNorm + (mMouseX - mDragStartX) / geo::kLevelDragRange);
        const double centre = 0.5;
        if (std::fabs(norm - centre) * geo::kLevelDragRange < geo::kLevelCentreSnap)
            norm = centre;
    } else {
        norm = clampNorm(mDragStartNorm + (mDragStartY - mMouseY) / kKnobDragRange);
    }
    mController->setParamNormalized(mDragParam, norm);
    mController->performEdit(mDragParam, norm);
    invalidate();
}

void RationsEditorView::onMouseUp(int /*x*/, int /*y*/, int button)
{
    if (button != 1)
        return;
    if (mScrollDrag) {
        mScrollDrag = false;
        invalidate(); // the thumb goes back to its resting brightness
    }
    if (mDragParam && mController) {
        mController->endEdit(mDragParam);
        mDragParam = 0;
        invalidate(); // the readout goes away
    }
}

//------------------------------------------------------------------------
// Wheel adjusts whatever is under the cursor (wheel up = increase).
void RationsEditorView::onMouseWheel(int x, int y, int delta)
{
    const float fx = static_cast<float>((x - mOffX) / mScale);
    const float fy = static_cast<float>((y - mOffY) / mScale);

    if (mBrowser.isOpen()) {
        if (mBrowser.handleWheel(delta))
            invalidate();
        return;
    }
    if (mPage == geo::Page::Cabinet) {
        if (blendActive() && hitCircle(fx, fy, geo::kBlendCX, geo::kBlendCY, geo::kBlendHitR))
            nudgeParam(kIrBlendId, delta * 0.05);
        return;
    }
    if (mPage == geo::Page::Settings) {
        // WHEN THERE IS A SCROLLBAR, THE WHEEL SCROLLS. Nothing else on this page gets it.
        //
        // The first rule written here was the polite one — a control under the pointer keeps the
        // wheel, the background scrolls — and driving the built editor is what showed it does not
        // work. This page is rows: twelve of them, 592 units wide on a 640-unit page, with 8-unit
        // gaps. The pointer is almost always over one, so "scrolls unless over a control" means
        // "does not scroll", and the first test of it wound a channel trim to -11.5 dB while
        // trying to reach the output section.
        //
        // So the rule follows the scrollbar, which is the thing the user can see: a scrollbar
        // means the wheel scrolls, no scrollbar means it nudges whatever is under it. Nothing is
        // lost at the size the nudge was designed for, because a window showing the whole page
        // has no scrollbar and behaves exactly as it did. And at a short window the trims are
        // still dragged and still right-click back to 0 dB — the wheel was the third way to reach
        // them, not the only one.
        if (scrolling()) {
            // Wheel up scrolls the content up, which is towards the top of the page — the same
            // sign convention the file browser's own list uses.
            setScroll(mScrollY - delta * geo::kScrollWheelStep);
            return;
        }
        const float cy = contentY(fy);
        // One click is kLevelWheelDb, so a trim can be set to a round number without aiming: a
        // drag lands wherever the pointer does, and these are values a player wants to be able
        // to repeat on the other three channels.
        for (int i = 0; i < geo::kLevelRowCount; ++i) {
            if (levelRowRect(i).contains(fx, cy)) {
                nudgeParam(kChannelLevelId[i],
                           delta * geo::kLevelWheelDb / (ranges::kLevelMax - ranges::kLevelMin));
                return;
            }
        }
        // The interface calibration level, in whole decibels: an interface's stated level is a
        // round number, and this is how a user lands on theirs without aiming a drag at it.
        if (inputCalibrationAvailable() && calValueRect().contains(fx, cy)) {
            nudgeParam(kInputCalLevelId,
                       delta * geo::kCalWheelDb / (ranges::kCalMax - ranges::kCalMin));
            return;
        }
        return;
    }
    if (mPage == geo::Page::Pedalboard) {
        // This page fits its window at every legal scale, so it has no scrollbar and the wheel
        // nudges whatever is under the pointer - which is exactly what the settings page's rule
        // prescribes for a page that does not scroll.
        for (int i = 0; i < geo::kPedalCount; ++i) {
            for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
                const geo::PedalPoint pt = geo::pedalKnobCenter(i, k);
                if (!hitCircle(fx, fy, static_cast<float>(pt.x), static_cast<float>(pt.y),
                               static_cast<float>(geo::kPedalKnobHitR)))
                    continue;
                nudgeParam(kPedalParams[pedalKnobParam(i, k)].id, delta * 0.05);
                return;
            }
            for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m) {
                if (!pedalMiniRect(i, m).contains(fx, fy))
                    continue;
                const PedalParamSpec &spec = kPedalParams[pedalMiniParam(i, m)];
                // A list steps by whole entries: landing between two sync divisions is not a
                // state the parameter has, and a 0.05 nudge on a twelve-entry list would skip
                // some of them and repeat others.
                if (spec.kind == PedalParamKind::List)
                    cycleList(spec, delta > 0 ? 1 : -1);
                return;
            }
        }
        return;
    }
    if (mPage != geo::Page::Head)
        return;
    for (int i = 0; i < geo::kKnobCount; ++i) {
        const geo::KnobSpec &k = geo::kKnobs[i];
        if (hitCircle(fx, fy, k.cx, k.cy, k.r)) {
            nudgeParam(k.id, delta * wheelStep(k.id));
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        const geo::KnobSpec &k = geo::kIoKnobs[i];
        if (hitCircle(fx, fy, k.cx, k.cy, k.r)) {
            nudgeParam(k.id, delta * 0.05);
            return;
        }
    }
}

//------------------------------------------------------------------------
// Load the previous or next .wav in this slot's own folder, wrapping at both ends. No-op when
// nothing is loaded, or when the folder holds only the file already loaded.
//
// This runs on the UI thread, not the audio thread — it is a directory scan, and setIrFile() hands
// the path to the processor through the usual message path. Nothing here touches the RT path.
void RationsEditorView::stepIr(int slot, int dir)
{
    std::string current;
    if (!irFile(slot, current) || !mController)
        return;

    std::filesystem::path currentPath;
    if (!utf8ToPath(current, currentPath))
        return; // a path this platform cannot express; see platform/respath.h
    const std::filesystem::path folder = currentPath.parent_path();
    if (folder.empty())
        return;

    // A folder of IRs is untrusted input like any other: cap the scan so a directory with a
    // pathological number of entries cannot stall the editor — this runs inside a mouse click, on
    // the host's UI thread. The cap is generous because listing names is cheap; it exists to bound
    // the worst case, not to be reached.
    constexpr int kMaxScanned = 4096;

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return;
    int seen = 0;
    for (; it != std::filesystem::directory_iterator() && !ec; it.increment(ec)) {
        if (++seen > kMaxScanned)
            break;
        if (!it->is_regular_file(ec))
            continue;
        const std::string name = it->path().filename().string();
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && strcasecmp(name.c_str() + dot + 1, irRow(slot).ext) == 0)
            files.push_back(it->path());
    }
    if (files.size() < 2)
        return; // nothing to step to

    // By file name, not by full path: within one folder they order the same, and the name is what
    // the row shows and what the user is stepping through.
    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path &a, const std::filesystem::path &b) {
                  return a.filename().string() < b.filename().string();
              });

    // Compare the filename rather than the whole path. The directory iterator builds its entries
    // from `folder`, which came from the loaded path, so the two agree today — but a path that
    // differed only in separators or in a trailing slash would silently find nothing and the
    // arrows would go dead.
    const std::string currentName = currentPath.filename().string();
    int index = -1;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].filename().string() == currentName) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0)
        return; // the loaded file is no longer in its folder

    const int n = static_cast<int>(files.size());
    const size_t target = static_cast<size_t>(((index + dir) % n + n) % n);
    mController->setIrFile(slot, files[target].string().c_str());
    invalidate();
}

//------------------------------------------------------------------------
void RationsEditorView::openIrBrowser(int slot)
{
    std::string start;
    irFile(slot, start);
    mBrowserSlot = slot;
    mBrowserChannel = -1; // an IR row, not a capture row
    // The browser holds no geometry of its own — geometry.h stays the one authority on where
    // anything in this editor is drawn.
    mBrowser.setBounds(Rect(geo::kBrowserX, geo::kBrowserY, geo::kBrowserW, geo::kBrowserH));
    mBrowser.open(start, irRow(slot).ext,
                  slot == 0 ? "Select an impulse response" : "Select a second impulse response",
                  FileBrowser::Mode::File);
    invalidate();
}

//------------------------------------------------------------------------
// Renaming a channel.
//
// This is the only typed input in the editor, and it is deliberately the SECOND way to name a
// channel rather than the only one. Whether keys reach an embedded plug-in view at all is the
// host's business: the SDK routes them through IPlugView::onKeyDown and forbids taking them off
// the platform window, but a host that never gives the view focus never calls it, and on Linux
// that varies between hosts. So the primary name is the basename of whatever the channel loaded —
// a folder called JCM800 names the channel JCM800 with nothing typed — and this only ever
// OVERRIDES that. In a host that delivers no keys nothing here is reachable and nothing is broken.
void RationsEditorView::beginRename(int channel)
{
    if (!mController || channel < 0 || channel >= kChannelCount)
        return;
    mRenaming = channel;
    // Seeded with the name currently SHOWN, not with the override, so a user who wants to adjust
    // the folder's name starts from it rather than from an empty field.
    mRenameText = mController->channelName(channel);
    mRenameCaret = mRenameText.size();
    invalidate();
}

void RationsEditorView::commitRename()
{
    if (mRenaming < 0)
        return;
    const int channel = mRenaming;
    mRenaming = -1;
    if (mController) {
        // Typing the basename back is the same as having no override at all, and storing it as one
        // would freeze the name against a later load. So a name that matches what the channel would
        // be called anyway is stored as empty - which is also how a user clears an override without
        // being told there is such a thing.
        const std::string current = mController->channelName(channel);
        std::string next = mRenameText;
        while (!next.empty() && next.back() == ' ')
            next.pop_back();
        mController->setChannelName(channel, next == current ? "" : next.c_str());
    }
    mRenameText.clear();
    invalidate();
}

void RationsEditorView::cancelRename()
{
    mRenaming = -1;
    mRenameText.clear();
    invalidate();
}

// One key. Returns whether it was consumed, which is the whole contract: the SDK warns that
// answering kResultTrue for a key that was not handled blocks the host's own key commands, and in
// a DAW the space bar is one of those.
bool RationsEditorView::handleRenameKey(char16 key, int16 keyCode, int16 modifiers)
{
    if (mRenaming < 0)
        return false;

    switch (keyCode) {
        case Steinberg::KEY_RETURN:
        case Steinberg::KEY_ENTER:
            commitRename();
            return true;
        case Steinberg::KEY_ESCAPE:
            cancelRename();
            return true;
        case Steinberg::KEY_BACK:
            if (mRenameCaret > 0) {
                mRenameText.erase(--mRenameCaret, 1);
                invalidate();
            }
            return true;
        case Steinberg::KEY_DELETE:
            if (mRenameCaret < mRenameText.size()) {
                mRenameText.erase(mRenameCaret, 1);
                invalidate();
            }
            return true;
        case Steinberg::KEY_LEFT:
            if (mRenameCaret > 0) {
                --mRenameCaret;
                invalidate();
            }
            return true;
        case Steinberg::KEY_RIGHT:
            if (mRenameCaret < mRenameText.size()) {
                ++mRenameCaret;
                invalidate();
            }
            return true;
        case Steinberg::KEY_HOME:
            mRenameCaret = 0;
            invalidate();
            return true;
        case Steinberg::KEY_END:
            mRenameCaret = mRenameText.size();
            invalidate();
            return true;
        default:
            break;
    }

    // A printable character. Deliberately ASCII only: `key` is one UTF-16 code unit, and turning a
    // stream of those into UTF-8 correctly means handling surrogate pairs and dead keys, which is
    // a real piece of text handling rather than a few lines. A name outside ASCII is set by naming
    // the FOLDER, which goes through the filesystem and carries whatever bytes it likes. Flagged
    // here rather than left to be discovered.
    if (modifiers != 0 || key < 0x20 || key > 0x7E)
        return false;
    if (mRenameText.size() >= kRenameMaxChars)
        return true; // consumed, but the field is full
    mRenameText.insert(mRenameCaret, 1, static_cast<char>(key));
    ++mRenameCaret;
    invalidate();
    return true;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsEditorView::onKeyDown(char16 key, int16 keyCode, int16 modifiers)
{
    return handleRenameKey(key, keyCode, modifiers) ? kResultTrue : kResultFalse;
}

// Nothing is done on release, and kResultFalse is the honest answer for every key: reporting a
// release as handled would tell the host this view is consuming keys it is not.
tresult PLUGIN_API RationsEditorView::onKeyUp(char16, int16, int16)
{
    return kResultFalse;
}

//------------------------------------------------------------------------
// The capture browser. Mode::FileOrDirectory, which has been in FileBrowser since it was ported
// from the parent plug-in and until now had nothing here to call it: this plug-in shipped its
// captures and had no loader at all.
//
// Either answer is ordinary. A folder of captures is what a channel's dial sweeps and is the
// normal case; a single .nam is an ordinary thing to own, and everything below the editor has
// always been able to play one as a bank of one.
// The card is inset into the SETTINGS PAGE, and that page can be taller than the window showing
// it. So the height comes from the viewport rather than from the page: a card sized to 928 units
// in a window showing 400 of them would hang out of the bottom, taking its Choose button with it.
// recomputeLayout() calls this again on every resize, so a window dragged shorter with the picker
// already up is the same case rather than a special one.
void RationsEditorView::boundCaptureBrowser()
{
    const float y = static_cast<float>(geo::kCaptureBrowserY);
    const float maxH = static_cast<float>(viewportH()) - 2.0f * y;
    const float h = std::max(std::min(static_cast<float>(geo::kCaptureBrowserH), maxH),
                             static_cast<float>(geo::kCaptureBrowserMinH));
    mBrowser.setBounds(Rect(static_cast<float>(geo::kCaptureBrowserX), y,
                            static_cast<float>(geo::kCaptureBrowserW), h));
    // A card that just got shorter shows fewer rows, and the list may be scrolled past what is
    // left of it. The browser clamps its own scroll on the next wheel click, which is too late to
    // stop this frame drawing an empty list.
    mBrowser.clampScroll();
}

void RationsEditorView::openCaptureBrowser(int channel)
{
    if (!mController)
        return;
    mBrowserChannel = channel;
    boundCaptureBrowser();
    // Seeded from what this channel already has, so reopening the picker starts where the user
    // last was rather than at their home directory. That is the whole of "remembering" a folder
    // here: the path is in the state blob, so it survives the session too.
    mBrowser.open(mController->capturePath(channel), "nam",
                  "Select a capture, or a folder of captures", FileBrowser::Mode::FileOrDirectory);
    invalidate();
}

//------------------------------------------------------------------------
// Controller callbacks (host run-loop thread)
//------------------------------------------------------------------------
void RationsEditorView::ParamChanged(Vst::ParamID id, Vst::ParamValue value)
{
    switch (id) {
        case kInputMeterId:
            if (static_cast<float>(value) > mInDisp)
                mInDisp = static_cast<float>(value); // instant attack; onTick releases
            if (mInDisp > mInPeak) {
                mInPeak = mInDisp;
                mInPeakHold = kPeakHoldTicks;
            }
            return;
        case kOutputMeterId:
            if (static_cast<float>(value) > mOutDisp)
                mOutDisp = static_cast<float>(value);
            if (mOutDisp > mOutPeak) {
                mOutPeak = mOutDisp;
                mOutPeakHold = kPeakHoldTicks;
            }
            return;
        case kBankProgressId: {
            mBankProgress = static_cast<float>(value);
            // Crossing into "finished" is worth one more question: a capture that failed to parse
            // is skipped by the worker, which changes the count the editor was told earlier.
            const bool complete = mBankProgress >= 0.999f;
            if (complete != mProgressComplete) {
                mProgressComplete = complete;
                mCapsSettled = false;
                mCapsPollTicks = 0;
            }
            invalidate();
            return;
        }
        case kActiveIndexId: {
            const int c = activeChannel();
            const int index = mEntryCount[c] > 1
                                  ? static_cast<int>(std::lround(value * (mEntryCount[c] - 1)))
                                  : (mEntryCount[c] == 1 ? 0 : -1);
            if (index != mActiveIndex) {
                mActiveIndex = index;
                invalidate();
            }
            return;
        }
        default:
            break;
    }
    invalidate();
}

void RationsEditorView::ModelCapsChanged(const int entryCounts[kChannelCount],
                                         const std::vector<std::string> names[kChannelCount])
{
    for (int c = 0; c < kChannelCount; ++c) {
        mEntryCount[c] = entryCounts[c];
        mCaptureNames[c] = names[c];
    }
    if (mEntryCount[activeChannel()] <= 0)
        mActiveIndex = -1;
    invalidate();
}

//------------------------------------------------------------------------
// The capture names and the entry counts are produced by the processor's WORKER threads, so they
// do not exist yet when the plug-in is created and the caps sent then necessarily report an empty
// set. Ask again, twice a second, until the answer arrives and the builds have finished — then
// stop, so an idle editor sends nothing. The poll count is a backstop for a host that never
// processes audio, where the progress parameter would never reach 1 and this would otherwise never
// settle.
void RationsEditorView::pollCaps()
{
    if (!mController || mCapsSettled)
        return;
    if (--mCapsPollTicks > 0)
        return;
    mCapsPollTicks = kCapsPollTicks;

    mController->requestCaps();
    bool any = false;
    for (int c = 0; c < kChannelCount; ++c)
        any = any || mEntryCount[c] > 0;
    // The poll count is a backstop against asking forever, so it is checked whether or not an
    // answer has ever arrived. Gating it on `any` would leave the editor polling for the whole
    // session against a processor that never replies — which is exactly the case while the DSP
    // is not wired up yet, and would also be a host that never runs audio.
    if (++mCapsPolls > kCapsMaxPolls || (any && mProgressComplete))
        mCapsSettled = true;
}

//------------------------------------------------------------------------
// A learn completes on the AUDIO thread, which cannot send a message and cannot call the
// controller, so nothing pushes the answer here. While a row is armed the editor asks - twice a
// second, and only while the settings page is open with a row waiting - and stops the moment the
// processor reports the row is no longer armed. An idle editor sends nothing.
void RationsEditorView::pollMidi()
{
    if (!mController || mPage != geo::Page::Settings || mController->armedMidiRow() < 0)
        return;
    if (--mMidiPollTicks > 0)
        return;
    mMidiPollTicks = kMidiPollTicks;
    mController->requestMidiTable();
}

//------------------------------------------------------------------------
void RationsEditorView::onTick()
{
    pollCaps();
    pollMidi();

    const float in = mInDisp, out = mOutDisp;
    mInDisp *= kMeterRelease;
    mOutDisp *= kMeterRelease;

    if (mInPeakHold > 0)
        --mInPeakHold;
    else
        mInPeak *= kPeakRelease;
    if (mOutPeakHold > 0)
        --mOutPeakHold;
    else
        mOutPeak *= kPeakRelease;

    if (in > 0.002f || out > 0.002f || mInPeak > 0.002f || mOutPeak > 0.002f)
        invalidate();
}

} // namespace Rations
