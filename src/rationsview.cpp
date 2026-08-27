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

namespace
{

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
constexpr int kRowIconH = 16;

// Capability polling: every 15 ticks (~500 ms), and never more than this many times.
constexpr int kCapsPollTicks = 15;
constexpr int kCapsMaxPolls = 40;

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

    // The browser belongs to the cabinet page's rows; leaving it open across a page change would
    // draw a picker over a panel that has nothing to pick.
    mBrowser.close();
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
void RationsEditorView::recomputeLayout()
{
    const geo::PageSize ps = geo::pageSize(mPage);
    if (mDevW <= 0 || mDevH <= 0 || ps.w <= 0 || ps.h <= 0)
        return;
    double s = std::min(mDevW / static_cast<double>(ps.w), mDevH / static_cast<double>(ps.h));
    s = std::min(std::max(s, geo::kScaleMin), geo::kScaleMax);
    mScale = s;
    mOffX = (mDevW - ps.w * s) * 0.5;
    mOffY = (mDevH - ps.h * s) * 0.5;
}

//------------------------------------------------------------------------
// Aspect-locked to the CURRENT page. Fitting INSIDE whatever box the host proposes (min of the two
// ratios) is what makes dragging any edge or corner behave — dragging the bottom edge shrinks the
// width to match instead of being ignored.
void RationsEditorView::constrainSize(int &w, int &h) const
{
    const geo::PageSize ps = geo::pageSize(mPage);
    const double byW = w / static_cast<double>(ps.w);
    const double byH = h / static_cast<double>(ps.h);
    double s = std::min(byW, byH);
    s = std::min(std::max(s, geo::kScaleMin), geo::kScaleMax);
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
    compose(c);
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
    if (mBrowser.isOpen())
        mBrowser.draw(c);
}

//------------------------------------------------------------------------
void RationsEditorView::composeHead(Canvas &c)
{
    for (int i = 0; i < geo::kKnobCount; ++i)
        drawKnob(c, geo::kKnobs[i], true);

    // Five bat switches and five LEDs: one per channel, plus the gate. Exactly one channel LED is
    // ever lit, which is what kChannelId being a list parameter rather than four booleans buys.
    const int active = activeChannel();
    const bool gateOn = paramValue(kNoiseGateOnId) > 0.5;
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const bool on = (i < geo::kChannelToggleCount) ? (i == active) : gateOn;
        drawToggle(c, geo::kToggles[i], on);
        drawLed(c, static_cast<float>(geo::kToggles[i].cx), static_cast<float>(geo::kLedCY), on);
    }

    // Bypass. The LED follows the plug-in being IN circuit, so it is off when bypassed.
    const bool bypassed = paramValue(kBypassId) > 0.5;
    drawToggle(c, geo::kBypassToggle, bypassed);
    drawLed(c, static_cast<float>(geo::kBypassLedCX), static_cast<float>(geo::kBypassLedCY),
            !bypassed);

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
    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
void RationsEditorView::composePedalboard(Canvas &c)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Pedalboard));
    c.setFont(Font::Title);
    c.setFontSize(geo::kPedalPlaceholderSize);
    c.setColor(geo::kDimColor);
    c.drawString("Pedalboard", cx - c.stringWidth("Pedalboard") * 0.5f,
                 static_cast<float>(geo::kPedalPlaceholderY));
    c.setFont(Font::Body);
    c.setFontSize(geo::kPedalCaptionSize);
    const char *caption = "overdrive, flanger, chorus, delay, reverb";
    c.drawString(caption, cx - c.stringWidth(caption) * 0.5f,
                 static_cast<float>(geo::kPedalPlaceholderY + geo::kPedalCaptionDY));
    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
