// panelrender — render the editor's pages offline to PNGs, and audit the art.
//
// Two jobs, in this order:
//
//   1. AUDIT. Every asset the editor loads is opened here, and a missing or
//      unreadable one is a non-zero exit with the name printed. At run time all
//      of these degrade quietly to a flat-colour fallback (which is the correct
//      behaviour in a host — a broken install must not take the session down),
//      and quiet degradation is exactly why a build needs something that shouts.
//
//      The audit covers TEXT as well as files: every legend on every page is
//      measured against the space geometry.h gives it, and a legend that
//      outgrows its allowance is an error here rather than a collision on
//      screen. That is what makes the sizes in geometry.h's typography block
//      measured numbers instead of taste.
//
//   2. RENDER. Each page is drawn through the same Canvas, FontStack, PNG and
//      SVG code the editor uses, with no X11, no host and no VST3 objects, so
//      layout and art can be judged before any windowing work exists. Each page
//      has its own canvas size (geo::pageSize) — the editor asks the host to
//      resize the window on a page change — so the four PNGs are four different
//      shapes, exactly as the four windows are.
//
// Usage: panelrender <output-prefix> [resource-dir] [scale]
// Writes <prefix>-head.png, <prefix>-cabinet.png, <prefix>-pedalboard.png and
// <prefix>-settings.png.
// The resource directory defaults to the usual runtime resolution
// (RATIONS_RESOURCE_DIR, the bundle layout, or an executable-relative dir).
// `scale` renders at a non-default size, which is how the host-resize path's
// single cairo_scale is checked without a host.
//
// The drawing below is deliberately NOT shared with the editor's own view
// class: this rig has to keep working when that class is mid-rewrite, and a
// second independent reading of geometry.h is a check on the geometry rather
// than a copy of it.

#include "geometry.h"
#include "midilearn.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/svg.h"
#include "platform/respath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace Rations;

namespace
{

// Every raster layer the editor loads. Keep in step with gui/make_assets.sh and
// with the RATIONS_IMG_FILES list in CMakeLists.txt.
const char *const kRequiredImages[] = {
    "head",         "cabinet",       "dial",          "led_on",        "led_off",
    "switch_up_ring", "switch_down_ring", "meter_track",
    // The pedalboard. The five enclosures and the footswitch cap; the pedals'
    // knobs and LEDs are the very same dial/led_on/led_off named above, which is
    // why there are no pedal-specific entries for them.
    "pedal-boost", "pedal-chorus", "pedal-flanger", "pedal-delay", "pedal-reverb",
    "pedal_switch",
};
// Every icon the editor rasterises. Folder is the author's own; the rest are
// the original plug-in's, unmodified.
const char *const kRequiredIcons[] = {
    "Folder", "File", "Gear", "Cross", "ArrowLeft", "ArrowRight", "SlimmableIcon",
};

constexpr int kImageCount = static_cast<int>(sizeof(kRequiredImages) / sizeof(char *));
constexpr int kIconCount = static_cast<int>(sizeof(kRequiredIcons) / sizeof(char *));

//------------------------------------------------------------------------
// A knob: the dial bitmap rotated about its own centre. The art carries a gold
// pointer straight up at mid-travel, so nothing is drawn on top of it.
void drawKnob(Canvas &c, ImageCache &images, float cx, float cy, float r, double norm)
{
    const Rect face(cx - r, cy - r, 2.0f * r, 2.0f * r);
    if (cairo_surface_t *dial = images.get("dial")) {
        c.drawImageRotated(dial, face, (norm - 0.5) * geo::kKnobSweepDeg);
    } else {
        c.setColor(0x28282E);
        c.fillEllipse(face);
        c.setColor(geo::kGold);
        c.setPenSize(2.0f);
        const double a = (norm - 0.5) * geo::kKnobSweepDeg * 3.14159265358979323846 / 180.0;
        c.strokeLine(cx, cy, cx + std::sin(a) * r * 0.8, cy - std::cos(a) * r * 0.8);
    }
}

//------------------------------------------------------------------------
void drawCenteredText(Canvas &c, Font f, float size, uint32_t rgb, const char *text, float cx,
                      float baselineY)
{
    c.setFont(f);
    c.setFontSize(size);
    c.setColor(rgb);
    c.drawString(text, cx - c.stringWidth(text) * 0.5f, baselineY);
}

//------------------------------------------------------------------------
// A bat toggle. `on` is the parameter, not the bat: ToggleSpec::invert is what
// turns "bypass engaged" into a bat pointing DOWN.
void drawToggle(Canvas &c, ImageCache &images, const geo::ToggleSpec &t, bool on)
{
    const bool batUp = t.invert ? !on : on;
    const Rect dest(t.cx - t.w / 2.0f, t.cy - t.h / 2.0f, static_cast<float>(t.w),
                    static_cast<float>(t.h));
    if (cairo_surface_t *s = images.get(batUp ? "switch_up_ring" : "switch_down_ring")) {
        c.drawImage(s, dest);
    } else {
        // The lever is fixed at the CENTRE and the ball is the end that travels,
        // so a missing asset degrades to a shape that agrees with the art
        // instead of inverting it.
        const float cx = dest.centerX();
        const float cy = dest.centerY();
        const float ballR = dest.w * 0.26f;
        const float ballY = batUp ? dest.y + ballR + 2.0f : dest.bottom() - ballR - 2.0f;
        c.setColor(batUp ? 0xE8EAEC : 0x9A9EA2);
        c.fillRect(Rect(cx - 2.0f, std::min(cy, ballY), 4.0f, std::fabs(ballY - cy)));
        c.fillEllipse(cx, ballY, ballR, ballR);
    }

    // The legend is silkscreen: it names the switch, it does not report it.
    // Dimming it when off would read as "disabled", and for BYPASS that is
    // backwards — bypass off is the normal state. The bat carries the state.
    if (t.label)
        drawCenteredText(c, Font::Title, geo::kToggleLabelSize, geo::kTextColor, t.label, t.cx,
                         t.cy + geo::kToggleLabelDY);
}

//------------------------------------------------------------------------
void drawLed(Canvas &c, ImageCache &images, float cx, float cy, bool lit,
             float r = static_cast<float>(geo::kLedR))
{
    const Rect dest(cx - r, cy - r, 2.0f * r, 2.0f * r);
    if (cairo_surface_t *led = images.get(lit ? "led_on" : "led_off")) {
        c.drawImage(led, dest);
    } else {
        c.setColor(lit ? 0xE02020 : 0x141414);
        c.fillEllipse(dest);
    }
}

//------------------------------------------------------------------------
// The level meter. Green where the sibling single-capture plug-in is azure;
// everything else about the mechanism is the parent plug-in's, including the
// flat-rect fallback when the track art is missing.
void drawMeter(Canvas &c, ImageCache &images, const geo::MeterRect &m, float level, float peak)
{
    const Rect rect(m.x, m.y, m.w, m.h);
    if (cairo_surface_t *bg = images.get("meter_track")) {
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
void drawButton(Canvas &c, const geo::ButtonSpec &b)
{
    const Rect r(b.x, b.y, b.w, b.h);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);
    // Baseline from the button's centre rather than its bottom: the cap height
    // is about 0.72 em in Michroma, so half of that below the centre puts the
    // legend optically centred at any size.
    drawCenteredText(c, Font::Title, geo::kPageButtonTextSize, geo::kTextColor, b.label,
                     r.centerX(), r.centerY() + geo::kPageButtonTextSize * 0.36f);
}

//------------------------------------------------------------------------
// The IR loader row: an icon, prev/next arrows and a file name. This is the
// parent plug-in's row unchanged — same geometry constants, same dimmed alpha
// when nothing is loaded — so a user who only ever fills slot A sees exactly
// what they saw there.
void drawIrRow(Canvas &c, SvgCache &icons, const geo::FileRow &row, const char *text, bool loaded)
{
    const Rect r(row.x, row.y, row.w, row.h);
    c.setColor(0x0C0B0A);
    c.fillRoundRect(r, 4.0f);
    c.setColor(geo::kGold, 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, 4.0f);

    const int ih = row.h - 12;
    if (cairo_surface_t *s = icons.getByHeight("File", ih))
        c.drawImage(s, Rect(row.x + 8, row.y + 6, ih, ih));

    const float cy = row.y + row.h * 0.5f;
    const uint8_t alpha = loaded ? 255 : 90;
    if (cairo_surface_t *s = icons.getByHeight("ArrowLeft", geo::kIrArrowH))
        c.drawImageCentered(s, row.x + geo::kIrArrowPrevCX, cy, alpha);
    if (cairo_surface_t *s = icons.getByHeight("ArrowRight", geo::kIrArrowH))
        c.drawImageCentered(s, row.x + geo::kIrArrowNextCX, cy, alpha);

    const float tx = row.x + geo::kIrTextDX;
    c.setFont(Font::Body);
    c.setFontSize(geo::kFileRowTextSize);
    c.setColor(loaded ? geo::kTextColor : geo::kDimColor);
    c.drawString(c.clipToWidth(text, row.w - (tx - row.x) - 10).c_str(), tx, row.y + row.h - 10);
}

//------------------------------------------------------------------------
// The plug-in's dial readout, reproduced. In the editor the string comes back from
// getParamStringByValue, which is the SDK denormalizing into "%.1f" at the precision the
// parameter was given and the view appending KnobSpec::unit with one space. There is no
// controller here, so the same arithmetic is written out — and it has to be the SAME string,
// because what the audit below measures is whether the real readout fits its column.
std::string knobValueText(const geo::KnobSpec &k, double norm)
{
    double lo = 0.0, hi = 1.0;
    if (k.id == kNoiseGateThresholdId) {
        lo = ranges::kNgMin;
        hi = ranges::kNgMax;
    } else if (k.id == kBassId || k.id == kMiddleId || k.id == kTrebleId) {
        lo = ranges::kToneMin;
        hi = ranges::kToneMax;
    } else if (k.id == kInputGainId || k.id == kOutputGainId) {
        lo = ranges::kGainMin;
        hi = ranges::kGainMax;
    } else {
        return std::string(); // a channel dial names a capture, not a level
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f", lo + norm * (hi - lo));
    std::string out(buf);
    if (k.unit) {
        out += ' ';
        out += k.unit;
    }
    return out;
}

// The four dials that have a permanent value row under them, and the two I/O dials, drawn in the
// worst string each can show rather than in its demo position: what the audit is for is the
// column, and the column has to hold "-100.0 dB" whether or not the demo happens to.
const char *kWorstKnobValue[geo::kKnobCount] = {nullptr,     nullptr, nullptr, nullptr,
                                                "-100.0 dB", "10.0",  "10.0",  "10.0"};
const char *kWorstIoValue = "-40.0 dB";

void renderHead(Canvas &c, ImageCache &images, SvgCache &icons)
{
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());
    c.drawImage(images.get("head"), Rect(0, 0, geo::kWinW, geo::kWinH));

    // The wordmark: text in Michroma, centred on the faceplate by measurement.
    drawCenteredText(c, Font::Title, geo::kTitleSize, geo::kTextColor, "Rations", geo::kFaceCX,
                     geo::kTitleBaselineY);

    // Demo values, chosen so every control shows a distinct position rather
    // than a row of identical knobs. Crunch is the channel that is "on".
    const int activeChannel = 1;
    const double demo[geo::kKnobCount] = {0.30, 0.62, 0.45, 0.80, 0.25, 0.50, 0.55, 0.45};
    for (int i = 0; i < geo::kKnobCount; ++i) {
        drawKnob(c, images, static_cast<float>(geo::kKnobs[i].cx),
                 static_cast<float>(geo::kKnobs[i].cy), static_cast<float>(geo::kKnobs[i].r),
                 demo[i]);
        drawCenteredText(c, Font::Title, geo::kKnobLabelSize, geo::kTextColor, geo::kKnobs[i].label,
                         static_cast<float>(geo::kKnobs[i].cx),
                         static_cast<float>(geo::kKnobs[i].cy - geo::kKnobLabelDY));
        // The permanent value row, on the four dials that have one. Dim, because the editor draws
        // it dim until that dial is dragged, and it is the idle panel that is being audited.
        const std::string value = knobValueText(geo::kKnobs[i], demo[i]);
        if (!value.empty())
            drawCenteredText(c, Font::Body, geo::kKnobValueSize, geo::kDimColor, value.c_str(),
                             static_cast<float>(geo::kKnobs[i].cx),
                             static_cast<float>(geo::kKnobs[i].cy + geo::kKnobValueDY));
    }

    // Four LEDs and four bat switches, one per channel. Exactly one channel LED
    // is ever lit, which is the whole point of kChannelId being a list parameter
    // rather than four booleans. The gate's pair is up in the utility row.
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const bool on = (i == activeChannel);
        drawToggle(c, images, geo::kToggles[i], on);
        drawLed(c, images, static_cast<float>(geo::kToggles[i].cx), static_cast<float>(geo::kLedCY),
                on);
    }

    // The utility row — BYPASS, EQ and GATE with a lamp each, in the band left of the wordmark.
    // None of the three is in the mock; see geometry.h. Drawn in the state the panel is in when
    // nobody has touched it: bypass off, EQ and gate on, so all three lamps are lit.
    for (int i = 0; i < geo::kTopToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kTopToggles[i];
        const bool on = (t.id != kBypassId);
        drawToggle(c, images, t, on);
        drawLed(c, images, static_cast<float>(geo::kTopLedCX[i]),
                static_cast<float>(geo::kTopLedCY), t.invert ? !on : on,
                static_cast<float>(geo::kTopLedR));
    }

    drawMeter(c, images, geo::kInputMeter, 0.62f, 0.78f);
    drawMeter(c, images, geo::kOutputMeter, 0.48f, 0.0f);

    const double io[2] = {0.55, 0.50};
    for (int i = 0; i < 2; ++i) {
        drawKnob(c, images, static_cast<float>(geo::kIoKnobs[i].cx),
                 static_cast<float>(geo::kIoKnobs[i].cy), static_cast<float>(geo::kIoKnobs[i].r),
                 io[i]);
        drawCenteredText(c, Font::Title, geo::kIoLabelSize, geo::kTextColor, geo::kIoKnobs[i].label,
                         static_cast<float>(geo::kIoKnobs[i].cx),
                         static_cast<float>(geo::kIoLabelBaselineY));
        const std::string value = knobValueText(geo::kIoKnobs[i], io[i]);
        drawCenteredText(c, Font::Body, geo::kKnobValueSize, geo::kDimColor, value.c_str(),
                         static_cast<float>(geo::kIoKnobs[i].cx),
                         static_cast<float>(geo::kIoValueBaselineY));
    }

    for (const geo::ButtonSpec &b : geo::kPageButtons)
        drawButton(c, b);

    if (cairo_surface_t *slim = icons.getByHeight("SlimmableIcon", geo::kSlimIconH))
        c.drawImage(slim, Rect(geo::kSlimIconCX - geo::kSlimIconW / 2.0f,
                               geo::kSlimIconCY - geo::kSlimIconH / 2.0f, geo::kSlimIconW,
                               geo::kSlimIconH));
    if (cairo_surface_t *gear = icons.getByHeight("Gear", 2 * geo::kGearR))
        c.drawImage(gear, Rect(geo::kGearCX - geo::kGearR, geo::kGearCY - geo::kGearR,
                               2 * geo::kGearR, 2 * geo::kGearR));
}

//------------------------------------------------------------------------
void renderCabinet(Canvas &c, ImageCache &images, SvgCache &icons)
{
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());
    c.drawImage(images.get("cabinet"), Rect(geo::kCabX, geo::kCabY, geo::kCabW, geo::kCabH));

