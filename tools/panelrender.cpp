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
    "head",           "cabinet",          "dial",        "led_on", "led_off",
    "switch_up_ring", "switch_down_ring", "meter_track",
};
// Every icon the editor rasterises. Folder is the author's own; the rest are
// the original plug-in's, unmodified.
const char *const kRequiredIcons[] = {
    "Folder", "File", "Gear", "Cross", "ArrowLeft", "ArrowRight",
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
    const Rect dest(t.cx - geo::kToggleW / 2.0f, t.cy - geo::kToggleH / 2.0f, geo::kToggleW,
                    geo::kToggleH);
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
void drawLed(Canvas &c, ImageCache &images, float cx, float cy, bool lit)
{
    const Rect dest(cx - geo::kLedR, cy - geo::kLedR, 2.0f * geo::kLedR, 2.0f * geo::kLedR);
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
    }

    // Five LEDs and five bat switches: one per channel, plus the gate. Exactly
    // one channel LED is ever lit, which is the whole point of kChannelId being
    // a list parameter rather than four booleans.
    const bool gateOn = true;
    for (int i = 0; i < geo::kToggleCount; ++i) {
        const bool on = (i < geo::kChannelToggleCount) ? (i == activeChannel) : gateOn;
        drawToggle(c, images, geo::kToggles[i], on);
        drawLed(c, images, static_cast<float>(geo::kToggles[i].cx), static_cast<float>(geo::kLedCY),
                on);
    }

    // Bypass, in the band left of the wordmark. Not in the mock — see geometry.h.
    drawToggle(c, images, geo::kBypassToggle, false);
    drawLed(c, images, static_cast<float>(geo::kBypassLedCX), static_cast<float>(geo::kBypassLedCY),
            true);

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
    }

    for (const geo::ButtonSpec &b : geo::kPageButtons)
        drawButton(c, b);

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
void renderPedalboard(Canvas &c, ImageCache &, SvgCache &)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Pedalboard));
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());
    drawCenteredText(c, Font::Title, geo::kPedalPlaceholderSize, geo::kDimColor, "Pedalboard", cx,
                     static_cast<float>(geo::kPedalPlaceholderY));
    drawCenteredText(c, Font::Body, geo::kPedalCaptionSize, geo::kDimColor,
                     "overdrive, flanger, chorus, delay, reverb", cx,
                     static_cast<float>(geo::kPedalPlaceholderY + geo::kPedalCaptionDY));
    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
// The settings page: the channel trims, then MIDI learn. The values drawn here
// are stand-ins - this tool has no processor to ask - chosen so that every state
// each kind of row can be in is on screen at once, since each is a different
// width and a different set of controls. For the trims that means both extremes,
// the default, and a value in between; for MIDI it means learned, listening and
// unlearned.
void renderSettings(Canvas &c, ImageCache &, SvgCache &)
{
    const float cx = static_cast<float>(geo::pageCX(geo::Page::Settings));
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());

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
    };
    const int kArmed = 2; // one row shown listening, so that state is on screen too

    for (int i = 0; i < geo::kMidiRowCount; ++i) {
        const Rect r(geo::kMidiRowX, geo::kMidiRowY0 + i * geo::kMidiRowPitch, geo::kMidiRowW,
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

    // The upper band: bypass and the gear. Not the same row as the five bat switches, but on the
    // same faceplate and hit-tested from the same click, so they belong in the same check — and
    // the bypass pair in particular has been moved once already.
    boxes.push_back({"BYPASS", geo::kBypassToggle.cx - geo::kToggleHitW / 2.0f,
                     static_cast<float>(geo::kBypassToggle.cy + geo::kToggleHitTop),
                     geo::kBypassToggle.cx + geo::kToggleHitW / 2.0f,
                     static_cast<float>(geo::kBypassToggle.cy + geo::kToggleHitBottom)});
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
bool auditText(FontStack &fonts)
{
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(scratch);
    Canvas c(cr, &fonts, 8, 8);

    std::vector<TextFit> fits;

    // The wordmark. Its band is bounded on the left by the bypass toggle and on
    // the right by the gear, and it is centred between them.
    const float titleRoom =
        2.0f * std::min(geo::kFaceCX - (geo::kBypassToggleCX + geo::kToggleHitW / 2.0f),
                        (geo::kGearCX - geo::kGearR) - static_cast<float>(geo::kFaceCX));
    fits.push_back({"wordmark", Font::Title, geo::kTitleSize, "Rations", titleRoom});

    // Dial legends: they must not reach their neighbours', so the allowance is
    // the pitch less a gap.
    for (const geo::KnobSpec &k : geo::kKnobs)
        fits.push_back(
            {"dial legend", Font::Title, geo::kKnobLabelSize, k.label, geo::kKnobPitch - 8.0f});
    // Input / Output sit in their own column outside the dial row; the limit is
    // the canvas edge on one side and the meter column's own width on the other.
    for (const geo::KnobSpec &k : geo::kIoKnobs)
        fits.push_back(
            {"i/o legend", Font::Title, geo::kIoLabelSize, k.label, 2.0f * geo::kSideCXL});
    // BYPASS is drawn BELOW its LED, not beside it, so the LED is not what
    // bounds it: the input meter's column is, on the left, and the wordmark on
    // the right (which is further away, so the left bound decides it).
    fits.push_back({"toggle legend", Font::Title, geo::kToggleLabelSize, geo::kBypassToggle.label,
                    2.0f * (geo::kBypassToggleCX - (geo::kInputMeter.x + geo::kMeterW) - 8.0f)});
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

    // Pedalboard page.
    fits.push_back({"pedal heading", Font::Title, geo::kPedalPlaceholderSize, "Pedalboard",
                    geo::kPedalPageW - 32.0f});
    fits.push_back({"pedal caption", Font::Body, geo::kPedalCaptionSize,
                    "overdrive, flanger, chorus, delay, reverb", geo::kPedalPageW - 32.0f});

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
    for (int i = 0; i < kMidiLearnRowCount; ++i)
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

    int bad = 0;
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
    if (scale < geo::kScaleMin || scale > geo::kScaleMax) {
        fprintf(stderr, "panelrender: scale %.3f is outside the editor's range [%.2f, %.2f]\n",
                scale, geo::kScaleMin, geo::kScaleMax);
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

    // Text is audited even when a font fell back to a system face — a legend
    // that overflows in the fallback still overflows on screen.
    if (!auditText(fonts))
        ++missing;
    if (!auditHitBoxes())
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