// The MIDI settings page. Four rows, one per channel; the gate is absent because it is
// deliberately not on the MIDI path at all.
//
// TODO (MIDI phase): the learn table itself does not exist yet, so every row reads "not learned"
// and the Learn buttons are drawn disabled and ignore clicks. Drawn disabled rather than omitted
// on purpose — a control that looks live and does nothing is worse than one that says it cannot,
// and the layout is what the later phase has to land in.
void RationsEditorView::composeSettings(Canvas &c)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Settings));
    c.setFont(Font::Title);
    c.setFontSize(geo::kSettingsHeadingSize);
    c.setColor(geo::kTextColor);
    c.drawString("MIDI Learn", cx - c.stringWidth("MIDI Learn") * 0.5f,
                 static_cast<float>(geo::kSettingsHeadingY));

    for (int i = 0; i < geo::kMidiRowCount; ++i) {
        const Rect r(static_cast<float>(geo::kMidiRowX),
                     static_cast<float>(geo::kMidiRowY0 + i * geo::kMidiRowPitch),
                     static_cast<float>(geo::kMidiRowW), static_cast<float>(geo::kMidiRowH));
        c.setColor(0x0C0B0A);
        c.fillRoundRect(r, 4.0f);
        c.setColor(geo::kGold, 190);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, 4.0f);

        const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(kChannelDirName[i], r.x + 12.0f, base);

        // The mapping is data, not a legend, so it is the body face.
        c.setFont(Font::Body);
        c.setColor(geo::kDimColor);
        c.drawString("not learned", r.x + 120.0f, base);

        const geo::ButtonSpec learn = {static_cast<int>(r.right()) - geo::kMidiLearnW -
                                           geo::kMidiLearnInset,
                                       static_cast<int>(r.y) + geo::kMidiLearnInset,
                                       geo::kMidiLearnW,
                                       geo::kMidiRowH - 2 * geo::kMidiLearnInset,
                                       "Learn",
                                       geo::Page::Settings};
        drawButton(c, learn, false);
    }

    c.setFont(Font::Body);
    c.setFontSize(geo::kSettingsFootnoteSize);
    c.setColor(geo::kDimColor);
    const char *note = "The gate switch is not learnable and stays where you leave it.";
    c.drawString(note, cx - c.stringWidth(note) * 0.5f,
                 static_cast<float>(geo::kSettingsFootnoteY));
    drawButton(c, geo::kBackButton);
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
    const std::string text =
        c.clipToWidth(showValue ? readout : std::string(k.label), geo::kKnobPitch - 6.0f);
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
void RationsEditorView::drawLed(Canvas &c, float cx, float cy, bool lit)
{
    const Rect dest(cx - geo::kLedR, cy - geo::kLedR, 2.0f * geo::kLedR, 2.0f * geo::kLedR);
    const int px = static_cast<int>(std::lround(2.0 * geo::kLedR * mScale));
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

    if (cairo_surface_t *icon = mSvgs.getByHeight("File", kRowIconH))
        c.drawImageCentered(icon, r.x + 8 + kRowIconH * 0.5f, cy);

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

// kChannelId is a 4-entry list parameter, so its normalized value is index/(count-1).
int RationsEditorView::activeChannel() const
{
    const double norm = paramValue(kChannelId);
    const int index = static_cast<int>(std::lround(norm * (kChannelCount - 1)));
    return std::clamp(index, 0, kChannelCount - 1);
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

void RationsEditorView::startDrag(Vst::ParamID id, float y)
{
    if (!mController)
        return;
    mDragParam = id;
    mDragStartY = y;
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
void RationsEditorView::onMouseDown(int x, int y, int button)
{
    if (button != 1) // left button only
        return;
    // Physical pixels in, logical units out: this is the only place the page transform is undone,
    // and everything below hit-tests against geometry.h directly.
    const float fx = static_cast<float>((x - mOffX) / mScale);
    const float fy = static_cast<float>((y - mOffY) / mScale);
    mMouseX = fx;
    mMouseY = fy;

    // The file browser captures input while open.
    if (mBrowser.isOpen()) {
        const FileBrowser::Result r = mBrowser.handleClick(fx, fy);
        if (r == FileBrowser::Result::Chosen && mController)
            mController->setIrFile(mBrowserSlot, mBrowser.chosenPath().c_str());
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
        case geo::Page::Pedalboard:
        case geo::Page::Settings:
            // Nothing else on either page is live yet; the settings rows arrive with the MIDI
            // phase and are drawn disabled until then.
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
            startDrag(k.id, y);
            invalidate(); // the readout appears
            return true;
        }
    }
    for (int i = 0; i < 2; ++i) {
        const geo::KnobSpec &k = geo::kIoKnobs[i];
        if (hitCircle(x, y, k.cx, k.cy, k.r)) {
            startDrag(k.id, y);
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
            if (i != activeChannel())
                editParam(kChannelId, i / static_cast<double>(kChannelCount - 1));
        } else {
            editParam(t.id, paramValue(t.id) > 0.5 ? 0.0 : 1.0);
        }
        invalidate();
        return true;
    }

    const Rect bypass(geo::kBypassToggle.cx - geo::kToggleHitW / 2.0f,
                      static_cast<float>(geo::kBypassToggle.cy + geo::kToggleHitTop),
                      geo::kToggleHitW,
                      static_cast<float>(geo::kToggleHitBottom - geo::kToggleHitTop));
    if (bypass.contains(x, y)) {
        editParam(kBypassId, paramValue(kBypassId) > 0.5 ? 0.0 : 1.0);
        invalidate();
        return true;
    }
    return false;
}

//------------------------------------------------------------------------
bool RationsEditorView::handleCabinetClick(float x, float y)
{
    if (blendActive() && hitCircle(x, y, geo::kBlendCX, geo::kBlendCY, geo::kBlendHitR)) {
        startDrag(kIrBlendId, y);
        invalidate();
        return true;
    }
    return handleIrRowClick(0, x, y) || handleIrRowClick(1, x, y);
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
    mMouseY = static_cast<float>((y - mOffY) / mScale);
    if (!mDragParam || !mController)
        return;
    const double norm = clampNorm(mDragStartNorm + (mDragStartY - mMouseY) / kKnobDragRange);
    mController->setParamNormalized(mDragParam, norm);
    mController->performEdit(mDragParam, norm);
    invalidate();
}

void RationsEditorView::onMouseUp(int /*x*/, int /*y*/, int button)
{
    if (button != 1)
        return;
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
    // The browser holds no geometry of its own — geometry.h stays the one authority on where
    // anything in this editor is drawn.
    mBrowser.setBounds(Rect(geo::kBrowserX, geo::kBrowserY, geo::kBrowserW, geo::kBrowserH));
    mBrowser.open(start, irRow(slot).ext,
                  slot == 0 ? "Select an impulse response" : "Select a second impulse response",
                  FileBrowser::Mode::File);
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
void RationsEditorView::onTick()
{
    pollCaps();

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