    // Slot A loaded, slot B empty: the normal case, and the one the design has
    // to get right. With one IR the Blend dial does nothing, so it is drawn
    // disabled rather than left looking live.
    const bool bothLoaded = false;
    drawKnob(c, images, static_cast<float>(geo::kBlendCX), static_cast<float>(geo::kBlendCY),
             static_cast<float>(geo::kBlendR), 0.0);
    drawCenteredText(
        c, Font::Title, geo::kBlendLabelSize, bothLoaded ? geo::kTextColor : geo::kDimColor,
        "Blend", static_cast<float>(geo::kBlendCX), static_cast<float>(geo::kBlendLabelBaselineY));

    drawIrRow(c, icons, geo::kIrRowA, "Marshall 1960A - SM57 cap edge.wav", true);
    drawIrRow(c, icons, geo::kIrRowB, geo::kIrRowB.placeholder, false);

    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
// The pedalboard's lettering - plain white, mirroring RationsEditorView::drawPedalString.
void drawPedalString(Canvas &c, const char *text, float x, float y)
{
    c.setColor(geo::kPedalInk);
    c.drawString(text, x, y);
}

// One pedal's face, generated from its slice of kPedalParams exactly as the
// editor generates it. Nothing here is per-pedal. The silkscreen is white on a
// plain white, it does not dim - the LED carries the state - and a live control
// inverts into a filled white plate rather than changing colour; all three rules
// are stated at RationsEditorView::drawPedal.
void drawPedalFace(Canvas &c, ImageCache &images, int pedal, bool on, unsigned liveMini)
{
    const geo::PedalSpec &p = geo::kPedals[pedal];

    for (int k = 0, nk = pedalKnobCount(pedal); k < nk; ++k) {
        const PedalParamSpec &spec = kPedalParams[pedalKnobParam(pedal, k)];
        const geo::PedalPoint pt = geo::pedalKnobCenter(pedal, k);
        const float cx = static_cast<float>(pt.x);
        const float cy = static_cast<float>(pt.y);
        drawKnob(c, images, cx, cy, static_cast<float>(geo::kPedalKnobR),
                 pedalNorm(spec, spec.def));
        c.setFont(Font::Title);
        c.setFontSize(static_cast<float>(geo::kPedalLabelSize));
        const std::string fit =
            c.clipToWidth(spec.legend, static_cast<float>(geo::pedalKnobLabelAllowance(pedal, k)));
        drawPedalString(c, fit.c_str(), cx - c.stringWidth(fit.c_str()) * 0.5f,
                         cy + geo::kPedalKnobR + geo::kPedalLabelDY);
    }

    for (int m = 0, nm = pedalMiniCount(pedal); m < nm; ++m) {
        const PedalParamSpec &spec = kPedalParams[pedalMiniParam(pedal, m)];
        const geo::PedalPoint pt = geo::pedalMiniCenter(pedal, m);
        const Rect box(static_cast<float>(pt.x - geo::kPedalMiniW / 2),
                       static_cast<float>(pt.y - geo::kPedalMiniH / 2),
                       static_cast<float>(geo::kPedalMiniW), static_cast<float>(geo::kPedalMiniH));
        // The editor asks the controller for a list's text; there is no controller
        // here, so the one list that exists is read straight out of the table its
        // StringListParameter is populated from. If a second list is ever added this
        // needs a real lookup, and the text audit below is what will say so.
        const bool isList = spec.kind == PedalParamKind::List;
        const char *text = isList ? kDelaySyncNames[kDelaySyncCount - 1] : spec.legend;
        const bool live = ((liveMini >> m) & 1u) != 0u;
        c.setColor(geo::kPedalInk);
        if (live) {
            c.fillRoundRect(box, static_cast<float>(geo::kPedalMiniRadius));
        } else {
            c.setPenSize(1.0f);
            c.strokeRoundRect(box, static_cast<float>(geo::kPedalMiniRadius));
        }
        c.setFont(Font::Title);
        c.setFontSize(static_cast<float>(geo::kPedalMiniSize));
        const std::string fit = c.clipToWidth(text, static_cast<float>(geo::kPedalMiniW - 6));
        const float tx = box.centerX() - c.stringWidth(fit.c_str()) * 0.5f;
        const float ty = box.centerY() + geo::kPedalMiniSize * 0.36f;
        if (live) {
            c.setColor(geo::kPedalInkPlate);
            c.drawString(fit.c_str(), tx, ty);
        } else {
            drawPedalString(c, fit.c_str(), tx, ty);
        }
    }

    const geo::PedalPoint led = geo::pedalLedCenter(pedal);
    drawLed(c, images, static_cast<float>(led.x), static_cast<float>(led.y), on,
            static_cast<float>(geo::kPedalLedR));

    const geo::PedalPoint sw = geo::pedalSwitchCenter(pedal);
    const float r = static_cast<float>(geo::kPedalSwitchR);
    if (cairo_surface_t *cap = images.get("pedal_switch")) {
        c.drawImage(cap, Rect(sw.x - r, sw.y - r, 2.0f * r, 2.0f * r));
    } else {
        c.setColor(0x6E7276);
        c.fillEllipse(static_cast<float>(sw.x), static_cast<float>(sw.y), r, r);
    }

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
void renderPedalboard(Canvas &c, ImageCache &images, SvgCache &)
{
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());

    // Deliberately a copy of RationsEditorView::drawPedalboardStatic rather than a
    // call to it: this tool links the graphics stack, not the plug-in, which is
    // what lets it audit the art without a host. The two are kept in step by the
    // asset and text audits below failing when they drift.
    for (int i = 0; i < geo::kPedalCount; ++i) {
        const geo::PedalSpec &p = geo::kPedals[i];
        c.drawImage(images.get(p.art),
                    Rect(static_cast<float>(geo::kPedalLeft(i)), static_cast<float>(p.y),
                         static_cast<float>(geo::kPedalW), static_cast<float>(geo::kPedalH)));
    }

    c.setColor(geo::kDimColor);
    c.setPenSize(geo::kPedalCablePen);
    for (int i = 0; i + 1 < geo::kPedalCount; ++i) {
        if (geo::kPedals[i].post != geo::kPedals[i + 1].post)
            continue;
        const float x0 = static_cast<float>(geo::kPedalLeft(i) + geo::kPedalW);
        const float x1 = static_cast<float>(geo::kPedalLeft(i + 1));
        const float y = static_cast<float>(geo::kPedals[i].y + geo::kPedalJackY);
        const float d = (x1 - x0) * 0.35f;
        c.strokeBezier(x0, y, x0 + d, y + geo::kPedalCableSag, x1 - d, y + geo::kPedalCableSag, x1,
                       y);
    }

    c.setFont(Font::Title);
    c.setFontSize(geo::kPedalRowLegendSize);
    c.setColor(geo::kDimColor);
    c.drawString("PRE", static_cast<float>(geo::kPedalRowLegendX),
                 static_cast<float>(geo::kPedalRow1Y - geo::kPedalRowLegendDY));
    c.drawString("POST", static_cast<float>(geo::kPedalRowLegendX),
                 static_cast<float>(geo::kPedalRow2Y - geo::kPedalRowLegendDY));

    // The faces. Deliberately a copy of RationsEditorView::drawPedal for the same
    // reason the enclosures above are: this tool links the graphics stack, not
    // the plug-in. The two are kept in step by the text audit below, which
    // measures the very strings this draws against the very allowances it clips
    // to, and fails when either moves.
    //
    // Fixture values are the awkward ones rather than the tidy ones: the on/off
    // states alternate so both are on screen at once, the Delay's Sync sits on
    // the WIDEST of its twelve divisions rather than on "Free", and its
    // Ping-Pong is lit — so the page shows the widest thing each control can
    // ever be asked to draw, which is the only state worth auditing.
    static const bool kOn[geo::kPedalCount] = {true, false, true, false, true};
    for (int i = 0; i < geo::kPedalCount; ++i)
        // Bit 0 of the mask is the first mini slot: the Delay's Sync draws FILLED and its
        // Ping-Pong OUTLINED, so both halves of "filled is live, outlined is idle" are on
        // screen in the one place either of them is ever drawn.
        drawPedalFace(c, images, i, kOn[i], 0x1u);

    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
// The settings page: the channel trims, then MIDI learn. The values drawn here
// are stand-ins - this tool has no processor to ask - chosen so that every state
// each kind of row can be in is on screen at once, since each is a different
// width and a different set of controls. For the trims that means both extremes,
// the default, and a value in between; for MIDI it means learned, listening and
// unlearned.
void renderSettings(Canvas &c, ImageCache &images, SvgCache &svgs)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Settings));
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());

    // --- the capture loaders -----------------------------------------------------------------
    // Fixture values chosen to be the awkward ones rather than the tidy ones: a folder with a long
    // name, a single capture, a user-typed override that is far wider than the field, and an empty
    // channel. If a row can survive those four it can survive a user.
    drawCenteredText(c, Font::Title, geo::kSettingsHeadingSize, geo::kTextColor,
                     geo::kCaptureHeading, cx, static_cast<float>(geo::kCaptureHeadingY));
    {
        struct CaptureFixture {
            const char *name;
            const char *text;
            bool loaded;
        };
        const CaptureFixture kRows[geo::kCaptureRowCount] = {
            {"JCM800", "JCM800  (12 captures)", true},
            {"Deluxe Reverb", "BF Deluxe 1965 vol6.nam  (single capture)", true},
            {"A Ridiculously Long Channel Name", "Marshall Silver Jubilee  (9 captures)", true},
            {"OD2", geo::kCapturePlaceholder, false},
        };
        for (int i = 0; i < geo::kCaptureRowCount; ++i) {
            const Rect r(geo::kMidiRowX, geo::kCaptureRowY0 + i * geo::kMidiRowPitch,
                         geo::kMidiRowW, geo::kMidiRowH);
            c.setColor(0x0C0B0A);
            c.fillRoundRect(r, 4.0f);
            c.setColor(geo::kGold, 190);
            c.setPenSize(1.0f);
            c.strokeRoundRect(r, 4.0f);

            const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
            // A folder or a file, the same way the row itself decides it — the icon is part of
            // what the row has to fit, so the audit draws it rather than assuming it is free.
            if (cairo_surface_t *icon =
                    svgs.getByHeight(kRows[i].loaded ? "Folder" : "File", geo::kRowIconH))
                c.drawImageCentered(icon, r.x + 8 + geo::kRowIconH * 0.5f, r.centerY());

            c.setFont(Font::Title);
            c.setFontSize(geo::kMidiRowTextSize);
            c.setColor(geo::kTextColor);
            c.drawString(c.clipToWidth(kRows[i].name, geo::kCaptureNameW).c_str(),
                         r.x + geo::kCaptureNameX, base);

            c.setFont(Font::Body);
            c.setFontSize(geo::kFileRowTextSize);
            c.setColor(kRows[i].loaded ? geo::kTextColor : geo::kDimColor);
            c.drawString(c.clipToWidth(kRows[i].text, geo::kCaptureTextW).c_str(),
                         r.x + geo::kCaptureTextX, base);

            if (kRows[i].loaded) {
                const Rect clear(r.right() - geo::kCaptureClearInset - geo::kCaptureClearW,
                                 r.y + geo::kCaptureClearInset, geo::kCaptureClearW,
                                 geo::kMidiRowH - 2 * geo::kCaptureClearInset);
                c.setColor(geo::kDimColor);
                c.setPenSize(1.5f);
                const Rect x = clear.inset(6.0f);
                c.strokeLine(x.left(), x.top(), x.right(), x.bottom());
                c.strokeLine(x.left(), x.bottom(), x.right(), x.top());
                c.setPenSize(1.0f);
            }
        }
    }
    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor,
                     geo::kCaptureFootnote, cx, static_cast<float>(geo::kCaptureFootnoteY));

    drawCenteredText(c, Font::Title, geo::kSettingsHeadingSize, geo::kTextColor, geo::kLevelHeading,
                     cx, static_cast<float>(geo::kLevelHeadingY));

    // Both ends of the range, the default, and one off it: the readout is widest
    // at the negative extreme and the thumb is at a different edge in each.
    const double kLevels[geo::kLevelRowCount] = {0.5, 0.0, 1.0, 0.68};
    for (int i = 0; i < geo::kLevelRowCount; ++i) {
        const Rect r(geo::kMidiRowX, geo::kLevelRowY0 + i * geo::kMidiRowPitch, geo::kMidiRowW,
                     geo::kMidiRowH);
        c.setColor(0x0C0B0A);
        c.fillRoundRect(r, 4.0f);
        c.setColor(geo::kGold, 190);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, 4.0f);

        const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(kMidiLearnRows[i].label, r.x + 12.0f, base);

        const Rect slider(r.x + geo::kLevelSliderX, r.centerY() - geo::kLevelThumbH * 0.5f,
                          geo::kLevelSliderW, geo::kLevelThumbH);
        const float trackY = slider.centerY() - geo::kLevelTrackH * 0.5f;
        const Rect track(slider.x, trackY, slider.w, geo::kLevelTrackH);
        c.setColor(0x000000, 170);
        c.fillRoundRect(track, geo::kLevelTrackH * 0.5f);
        c.setColor(geo::kGold, 90);
        c.setPenSize(1.0f);
        c.strokeRoundRect(track, geo::kLevelTrackH * 0.5f);

        const float travelX = slider.x + geo::kLevelThumbW * 0.5f;
        const float centreX = travelX + geo::kLevelTravel * 0.5f;
        const float thumbX = travelX + geo::kLevelTravel * static_cast<float>(kLevels[i]);
        if (std::fabs(thumbX - centreX) > 1.0f) {
            c.setColor(geo::kGold, 170);
            c.fillRect(Rect(std::min(centreX, thumbX), trackY, std::fabs(thumbX - centreX),
                            geo::kLevelTrackH));
        }
        c.setColor(geo::kGold, 200);
        c.setPenSize(1.0f);
        c.strokeLine(centreX, slider.centerY() - geo::kLevelCentreTickH * 0.5f, centreX,
                     slider.centerY() + geo::kLevelCentreTickH * 0.5f);

        const Rect thumb(thumbX - geo::kLevelThumbW * 0.5f, slider.y, geo::kLevelThumbW,
                         geo::kLevelThumbH);
        c.setColor(0x2A2724);
        c.fillRoundRect(thumb, 3.0f);
        c.setColor(geo::kGold, 230);
        c.setPenSize(1.0f);
        c.strokeRoundRect(thumb, 3.0f);

        const double db = ranges::kLevelMin + kLevels[i] * (ranges::kLevelMax - ranges::kLevelMin);
        char text[24];
        snprintf(text, sizeof(text), "%+.1f dB", db);
        c.setFont(Font::Body);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(std::fabs(db) < 0.05 ? geo::kDimColor : geo::kTextColor);
        c.drawString(text, r.right() - geo::kLevelReadoutInset - c.stringWidth(text), base);
    }

    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor, geo::kLevelFootnote,
                     cx, static_cast<float>(geo::kLevelFootnoteY));

    drawCenteredText(c, Font::Title, geo::kSettingsHeadingSize, geo::kTextColor, geo::kMidiHeading,
                     cx, static_cast<float>(geo::kSettingsHeadingY));

    // The widest each kind of binding gets, not a typical one: this page is a
    // measurement, so what it draws is the worst case the row has to hold.
    const MidiBinding kShown[geo::kMidiRowCount] = {
        {MidiMsg::ControlChange, kMidiAnyChannel, 127},
        {MidiMsg::NoteOn, 15, 1}, // "Note C#-2 ch 16"
        {MidiMsg::ProgramChange, kMidiAnyChannel, 127},
        {MidiMsg::Unlearned, kMidiAnyChannel, 0},
        // The five pedal rows, which are the same row drawn against the widest pedal names. Two
        // are left unlearned, because a half-mapped board is the ordinary state of one.
        {MidiMsg::ControlChange, kMidiAnyChannel, 80},
        {MidiMsg::NoteOn, 0, 127}, // "Note G8 ch 1"
        {MidiMsg::ProgramChange, kMidiAnyChannel, 5},
        {MidiMsg::Unlearned, kMidiAnyChannel, 0},
        {MidiMsg::Unlearned, kMidiAnyChannel, 0},
    };
    const int kArmed = 2; // one row shown listening, so that state is on screen too

    for (int i = 0; i < geo::kMidiRowCount; ++i) {
        const Rect r(geo::kMidiRowX, geo::midiRowY(i), geo::kMidiRowW, geo::kMidiRowH);
        c.setColor(0x0C0B0A);
        c.fillRoundRect(r, 4.0f);
        c.setColor(geo::kGold, 190);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, 4.0f);

        const float base = r.centerY() + geo::kMidiRowTextSize * 0.36f;
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(kMidiLearnRows[i].label, r.x + 12.0f, base);

        // The mapping itself is data, not a legend, so it is the body face. An unlearned row
        // draws nothing: the Learn button beside it already says what the row is for.
        if (i == kArmed || kShown[i].learned()) {
            const std::string text =
                (i == kArmed) ? std::string(geo::kMidiListeningText) : describeBinding(kShown[i]);
            c.setFont(Font::Body);
            c.setColor(i == kArmed ? geo::kDimColor : geo::kTextColor);
            c.drawString(text.c_str(), r.x + static_cast<float>(geo::kMidiTextX), base);
        }

        const int learnX = static_cast<int>(r.right()) - geo::kMidiLearnInset - geo::kMidiLearnW;
        const int by = static_cast<int>(r.y) + geo::kMidiLearnInset;
        const int bh = geo::kMidiRowH - 2 * geo::kMidiLearnInset;
        if (kShown[i].learned()) {
            const geo::ButtonSpec clear = {learnX - geo::kMidiButtonGap - geo::kMidiClearW,
                                           by,
                                           geo::kMidiClearW,
                                           bh,
                                           geo::kMidiClearLabel,
                                           geo::Page::Settings};
            drawButton(c, clear);
        }
        const geo::ButtonSpec learn = {learnX,
                                       by,
                                       geo::kMidiLearnW,
                                       bh,
                                       i == kArmed ? geo::kMidiListenLabel : geo::kMidiLearnLabel,
                                       geo::Page::Settings};
        drawButton(c, learn);
    }

    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor,
                     geo::kSettingsFootnote, cx, static_cast<float>(geo::kSettingsFootnoteY));
    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor,
                     geo::kSettingsFootnote2, cx, static_cast<float>(geo::kSettingsFootnote2Y));
    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor,
                     geo::kSettingsFootnote3, cx, static_cast<float>(geo::kSettingsFootnote3Y));

    // --- the output section ------------------------------------------------------------------
    // Drawn with Calibrated GATED and the calibration block live, which is the mixed state: a
    // capture set that states an input level but no output level. Rendering it all-available would
    // never show what a greyed row looks like, and that row carries the longest string on the page.
    drawCenteredText(c, Font::Title, geo::kSettingsHeadingSize, geo::kTextColor,
                     geo::kOutputHeading, cx, static_cast<float>(geo::kOutputHeadingY));
    {
        constexpr int kCurrent = kOutputNormalized;
        for (int i = 0; i < kOutputModeCount; ++i) {
            const Rect row(geo::kMidiRowX, geo::kOutputRowY0 + i * geo::kOutputRowPitch,
                           geo::kOutputRowW, geo::kOutputRowH);
            const bool gated = i == kOutputCalibrated;
            const float dotCX = row.x + geo::kOutputDotCX;
            const float dotCY = row.centerY();

            c.setColor(gated ? 0x4A4740 : geo::kAccent);
            c.setPenSize(1.5f);
            c.strokeEllipse(dotCX, dotCY, geo::kOutputDotR, geo::kOutputDotR);
            if (i == kCurrent) {
                c.setColor(geo::kAccentBright);
                c.fillEllipse(dotCX, dotCY, geo::kOutputDotFillR, geo::kOutputDotFillR);
            }
            c.setPenSize(1.0f);

            std::string label(geo::kOutputModeNames[i]);
            if (gated)
                label += geo::kOutputUnsupported;
            c.setFont(Font::Title);
            c.setFontSize(geo::kMidiRowTextSize);
            c.setColor(gated ? 0x6A6460 : geo::kTextColor);
            c.drawString(label.c_str(), row.x + geo::kOutputTextX,
                         dotCY + geo::kMidiRowTextSize * 0.36f);
        }

        // The same bat the faceplate uses, through the same helper: a second way of drawing a
        // switch would be a second thing to keep looking like the first.
        constexpr geo::ToggleSpec kCalToggle = {kCalibrateInputId, geo::kCalToggleCX,
                                                geo::kCalToggleCY, geo::kToggleW,
                                                geo::kToggleH,     nullptr,
                                                false};
        drawToggle(c, images, kCalToggle, /*on=*/true);
        const Rect tog(geo::kCalToggleCX - geo::kToggleW * 0.5f,
                       geo::kCalToggleCY - geo::kToggleH * 0.5f, geo::kToggleW, geo::kToggleH);
        c.setFont(Font::Title);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(geo::kCalibrateLabel, static_cast<float>(geo::kCalLabelX),
                     tog.centerY() + geo::kMidiRowTextSize * 0.36f);

        const Rect value(geo::kCalValueX, geo::kCalValueY, geo::kCalValueW, geo::kCalValueH);
        c.setColor(0x000000, 170);
        c.fillRoundRect(value, 4.0f);
        c.setColor(geo::kGold, 190);
        c.setPenSize(1.0f);
        c.strokeRoundRect(value, 4.0f);
        char dbu[24];
        snprintf(dbu, sizeof(dbu), "%+.1f dBu", ranges::kCalDefault);
        c.setFont(Font::Body);
        c.setFontSize(geo::kMidiRowTextSize);
        c.setColor(geo::kTextColor);
        c.drawString(dbu, value.centerX() - c.stringWidth(dbu) * 0.5f,
                     value.centerY() + geo::kMidiRowTextSize * 0.36f);
    }
    drawCenteredText(c, Font::Body, geo::kSettingsFootnoteSize, geo::kDimColor,
                     geo::kOutputFootnote, cx, static_cast<float>(geo::kOutputFootnoteY));

    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
