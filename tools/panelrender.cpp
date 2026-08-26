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
//   2. RENDER. Each page is drawn through the same Canvas, FontStack, PNG and
//      SVG code the editor uses, with no X11, no host and no VST3 objects, so
//      layout and art can be judged before any windowing work exists.
//
// Usage: panelrender <output-prefix> [resource-dir] [scale]
// Writes <prefix>-head.png, <prefix>-cabinet.png and <prefix>-pedalboard.png.
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
#include <string>

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
        drawCenteredText(c, Font::Body, geo::kToggleLabelSize, geo::kTextColor, t.label, t.cx,
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
    drawCenteredText(c, Font::Body, geo::kPageButtonTextSize, geo::kTextColor, b.label, r.centerX(),
                     r.bottom() - 9.0f);
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
    c.setFontSize(12);
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
        drawCenteredText(c, Font::Body, geo::kKnobLabelSize, geo::kTextColor, geo::kKnobs[i].label,
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
        drawCenteredText(c, Font::Body, geo::kIoLabelSize, geo::kTextColor, geo::kIoKnobs[i].label,
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
        c, Font::Body, geo::kBlendLabelSize, bothLoaded ? geo::kTextColor : geo::kDimColor, "Blend",
        static_cast<float>(geo::kBlendCX), static_cast<float>(geo::kBlendLabelBaselineY));

    drawIrRow(c, icons, geo::kIrRowA, "Marshall 1960A - SM57 cap edge.wav", true);
    drawIrRow(c, icons, geo::kIrRowB, geo::kIrRowB.placeholder, false);

    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
void renderPedalboard(Canvas &c, ImageCache &, SvgCache &)
{
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());
    drawCenteredText(c, Font::Title, geo::kPedalPlaceholderSize, geo::kDimColor, "Pedalboard",
                     geo::kFaceCX, geo::kPedalPlaceholderY);
    drawCenteredText(c, Font::Body, 14, geo::kDimColor, "overdrive, flanger, chorus, delay, reverb",
                     geo::kFaceCX, geo::kPedalPlaceholderY + 30);
    drawButton(c, geo::kBackButton);
}

//------------------------------------------------------------------------
bool renderPage(const char *path, double scale, FontStack &fonts, ImageCache &images,
                SvgCache &icons, void (*draw)(Canvas &, ImageCache &, SvgCache &))
{
    const int pxW = static_cast<int>(std::lround(geo::kWinW * scale));
    const int pxH = static_cast<int>(std::lround(geo::kWinH * scale));
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

    Canvas c(cr, &fonts, geo::kWinW, geo::kWinH);
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

    if (missing) {
        fprintf(stderr, "panelrender: %d asset problem(s); not rendering\n", missing);
        return 1;
    }
    printf("assets     %d images, %d icons, 2 fonts — all present\n", kImageCount, kIconCount);

    //--- render ----------------------------------------------------------
    struct PageOut {
        const char *suffix;
        void (*draw)(Canvas &, ImageCache &, SvgCache &);
    };
    const PageOut pages[] = {
        {"-head.png", renderHead},
        {"-cabinet.png", renderCabinet},
        {"-pedalboard.png", renderPedalboard},
    };
    for (const PageOut &p : pages)
        if (!renderPage((prefix + p.suffix).c_str(), scale, fonts, images, icons, p.draw))
            return 1;

    return 0;
}