// One legend, and the width geometry.h leaves it. Measured rather than trusted:
// see the typography block in geometry.h.
struct TextFit {
    const char *where;
    Font font;
    float size;
    const char *text;
    float allowance;
};

//------------------------------------------------------------------------
// Cap evenness. Baseline is fixed, so the first SOLIDLY inked raster row of a
// glyph is its apparent cap height, and a row of text whose caps and digits do
// not agree on that row has some letters visibly short — which is what a short
// D in "OD2" is. It is a property of the SIZE, not of the string, so the check
// runs once per Michroma size over the caps and digits that size actually
// draws: see the typography note in geometry.h for the measurement behind it.
//
// Restricted to the characters in use rather than the whole alphabet, because
// the two are different questions. "Rations" carries exactly one capital, so
// there is nothing for its R to look short against, and failing the wordmark
// over letters it does not contain would be a false alarm that pushes a size
// around for no visible reason.
//
// Michroma only. Font::Body carries file names, a drag readout and two lines of
// prose — variable mixed-case text whose size is set by the row it sits in, and
// where a one-pixel cap difference inside a lowercase word is neither visible
// nor avoidable.
int capStep(FontStack &fonts, float size, const std::string &caps)
{
    const int box = static_cast<int>(size * 3.0f) + 8;
    const float baseline = size * 2.0f;
    int lo = box, hi = -1;
    for (char g : caps) {
        const char one[2] = {g, '\0'};
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, box, box);
        cairo_t *cr = cairo_create(s);
        {
            // Through Canvas, so the glyphs are rasterised with exactly the font
            // options the editor draws with.
            Canvas c(cr, &fonts, static_cast<float>(box), static_cast<float>(box));
            c.setColor(0x000000);
            c.fillRect(c.bounds());
            c.setFont(Font::Title);
            c.setFontSize(size);
            c.setColor(0xFFFFFF);
            c.drawString(one, 4.0f, baseline);
        }
        cairo_destroy(cr);
        cairo_surface_flush(s);
        const unsigned char *d = cairo_image_surface_get_data(s);
        const int stride = cairo_image_surface_get_stride(s);
        for (int y = 0; y < box; ++y) {
            int strongest = 0;
            for (int x = 0; x < box; ++x)
                strongest = std::max(strongest, static_cast<int>(d[y * stride + x * 4 + 1]));
            if (strongest > 128) { // more than half covered: real ink, not a fringe
                lo = std::min(lo, y);
                hi = std::max(hi, y);
                break;
            }
        }
        cairo_surface_destroy(s);
    }
    return (hi < 0) ? 0 : hi - lo;
}

//------------------------------------------------------------------------
// The page buttons share the bottom row with the five bat switches, and a bat
// switch's CLICK target is much wider than its art (see kToggleHitW), so a
// button that looks clear of one on screen can still be stealing its clicks.
// The mock's own positions were 1 px inside the Gate switch's hit box.
bool auditHitBoxes()
{
    struct Box {
        const char *what;
        float l, t, r, b;
    };
    std::vector<Box> boxes;
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const geo::ToggleSpec &t = geo::kToggles[i];
        boxes.push_back({geo::kKnobs[i].label, t.cx - geo::kToggleHitW / 2.0f,
                         static_cast<float>(t.cy + geo::kToggleHitTop),
                         t.cx + geo::kToggleHitW / 2.0f,
                         static_cast<float>(t.cy + geo::kToggleHitBottom)});
    }
    for (const geo::ButtonSpec &b : geo::kPageButtons)
        boxes.push_back({b.label, static_cast<float>(b.x), static_cast<float>(b.y),
                         static_cast<float>(b.x + b.w), static_cast<float>(b.y + b.h)});

    // The upper band: the two utility switches and the gear. Not the same row as the five bat
    // switches, but on the same faceplate and hit-tested from the same click, so they belong in
    // the same check — and BYPASS in particular has been moved once already, and EQ was fitted
    // into the space beside it with nothing to spare.
    for (const geo::ToggleSpec &t : geo::kTopToggles)
        boxes.push_back({t.label, t.cx - geo::kTopToggleHitW / 2.0f,
                         static_cast<float>(t.cy + geo::kTopToggleHitTop),
                         t.cx + geo::kTopToggleHitW / 2.0f,
                         static_cast<float>(t.cy + geo::kTopToggleHitBottom)});
    // Slim's target, with the editor's own 4 px slop. It is the newest thing in that corner and
    // the one with a real neighbour, so it is exactly the kind of box this audit exists for.
    boxes.push_back({"slim icon", geo::kSlimIconCX - geo::kSlimIconW / 2.0f - 4,
                     geo::kSlimIconCY - geo::kSlimIconH / 2.0f - 4,
                     geo::kSlimIconCX + geo::kSlimIconW / 2.0f + 4,
                     geo::kSlimIconCY + geo::kSlimIconH / 2.0f + 4});
    // The gear's click target is its radius plus the same 4 px slop the editor allows.
    boxes.push_back({"gear", static_cast<float>(geo::kGearCX - geo::kGearR - 4),
                     static_cast<float>(geo::kGearCY - geo::kGearR - 4),
                     static_cast<float>(geo::kGearCX + geo::kGearR + 4),
                     static_cast<float>(geo::kGearCY + geo::kGearR + 4)});
    // The two level meters are not clickable, but a control drawn on top of one is a control
    // drawn on top of the thing it is supposed to sit beside, so they are in the check as
    // obstacles.
    for (const geo::MeterRect *m : {&geo::kInputMeter, &geo::kOutputMeter})
        boxes.push_back({m == &geo::kInputMeter ? "input meter" : "output meter",
                         static_cast<float>(m->x), static_cast<float>(m->y),
                         static_cast<float>(m->x + m->w), static_cast<float>(m->y + m->h)});

    int hits = 0;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const Box &a = boxes[i], &c = boxes[j];
            if (a.l < c.r && c.l < a.r && a.t < c.b && c.t < a.b) {
                fprintf(stderr,
                        "panelrender: the \"%s\" and \"%s\" hit boxes overlap "
                        "(%.0f..%.0f vs %.0f..%.0f horizontally) — one of them will swallow the "
                        "other's clicks\n",
                        a.what, c.what, a.l, a.r, c.l, c.r);
                ++hits;
            }
        }
    // The bypass lamp is placed as the gear's reflection through the faceplate centre, which is
    // what makes it sit the same distance from the input meter as the gear does from the output
    // meter. That is a relationship, not a coincidence, so it is checked rather than trusted: the
    // two meter columns are themselves symmetric about that centre, so if this ever fails it is
    // because someone moved a column and not because the arithmetic drifted.
    const int mirrored = 2 * geo::kFaceCX - geo::kGearCX;
    if (geo::kBypassLedCX != mirrored) {
        fprintf(stderr,
                "panelrender: the bypass lamp is at x=%d but the gear's mirror is x=%d — the "
                "lamp no longer sits off the input meter the way the gear sits off the output "
                "meter\n",
                geo::kBypassLedCX, mirrored);
        ++hits;
    }

    if (hits == 0)
        printf("hit boxes  %zu targets on the faceplate, none overlapping; bypass lamp mirrors "
               "the gear\n",
               boxes.size());
    return hits == 0;
}

//------------------------------------------------------------------------
// VERTICAL CLEARANCE on a pedal face, measured with the real glyph ink.
//
// This is the audit that was missing, and the defect it exists for was found by a human looking at
// the page rather than by anything in this file: on the four-knob faces the upper row's legends
// touched the tops of the lower row's knobs. The arithmetic was exactly flush - a label baseline
// sits at knob edge + kPedalLabelDY, and 44 + 20 + 12 is precisely 96 - 20 - and then "Depth",
// "Repeats" and "Decay" hang a descender into the knob below.
//
// So the check is done with cairo's INK extents for the strings that are actually drawn, not with
// the font's nominal descent and not with a constant in geometry.h. "Tone" has nothing under its
// baseline and "Depth" does, and a row's clearance is set by whichever of its labels is worst.
//
// It is also done by COLUMN rather than by row: the three-knob faces are a triangle, so their
// upper labels sit beside the lower dial and not above it, and a check that compared rows would
// fail a layout that is correct. Two things collide only if their horizontal spans overlap.
bool auditPedalClearance(FontStack &fonts)
{
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(scratch);
    Canvas c(cr, &fonts, 8, 8);

    struct Span {
        std::string what;
        float l, r, top;
    };
    int bad = 0;
    float worst = 1e9f;
    std::string worstWhat;

    for (int i = 0; i < geo::kPedalCount; ++i) {
        // Everything on the face that a label could fall on to, as a horizontal span and the y its
        // ink begins at. In pedal-local coordinates.
        std::vector<Span> below;
        for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
            const geo::PedalPoint pt = geo::pedalKnobPos(nk, k);
            below.push_back({std::string(kPedalParams[pedalKnobParam(i, k)].legend) + " knob",
                             static_cast<float>(pt.x - geo::kPedalKnobR),
                             static_cast<float>(pt.x + geo::kPedalKnobR),
                             static_cast<float>(pt.y - geo::kPedalKnobR)});
        }
        for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m)
            below.push_back({std::string(kPedalParams[pedalMiniParam(i, m)].legend) + " box",
                             static_cast<float>(geo::kPedalMiniCX[m] - geo::kPedalMiniW / 2),
                             static_cast<float>(geo::kPedalMiniCX[m] + geo::kPedalMiniW / 2),
                             static_cast<float>(geo::kPedalMiniY - geo::kPedalMiniH / 2)});
        below.push_back({"LED", static_cast<float>(geo::kPedalKnobCX - geo::kPedalLedR),
                         static_cast<float>(geo::kPedalKnobCX + geo::kPedalLedR),
                         static_cast<float>(geo::kPedalLedY - geo::kPedalLedR)});
        below.push_back({"footswitch", static_cast<float>(geo::kPedalKnobCX - geo::kPedalSwitchR),
                         static_cast<float>(geo::kPedalKnobCX + geo::kPedalSwitchR),
                         static_cast<float>(geo::kPedalSwitchY - geo::kPedalSwitchR)});

        for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
            const PedalParamSpec &spec = kPedalParams[pedalKnobParam(i, k)];
            const geo::PedalPoint pt = geo::pedalKnobPos(nk, k);
            c.setFont(Font::Title);
            c.setFontSize(static_cast<float>(geo::kPedalLabelSize));
            const float w = c.stringWidth(spec.legend);
            const float l = pt.x - w * 0.5f;
            const float r = pt.x + w * 0.5f;
            const float inkBottom = pt.y + geo::kPedalKnobR + geo::kPedalLabelDY +
                                    c.stringDescent(spec.legend);
            for (const Span &o : below) {
                // "Below" is judged against the KNOB'S CENTRE, not against the label's ink.
                // The first version of this compared with the ink bottom, which meant an
                // obstacle the label had ALREADY run into counted as being above it and was
                // skipped - so the audit silently passed the exact defect it was written for.
                // Clearance is allowed to come out negative; that is the interesting case.
                if (o.top <= static_cast<float>(pt.y) || r <= o.l || o.r <= l)
                    continue; // above this knob, or in a different column
                const float clear = o.top - inkBottom;
                if (clear < geo::kPedalLabelClearance) {
                    fprintf(stderr,
                            "panelrender: on the %s, the \"%s\" legend's ink ends at y %.1f and "
                            "the %s begins at %.1f — %.1f units of clearance, %d wanted. Move the "
                            "row in geometry.h\n",
                            geo::kPedals[i].name, spec.legend, inkBottom, o.what.c_str(), o.top,
                            clear, geo::kPedalLabelClearance);
                    ++bad;
                }
                if (clear < worst) {
                    worst = clear;
                    worstWhat = std::string(geo::kPedals[i].name) + " " + spec.legend + " over " +
                                o.what;
                }
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);
    if (bad == 0)
        printf("pedal rows tightest legend clearance %.1f units (%s), %d wanted\n", worst,
               worstWhat.c_str(), geo::kPedalLabelClearance);
    return bad == 0;
}

//------------------------------------------------------------------------
// The silkscreen ink, RE-MEASURED FROM THE ART. geometry.h names one of two inks per pedal and
// records the contrast ratios it was chosen by; this recomputes them from the enclosure's own
// pixels and fails if the named ink is not the higher-contrast of the two. That is what stops a
// re-export - a slightly lighter green, a different yellow - from leaving a face lettered in
// something nobody can read, which is a defect no other audit here would notice.
//
// WCAG 2.x relative luminance and contrast ratio, over the mean of the enclosure's fully-opaque
// pixels inside the body box the lettering actually occupies. The mean is the right statistic
// because the art is a flat colour under a concentric ring texture: there is no second colour
// region for an average to hide.
double srgbToLinear(double v)
{
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double luminanceOf(uint32_t rgb)
{
    return 0.2126 * srgbToLinear(((rgb >> 16) & 0xFF) / 255.0) +
           0.7152 * srgbToLinear(((rgb >> 8) & 0xFF) / 255.0) +
           0.0722 * srgbToLinear((rgb & 0xFF) / 255.0);
}
double contrastRatio(double a, double b)
{
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

bool auditPedalInk(ImageCache &images)
{
    // The body box in the ART's own pixels: the enclosure without its jack lugs, from the top of
    // the first knob row to the name's baseline. Everything lettered lands inside it.
    const int x0 = geo::kPedalFaceLeft * geo::kPedalArtW / geo::kPedalW;
    const int x1 = geo::kPedalFaceRight * geo::kPedalArtW / geo::kPedalW;
    const int y0 = (geo::kPedalKnob4Row1Y - geo::kPedalKnobR) * geo::kPedalArtH / geo::kPedalH;
    const int y1 = geo::kPedalNameY * geo::kPedalArtH / geo::kPedalH;

    int bad = 0;
    std::string line;
    for (int i = 0; i < geo::kPedalCount; ++i) {
        cairo_surface_t *art = images.get(geo::kPedals[i].art);
        if (!art)
            continue; // already counted as a missing asset
        cairo_surface_flush(art);
        const unsigned char *data = cairo_image_surface_get_data(art);
        const int stride = cairo_image_surface_get_stride(art);
        const int w = cairo_image_surface_get_width(art);
        const int h = cairo_image_surface_get_height(art);
        if (!data)
            continue;
        double sr = 0, sg = 0, sb = 0;
        long n = 0;
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
                const uint32_t px = *reinterpret_cast<const uint32_t *>(data + y * stride + x * 4);
                if ((px >> 24) != 0xFFu)
                    continue; // premultiplied: only fully-opaque pixels are read directly
                sr += (px >> 16) & 0xFF;
                sg += (px >> 8) & 0xFF;
                sb += px & 0xFF;
                ++n;
            }
        if (n == 0) {
            fprintf(stderr, "panelrender: %s.png has no opaque body pixels to measure\n",
                    geo::kPedals[i].art);
            ++bad;
            continue;
        }
        const uint32_t mean = (static_cast<uint32_t>(sr / n + 0.5) << 16) |
                              (static_cast<uint32_t>(sg / n + 0.5) << 8) |
                              static_cast<uint32_t>(sb / n + 0.5);
        // While the art is open, re-derive the printable FACE from it. geometry.h names 22..168
        // and every legend's allowance is measured against those two numbers; they were wrong
        // once (they were the outermost opaque pixel, which is the outside of the border trim,
        // not the edge of the coloured face) and a legend printed on the border as a result. Walk
        // out from the centre line until the face colour gives way to the trim.
        {
            const int yMid = (y0 + y1) / 2;
            const uint32_t *row = reinterpret_cast<const uint32_t *>(data + yMid * stride);
            auto lumAt = [&](int x) {
                const uint32_t q = row[x];
                return luminanceOf(q & 0x00FFFFFFu);
            };
            const int mid = w / 2;
            const double ref = lumAt(mid);
            int fr = mid, fl = mid;
            while (fr + 1 < w && (row[fr + 1] >> 24) > 200u && lumAt(fr + 1) > ref * 0.35)
                ++fr;
            while (fl - 1 >= 0 && (row[fl - 1] >> 24) > 200u && lumAt(fl - 1) > ref * 0.35)
                --fl;
            const int faceL = fl * geo::kPedalW / geo::kPedalArtW;
            const int faceR = (fr + 1) * geo::kPedalW / geo::kPedalArtW;
            if (faceL > geo::kPedalFaceLeft || faceR < geo::kPedalFaceRight) {
                fprintf(stderr,
                        "panelrender: %s's printable face measures %d..%d but geometry.h letters "
                        "it to %d..%d — lettering would print on the border trim. Update "
                        "kPedalFaceLeft/kPedalFaceRight and re-check every legend's allowance\n",
                        geo::kPedals[i].name, faceL, faceR, geo::kPedalFaceLeft,
                        geo::kPedalFaceRight);
                ++bad;
            }
        }

        const double ratio =
            contrastRatio(luminanceOf(mean), luminanceOf(geo::kPedalInk));
        char buf[64];
        snprintf(buf, sizeof buf, "%s %.2f  ", geo::kPedals[i].name, ratio);
        line += buf;
    }
    if (bad == 0)
        printf("pedal ink  white on: %s(WCAG contrast against each enclosure's own mean face "
               "pixels; the two under 2.0 are what bright yellow costs, and no ink in the palette "
               "does better on them)\n",
               line.c_str());
    return bad == 0;
}

//------------------------------------------------------------------------
// The pedalboard's hit boxes, ONE PEDAL AT A TIME. Per pedal rather than across
// the page because the enclosures do not touch — kPedalGap is 22 units of bare
// board between them — so the only way two targets can fight is inside one face,
// and checking the whole page at once would drown that in 120 trivially-disjoint
// pairs.
//
// The footswitch is the one that can go wrong: its box is deliberately wider and
// taller than its cap (a foot, not a pointer), and every unit it grows is a unit
// nearer the mini slots above it.
bool auditPedalHitBoxes()
{
    // A target is a CIRCLE or a RECTANGLE, and the check has to know which. The
    // first version of this treated everything as its bounding box and reported
    // four overlaps on the three-knob faces that do not exist: a knob is tested
    // with hitCircle, and the centre knob of a triangle is 55.8 units from the
    // one above it against two 24-unit radii, so the circles clear each other by
    // 3.7 units while their boxes share a corner. An audit that fails on a shape
    // the editor never uses would have moved a layout that was correct.
    struct Target {
        std::string what;
        bool circle;
        float cx, cy, r;    // circle
        float l, t, rt, b;  // rect
    };
    auto overlap = [](const Target &a, const Target &b) {
        if (a.circle && b.circle) {
            const float dx = a.cx - b.cx, dy = a.cy - b.cy;
            return dx * dx + dy * dy < (a.r + b.r) * (a.r + b.r);
        }
        if (!a.circle && !b.circle)
            return a.l < b.rt && b.l < a.rt && a.t < b.b && b.t < a.b;
        const Target &c = a.circle ? a : b;
        const Target &q = a.circle ? b : a;
        // Closest point on the rectangle to the circle's centre.
        const float px = std::max(q.l, std::min(c.cx, q.rt));
        const float py = std::max(q.t, std::min(c.cy, q.b));
        const float dx = c.cx - px, dy = c.cy - py;
        return dx * dx + dy * dy < c.r * c.r;
    };

    int hits = 0;
    int targets = 0;
    for (int i = 0; i < geo::kPedalCount; ++i) {
        std::vector<Target> t;
        for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
            const PedalParamSpec &spec = kPedalParams[pedalKnobParam(i, k)];
            const geo::PedalPoint pt = geo::pedalKnobCenter(i, k);
            t.push_back({spec.title, true, static_cast<float>(pt.x), static_cast<float>(pt.y),
                         static_cast<float>(geo::kPedalKnobHitR), 0, 0, 0, 0});
        }
        for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m) {
            const PedalParamSpec &spec = kPedalParams[pedalMiniParam(i, m)];
            const geo::PedalPoint pt = geo::pedalMiniCenter(i, m);
            t.push_back({spec.title, false, 0, 0, 0,
                         static_cast<float>(pt.x - geo::kPedalMiniW / 2),
                         static_cast<float>(pt.y - geo::kPedalMiniH / 2),
                         static_cast<float>(pt.x + geo::kPedalMiniW / 2),
                         static_cast<float>(pt.y + geo::kPedalMiniH / 2)});
        }
        const geo::PedalPoint sw = geo::pedalSwitchCenter(i);
        t.push_back({std::string(geo::kPedals[i].name) + " footswitch", false, 0, 0, 0,
                     static_cast<float>(geo::kPedalLeft(i) + geo::kPedalBodyLeft),
                     static_cast<float>(sw.y - geo::kPedalSwitchHitH / 2),
                     static_cast<float>(geo::kPedalLeft(i) + geo::kPedalBodyRight),
                     static_cast<float>(sw.y + geo::kPedalSwitchHitH / 2)});
        targets += static_cast<int>(t.size());

        for (size_t a = 0; a < t.size(); ++a)
            for (size_t b = a + 1; b < t.size(); ++b)
                if (overlap(t[a], t[b])) {
                    fprintf(stderr,
                            "panelrender: on the %s, the \"%s\" and \"%s\" hit targets overlap "
                            "— one of them will swallow the other's clicks\n",
                            geo::kPedals[i].name, t[a].what.c_str(), t[b].what.c_str());
                    ++hits;
                }
    }
    if (hits == 0)
        printf("pedal hits %d targets across %d faces, none overlapping within a face\n", targets,
               geo::kPedalCount);
    return hits == 0;
}

//------------------------------------------------------------------------
// What the utility row's two legends must keep clear of each other, and of the dial legends on
// the row below. Eight units at kToggleLabelSize = 10 is about a whole cap height, which is what
// stops two centred legends in one column reading as one stacked pair.
constexpr float kUtilityLegendGap = 8.0f;

bool auditText(FontStack &fonts)
{
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(scratch);
    Canvas c(cr, &fonts, 8, 8);

    std::vector<TextFit> fits;

    // The wordmark. Its band is bounded on the left by the utility row and on the
    // right by the gear, and it is centred between them. The left bound is the
    // EQ lamp, which is the rightmost thing in that row — it used to be the
    // bypass switch's click target, and reading it off the row rather than off
    // one named control is what stops this going stale the next time the row
    // grows.
    float utilityRight = geo::kBypassToggleCX + geo::kTopToggleHitW / 2.0f;
    for (int i = 0; i < geo::kTopToggleCount; ++i) {
        utilityRight = std::max(utilityRight, geo::kTopToggles[i].cx + geo::kTopToggleHitW / 2.0f);
        utilityRight =
            std::max(utilityRight, geo::kTopLedCX[i] + static_cast<float>(geo::kTopLedR));
    }
    const float titleRoom =
        2.0f * std::min(geo::kFaceCX - utilityRight,
                        (geo::kGearCX - geo::kGearR) - static_cast<float>(geo::kFaceCX));
    fits.push_back({"wordmark", Font::Title, geo::kTitleSize, "Rations", titleRoom});

    // Dial legends: they must not reach their neighbours', so the allowance is
    // the pitch less a gap.
    for (const geo::KnobSpec &k : geo::kKnobs)
        fits.push_back(
            {"dial legend", Font::Title, geo::kKnobLabelSize, k.label, geo::kKnobPitch - 8.0f});
    // The permanent value rows, in the widest string each dial can ever show rather than in the
    // demo position above — the column has to hold "-100.0 dB" whether or not the render happens
    // to. Same allowance as the legend over it: a value that reached its neighbour's column would
    // be the same defect one row down.
    for (int i = 0; i < geo::kKnobCount; ++i)
        if (kWorstKnobValue[i])
            fits.push_back({"dial value", Font::Body, geo::kKnobValueSize, kWorstKnobValue[i],
                            geo::kKnobPitch - 8.0f});
    // Input / Output sit in their own column outside the dial row; the limit is
    // the canvas edge on one side and the meter column's own width on the other.
    for (const geo::KnobSpec &k : geo::kIoKnobs)
        fits.push_back(
            {"i/o legend", Font::Title, geo::kIoLabelSize, k.label, 2.0f * geo::kSideCXL});
    fits.push_back(
        {"i/o value", Font::Body, geo::kKnobValueSize, kWorstIoValue, 2.0f * geo::kSideCXL});
    // BYPASS is drawn BELOW its LED, not beside it, so the LED is not what
    // bounds it: the input meter's column is, on the left, and the wordmark on
    // the right (which is further away, so the left bound decides it). EQ sits
    // between the two, so this allowance is the outer bound for both of them —
    // what separates the PAIR is measured below, because two centred legends of
    // different widths are a pairwise question and not an allowance one.
    for (const geo::ToggleSpec &t : geo::kTopToggles)
        fits.push_back({"toggle legend", Font::Title, geo::kToggleLabelSize, t.label,
                        2.0f * (geo::kBypassToggleCX -
                                static_cast<float>(geo::kInputMeter.x + geo::kMeterW) - 8.0f)});
    for (const geo::ButtonSpec &b : geo::kPageButtons)
        fits.push_back(
            {"page button", Font::Title, geo::kPageButtonTextSize, b.label, b.w - 16.0f});
    fits.push_back({"back button", Font::Title, geo::kPageButtonTextSize, geo::kBackButton.label,
                    geo::kBackButton.w - 16.0f});

    // Cabinet page.
    fits.push_back({"blend legend", Font::Title, geo::kBlendLabelSize, "Blend",
                    2.0f * geo::kBlendHitR * 2.0f});
    fits.push_back({"ir placeholder", Font::Body, geo::kFileRowTextSize, geo::kIrRowB.placeholder,
                    geo::kIrRowW - geo::kIrTextDX - 10.0f});

    // Pedalboard page. Each pedal's name has to fit its own enclosure, not the
    // page, and the allowance is the body without its jack lugs — a name that ran
    // over the lugs would sit on the cable rather than on the box.
    for (int i = 0; i < geo::kPedalCount; ++i)
        fits.push_back({geo::kPedals[i].name, Font::Title, geo::kPedalNameSize,
                        geo::kPedals[i].name,
                        static_cast<float>(geo::kPedalFaceW)});
    // The row legend sits in the band ABOVE its row, not beside it, so its
    // allowance is the page's own width less the margin it starts at. (The first
    // version of this measured the space to the LEFT of the leftmost pedal, which
    // is exactly zero — the POST row's first enclosure starts at the same x the
    // legend does. The audit caught it, which is what the audit is for.)
    fits.push_back({"pedal row legend", Font::Title, geo::kPedalRowLegendSize, "POST",
                    static_cast<float>(geo::kPedalPageW - 2 * geo::kPedalRowLegendX)});
    // Every knob legend on every face, measured against the space one knob's
    // column actually has. The allowance is the knob PITCH less a margin, not the
    // enclosure width: two knobs sit side by side on most of these faces, and a
    // legend wider than its own column runs into its neighbour's rather than off
    // the box, which is the failure that would not be visible at the edge.
    for (int i = 0; i < geo::kPedalCount; ++i)
        for (int k = 0, nk = pedalKnobCount(i); k < nk; ++k) {
            const PedalParamSpec &spec = kPedalParams[pedalKnobParam(i, k)];
            fits.push_back({spec.title, Font::Title, geo::kPedalLabelSize, spec.legend,
                            static_cast<float>(geo::pedalKnobLabelAllowance(i, k))});
        }
    // The mini slots: their own legends, and — because a list draws its VALUE and
    // not its name — every one of the twelve sync divisions. Any of them can be
    // the string on screen, so all of them are measured.
    for (int i = 0; i < geo::kPedalCount; ++i)
        for (int m = 0, nm = pedalMiniCount(i); m < nm; ++m) {
            const PedalParamSpec &spec = kPedalParams[pedalMiniParam(i, m)];
            if (spec.kind != PedalParamKind::List) {
                fits.push_back({spec.title, Font::Title, geo::kPedalMiniSize, spec.legend,
                                static_cast<float>(geo::kPedalMiniW - 6)});
                continue;
            }
            for (int v = 0; v < kDelaySyncCount; ++v)
                fits.push_back({spec.title, Font::Title, geo::kPedalMiniSize, kDelaySyncNames[v],
                                static_cast<float>(geo::kPedalMiniW - 6)});
        }

    // Settings page.
    fits.push_back({"settings heading", Font::Title, geo::kSettingsHeadingSize, "MIDI Learn",
                    static_cast<float>(geo::kMidiRowW)});
    for (int i = 0; i < kMidiLearnRowCount; ++i)
        fits.push_back({"midi row", Font::Title, geo::kMidiRowTextSize, kMidiLearnRows[i].label,
                        static_cast<float>(geo::kMidiTextX) - 20.0f});
    // The channel-level section. The name shares the MIDI section's allowance because both
    // columns start at the same x, and the readout is measured at the widest value it can hold -
    // which is the negative extreme, since the minus sign is wider than the plus.
    fits.push_back({"level heading", Font::Title, geo::kSettingsHeadingSize, geo::kLevelHeading,
                    static_cast<float>(geo::kMidiRowW)});
    // Four, not kMidiLearnRowCount: the MIDI list has the pedals in it too and the levels do not,
    // and the two counts stopped being the same number when the footswitch rows landed.
    for (int i = 0; i < geo::kLevelRowCount; ++i)
        fits.push_back({"level row", Font::Title, geo::kMidiRowTextSize, kMidiLearnRows[i].label,
                        static_cast<float>(geo::kLevelSliderX) - 20.0f});
    static char levelReadout[2][24];
    snprintf(levelReadout[0], sizeof(levelReadout[0]), "%+.1f dB", ranges::kLevelMin);
    snprintf(levelReadout[1], sizeof(levelReadout[1]), "%+.1f dB", ranges::kLevelMax);
    for (const char *t : levelReadout)
        fits.push_back({"level readout", Font::Body, geo::kMidiRowTextSize, t,
                        static_cast<float>(geo::kLevelReadoutW)});
    fits.push_back({"level footnote", Font::Body, geo::kSettingsFootnoteSize, geo::kLevelFootnote,
                    static_cast<float>(geo::kMidiRowW)});
    fits.push_back({"midi heading", Font::Title, geo::kSettingsHeadingSize, geo::kMidiHeading,
                    static_cast<float>(geo::kMidiRowW)});

    fits.push_back({"midi learn button", Font::Title, geo::kPageButtonTextSize,
                    geo::kMidiLearnLabel, geo::kMidiLearnW - 16.0f});
    // The Learn button does not resize when it starts listening, so the longer of its two
    // legends is the one that has to fit - and it is the one nobody looks at until it is on
    // screen with a player's foot over the pedal.
    fits.push_back({"midi listen button", Font::Title, geo::kPageButtonTextSize,
                    geo::kMidiListenLabel, geo::kMidiLearnW - 16.0f});
    fits.push_back({"midi clear button", Font::Title, geo::kPageButtonTextSize,
                    geo::kMidiClearLabel, geo::kMidiClearW - 16.0f});
    // Every shape a binding can be written in, against the space between the channel name and the
    // Clear button. The widest of each kind rather than a typical one.
    // An unlearned row is not in this list because it no longer draws anything.
    static const MidiBinding kWidest[] = {
        {MidiMsg::ControlChange, kMidiAnyChannel, 127},
        {MidiMsg::ProgramChange, kMidiAnyChannel, 127},
        {MidiMsg::NoteOn, 15, 1},
    };
    static std::vector<std::string> bindingText;
    bindingText.clear();
    for (const MidiBinding &b : kWidest)
        bindingText.push_back(describeBinding(b));
    bindingText.push_back(geo::kMidiListeningText);
    for (const std::string &t : bindingText)
        fits.push_back({"midi binding", Font::Body, geo::kMidiRowTextSize, t.c_str(),
                        static_cast<float>(geo::kMidiTextW)});
    fits.push_back({"settings footnote", Font::Body, geo::kSettingsFootnoteSize,
                    geo::kSettingsFootnote, static_cast<float>(geo::kMidiRowW)});
    fits.push_back({"settings footnote 2", Font::Body, geo::kSettingsFootnoteSize,
                    geo::kSettingsFootnote2, static_cast<float>(geo::kMidiRowW)});
    fits.push_back({"settings footnote 3", Font::Body, geo::kSettingsFootnoteSize,
                    geo::kSettingsFootnote3, static_cast<float>(geo::kMidiRowW)});

    // The utility row, measured as a pair rather than against an allowance.
    //
    // BYPASS and EQ are both centred on their own switch and are very different
    // widths ("BYPASS" is nearly three times "EQ"), so neither one fits inside
    // half the pitch and neither one needs to: what matters is the gap between
    // the two ink boxes. The row was fitted into the band the channel lamps
    // vacated and there is nothing to spare in it, so the gap is measured.
    //
    // The vertical bound is measured here too, and it is the one the lamps'
    // move made tight: these legends sit directly above the dial legends, and
    // the static_assert in geometry.h can only use the font SIZE as a stand-in
    // for the cap height. This uses the real ink.
    int bad = 0;
    {
        c.setFont(Font::Title);
        c.setFontSize(geo::kToggleLabelSize);
        float prevRight = -1e9f;
        const char *prevWhat = nullptr;
        for (const geo::ToggleSpec &t : geo::kTopToggles) {
            const float half = 0.5f * c.stringWidth(t.label);
            if (prevWhat && t.cx - half - prevRight < kUtilityLegendGap) {
                fprintf(stderr,
                        "panelrender: the \"%s\" and \"%s\" legends are %.1f units apart, %.0f "
                        "wanted — the utility row is out of space, see kTopPairPitch\n",
                        prevWhat, t.label, t.cx - half - prevRight, kUtilityLegendGap);
                ++bad;
            }
            prevRight = t.cx + half;
            prevWhat = t.label;
        }
        float utilInk = geo::kBypassToggleCY + geo::kToggleLabelDY;
        for (const geo::ToggleSpec &t : geo::kTopToggles)
            utilInk = std::max(utilInk, geo::kBypassToggleCY + geo::kToggleLabelDY +
                                            c.stringDescent(t.label));
        // The dial legends underneath. Every one of the eight, because they are all on one row
        // and the tallest ink is what the row above has to clear — and a channel's legend is a
        // user's folder name at run time, so the table's own strings are a floor and not a
        // guarantee. That is what the clip to kKnobPitch - 6 handles horizontally; vertically a
        // cap is a cap.
        c.setFontSize(static_cast<float>(geo::kKnobLabelSize));
        float legendInk = 1e9f;
        for (const geo::KnobSpec &k : geo::kKnobs)
            legendInk = std::min(legendInk, (k.cy - static_cast<float>(geo::kKnobLabelDY)) -
                                                c.stringAscent(k.label));
        if (legendInk - utilInk < kUtilityLegendGap) {
            fprintf(stderr,
                    "panelrender: the utility row's legends stop at y=%.1f and the dial legends "
                    "start at y=%.1f, %.0f units wanted — see kKnobLabelDY\n",
                    utilInk, legendInk, kUtilityLegendGap);
            ++bad;
        }

        // The value row that the gate switch's move opened up, against the page buttons that sit
        // below it. Bass, Middle and Treble are all inside those buttons' x span, so this is a
        // real overlap and not a theoretical one — and it is measured with the descender rather
        // than with the font size, because "-100.0 dB" has no descender and "5.0" has none
        // either, so the size would report a clearance the ink does not need.
        c.setFont(Font::Body);
        c.setFontSize(static_cast<float>(geo::kKnobValueSize));
        float valueInk = -1e9f;
        for (int i = 0; i < geo::kKnobCount; ++i)
            if (kWorstKnobValue[i])
                valueInk = std::max(valueInk, (geo::kKnobs[i].cy + geo::kKnobValueDY) +
                                                  c.stringDescent(kWorstKnobValue[i]));
        if (static_cast<float>(geo::kPageButtonY) - valueInk < kUtilityLegendGap) {
            fprintf(stderr,
                    "panelrender: the dial value row's ink reaches y=%.1f and the page buttons "
                    "start at y=%d, %.0f units wanted — see kKnobValueDY\n",
                    valueInk, geo::kPageButtonY, kUtilityLegendGap);
            ++bad;
        }
    }
    for (const TextFit &f : fits) {
        c.setFont(f.font);
        c.setFontSize(f.size);
        const float w = c.stringWidth(f.text);
        if (w > f.allowance) {
            fprintf(stderr,
                    "panelrender: %s \"%s\" is %.0f px at size %.0f but has %.0f — lower "
                    "the size in geometry.h or widen the space\n",
                    f.where, f.text, w, f.size, f.allowance);
            ++bad;
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);

    // Every distinct Michroma size the panel uses, checked once, over the caps
    // and digits that size actually sets.
    std::map<float, std::string> capsAtSize;
    for (const TextFit &f : fits) {
        if (f.font != Font::Title)
            continue;
        std::string &set = capsAtSize[f.size];
        for (const char *p = f.text; *p; ++p)
            if (((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) &&
                set.find(*p) == std::string::npos)
                set.push_back(*p);
    }
    int checked = 0;
    for (const auto &entry : capsAtSize) {
        // One capital cannot disagree with itself.
        if (entry.second.size() < 2)
            continue;
        ++checked;
        const int step = capStep(fonts, entry.first, entry.second);
        if (step != 0) {
            fprintf(stderr,
                    "panelrender: Michroma %.0f grid-fits \"%s\" onto %d different rows, so a "
                    "legend at this size has visibly short letters (\"OD2\" is where it shows) — "
                    "move to the nearest cap-even size, see geometry.h\n",
                    entry.first, entry.second.c_str(), step + 1);
            ++bad;
        }
    }

    if (bad == 0)
        printf("text       %zu legends inside their allowance, %d Michroma sizes cap-even\n",
               fits.size(), checked);
    return bad == 0;
}

//------------------------------------------------------------------------
bool renderPage(const char *path, geo::Page page, double scale, FontStack &fonts,
                ImageCache &images, SvgCache &icons,
                void (*draw)(Canvas &, ImageCache &, SvgCache &))
{
    // Each page is its own window: the editor keeps its scale across a page
    // change and asks the host to resize to the incoming page's base size.
    const geo::PageSize base = geo::pageSize(page);
    const int pxW = static_cast<int>(std::lround(base.w * scale));
    const int pxH = static_cast<int>(std::lround(base.h * scale));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxW, pxH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "panelrender: cannot create a %dx%d surface\n", pxW, pxH);
        cairo_surface_destroy(surface);
        return false;
    }
    cairo_t *cr = cairo_create(surface);
    // The single scale the whole design rests on: everything drawn is in logical
    // units, exactly as the editor draws it.
    cairo_scale(cr, scale, scale);

    Canvas c(cr, &fonts, static_cast<float>(base.w), static_cast<float>(base.h));
    draw(c, images, icons);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const cairo_status_t st = cairo_surface_write_to_png(surface, path);
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "panelrender: cannot write %s: %s\n", path, cairo_status_to_string(st));
        return false;
    }
    printf("wrote      %s (%dx%d, scale %.2f)\n", path, pxW, pxH, scale);
    return true;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output-prefix> [resource-dir] [scale]\n", argv[0]);
        return 2;
    }
    const std::string prefix = argv[1];
    const std::string res = (argc > 2) ? std::string(argv[2]) : resourceDir();
    const double scale = (argc > 3) ? atof(argv[3]) : 1.0;
    if (res.empty()) {
        fprintf(stderr, "panelrender: no resource directory; pass one explicitly\n");
        return 1;
    }
    // The lowest floor any page has, which is the settings page's: the pages do not share one
    // any more. Each page is still rendered whole here, at its full height and with no scroll,
    // because what this tool audits is the ART — whether a legend fits the space geometry.h gives
    // it — and that question is asked of the page, not of the window showing part of it.
    if (scale < geo::kSettingsScaleMin || scale > geo::kScaleMax) {
        fprintf(stderr, "panelrender: scale %.3f is outside the editor's range [%.2f, %.2f]\n",
                scale, geo::kSettingsScaleMin, geo::kScaleMax);
        return 2;
    }
    printf("resources  %s\n", res.c_str());

    //--- audit -----------------------------------------------------------
    int missing = 0;

    FontStack fonts;
    if (!fonts.load(res)) {
        fprintf(stderr, "panelrender: MISSING font (see the warning above)\n");
        ++missing;
    }

    ImageCache images;
    images.setResourceDir(res);
    for (const char *name : kRequiredImages) {
        if (!images.get(name)) {
            fprintf(stderr, "panelrender: MISSING img/%s.png\n", name);
            ++missing;
        }
    }

    SvgCache icons;
    icons.setResourceDir(res);
    for (const char *name : kRequiredIcons) {
        if (!icons.getByHeight(name, 16)) {
            fprintf(stderr, "panelrender: MISSING img/%s.svg\n", name);
            ++missing;
        }
    }

    // The canvas size is derived from the art, so a re-export that changes the
    // trim silently moves every control. Catch it here rather than on screen.
    if (cairo_surface_t *head = images.get("head")) {
        const int bw = cairo_image_surface_get_width(head);
        const int bh = cairo_image_surface_get_height(head);
        if (bw != geo::kWinW || bh != geo::kWinH) {
            fprintf(stderr,
                    "panelrender: head.png is %dx%d but geometry.h says %dx%d — regenerate the "
                    "art or update kWinW/kWinH (and gui/geometry.sh) together\n",
                    bw, bh, geo::kWinW, geo::kWinH);
            ++missing;
        }
    }
    // The Blend dial's position is a FRACTION of the cabinet art, so a re-export
    // that changes the cabinet's aspect moves the painted knob out from under
    // the dial drawn over it.
    if (cairo_surface_t *cab = images.get("cabinet")) {
        const int cw = cairo_image_surface_get_width(cab);
        const int ch = cairo_image_surface_get_height(cab);
        const double want = static_cast<double>(geo::kCabW) / geo::kCabH;
        const double have = static_cast<double>(cw) / ch;
        if (std::fabs(want - have) > 0.01) {
            fprintf(stderr,
                    "panelrender: cabinet.png is %dx%d (aspect %.4f) but geometry.h draws it at "
                    "%dx%d (aspect %.4f) — the Blend dial would no longer sit on the painted "
                    "knob\n",
                    cw, ch, have, geo::kCabW, geo::kCabH, want);
            ++missing;
        }
    }

    // Every enclosure is drawn at ONE size and every control on it is placed by
    // scaling the mock's own pixels down from the trimmed export, so a re-export
    // whose trim moves by a pixel moves all five faces at once. The five must
    // also agree with each other: one layout serves all of them, which is only
    // legitimate while they really are the same shape.
    for (int i = 0; i < geo::kPedalCount; ++i) {
        cairo_surface_t *art = images.get(geo::kPedals[i].art);
        if (!art)
            continue; // already counted as a missing asset above
        const int aw = cairo_image_surface_get_width(art);
        const int ah = cairo_image_surface_get_height(art);
        if (aw != geo::kPedalArtW || ah != geo::kPedalArtH) {
            fprintf(stderr,
                    "panelrender: %s.png is %dx%d but geometry.h says the enclosures trim to "
                    "%dx%d — re-run gui/make_pedals.sh, or update kPedalArtW/kPedalArtH and "
                    "re-derive every control coordinate from the mock\n",
                    geo::kPedals[i].art, aw, ah, geo::kPedalArtW, geo::kPedalArtH);
            ++missing;
        }
    }

    // Text is audited even when a font fell back to a system face — a legend
    // that overflows in the fallback still overflows on screen.
    if (!auditText(fonts))
        ++missing;
    if (!auditHitBoxes())
        ++missing;
    if (!auditPedalHitBoxes())
        ++missing;
    if (!auditPedalInk(images))
        ++missing;
    if (!auditPedalClearance(fonts))
        ++missing;

    if (missing) {
        fprintf(stderr, "panelrender: %d asset problem(s); not rendering\n", missing);
        return 1;
    }
    printf("assets     %d images, %d icons, 2 fonts — all present\n", kImageCount, kIconCount);

    //--- render ----------------------------------------------------------
    struct PageOut {
        const char *suffix;
        geo::Page page;
        void (*draw)(Canvas &, ImageCache &, SvgCache &);
    };
    const PageOut pages[] = {
        {"-head.png", geo::Page::Head, renderHead},
        {"-cabinet.png", geo::Page::Cabinet, renderCabinet},
        {"-pedalboard.png", geo::Page::Pedalboard, renderPedalboard},
        {"-settings.png", geo::Page::Settings, renderSettings},
    };
    for (const PageOut &p : pages)
        if (!renderPage((prefix + p.suffix).c_str(), p.page, scale, fonts, images, icons, p.draw))
            return 1;

    return 0;
}
