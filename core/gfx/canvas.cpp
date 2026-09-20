// Canvas implementation. See canvas.h for the rect and angle conventions.

#include "canvas.h"
#include "fontstack.h"

#include <cmath>

namespace Rations
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

// Append a rounded-rect path. Cairo has no primitive for it; four arcs joined
// by the straight edges is the standard construction.
void roundRectPath(cairo_t *cr, const Rect &r, float radius)
{
    const double maxR = std::min(r.w, r.h) * 0.5;
    const double rad = std::min(static_cast<double>(radius), maxR);
    if (rad <= 0.0) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        return;
    }
    const double l = r.left(), t = r.top(), rt = r.right(), b = r.bottom();
    cairo_new_sub_path(cr);
    cairo_arc(cr, rt - rad, t + rad, rad, -0.5 * kPi, 0.0); // top-right
    cairo_arc(cr, rt - rad, b - rad, rad, 0.0, 0.5 * kPi);  // bottom-right
    cairo_arc(cr, l + rad, b - rad, rad, 0.5 * kPi, kPi);   // bottom-left
    cairo_arc(cr, l + rad, t + rad, rad, kPi, 1.5 * kPi);   // top-left
    cairo_close_path(cr);
}

} // namespace

//------------------------------------------------------------------------
Canvas::Canvas(cairo_t *cr, const FontStack *fonts, float width, float height)
    : mCr(cr), mFonts(fonts), mWidth(width), mHeight(height)
{
    cairo_set_line_cap(mCr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(mCr, CAIRO_LINE_JOIN_MITER);

    // METRIC HINTING OFF, and it is not a cosmetic preference. The panel is
    // drawn in logical units inside one cairo_scale(s, s), so a string's width
    // has to be s times its width at scale 1 or every centred legend walks as
    // the window is dragged. With cairo's default metric hinting each glyph
    // advance is rounded to a whole DEVICE pixel, and measuring "Crunch" in
    // Michroma at 13 against the same string at 52/4 disagrees by up to 1.25 px
    // — enough to shift a centred legend by a pixel at some window sizes and
    // nowhere else. Off, the two agree exactly, which is also what makes the
    // width allowances panelrender checks true at every scale rather than only
    // at 1.0.
    //
    // Hint style SLIGHT keeps the vertical grid-fitting that stops small text
    // going soft while leaving glyph outlines horizontally unhinted, so shapes
    // and spacing stay proportional. (FULL measures identically here — cap
    // heights are a vertical property and SLIGHT already snaps those — so
    // SLIGHT costs nothing and distorts less.)
    cairo_font_options_t *opts = cairo_font_options_create();
    if (cairo_font_options_status(opts) == CAIRO_STATUS_SUCCESS) {
        cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
        cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_OFF);
        cairo_set_font_options(mCr, opts);
    }
    cairo_font_options_destroy(opts);

    applyFont();
}

//------------------------------------------------------------------------
void Canvas::setColor(uint32_t rgb, int alpha)
{
    cairo_set_source_rgba(mCr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
                          (rgb & 0xff) / 255.0, alpha / 255.0);
}

void Canvas::setPenSize(float px)
{
    cairo_set_line_width(mCr, px);
}

//------------------------------------------------------------------------
void Canvas::fillRect(const Rect &r)
{
    cairo_rectangle(mCr, r.x, r.y, r.w, r.h);
    cairo_fill(mCr);
}

void Canvas::fillRoundRect(const Rect &r, float radius)
{
    roundRectPath(mCr, r, radius);
    cairo_fill(mCr);
}

void Canvas::strokeRoundRect(const Rect &r, float radius)
{
    // Half-pixel offset so a 1px stroke lands ON the boundary rather than
    // straddling two pixel rows.
    roundRectPath(mCr, Rect(r.x + 0.5f, r.y + 0.5f, r.w - 1.0f, r.h - 1.0f), radius);
    cairo_stroke(mCr);
}

void Canvas::strokeLine(float x0, float y0, float x1, float y1)
{
    cairo_move_to(mCr, x0, y0);
    cairo_line_to(mCr, x1, y1);
    cairo_stroke(mCr);
}

void Canvas::strokeBezier(float x0, float y0, float cx1, float cy1, float cx2, float cy2, float x3,
                          float y3)
{
    cairo_move_to(mCr, x0, y0);
    cairo_curve_to(mCr, cx1, cy1, cx2, cy2, x3, y3);
    cairo_stroke(mCr);
}

void Canvas::strokeConnector(float x0, float y0, float x1, float y1, float slack)
{
    strokeBezier(x0, y0, x0 + slack, y0, x1 - slack, y1, x1, y1);
}

void Canvas::fillEllipse(const Rect &r)
{
    fillEllipse(r.centerX(), r.centerY(), r.w * 0.5f, r.h * 0.5f);
}

void Canvas::fillEllipse(float cx, float cy, float rx, float ry)
{
    if (rx <= 0.0f || ry <= 0.0f)
        return;
    cairo_save(mCr);
    cairo_translate(mCr, cx, cy);
    cairo_scale(mCr, rx, ry);
    cairo_arc(mCr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
    cairo_restore(mCr); // restore before filling: keeps the pen circular
    cairo_fill(mCr);
}

void Canvas::strokeEllipse(float cx, float cy, float rx, float ry)
{
    if (rx <= 0.0f || ry <= 0.0f)
        return;
    cairo_save(mCr);
    cairo_translate(mCr, cx, cy);
    cairo_scale(mCr, rx, ry);
    cairo_arc(mCr, 0.0, 0.0, 1.0, 0.0, 2.0 * kPi);
    cairo_restore(mCr);
    cairo_stroke(mCr);
}

//------------------------------------------------------------------------
// Knob convention -> Cairo. Knob angles are degrees with 0 straight up and
// positive clockwise; Cairo's cairo_arc takes radians measured clockwise from
// east in a y-down space. An angle A therefore maps to (A - 90) degrees, and
// because both conventions run clockwise the sweep direction is preserved —
// cairo_arc always goes in increasing-angle (clockwise) order, which is what
// a0 < a1 means here too.
void Canvas::strokeArc(float cx, float cy, float radius, double a0Deg, double a1Deg, float pen,
                       uint32_t rgb)
{
    if (radius <= 0.0f)
        return;
    setColor(rgb);
    setPenSize(pen);
    const double a0 = (a0Deg - 90.0) * kPi / 180.0;
    const double a1 = (a1Deg - 90.0) * kPi / 180.0;
    cairo_new_sub_path(mCr);
    if (a1 >= a0)
        cairo_arc(mCr, cx, cy, radius, a0, a1);
    else
        cairo_arc_negative(mCr, cx, cy, radius, a0, a1);
    cairo_stroke(mCr);
    setPenSize(1.0f);
}

//------------------------------------------------------------------------
void Canvas::drawImage(cairo_surface_t *image, const Rect &dest)
{
    if (!image || dest.w <= 0.0f || dest.h <= 0.0f)
        return;
    const int iw = cairo_image_surface_get_width(image);
    const int ih = cairo_image_surface_get_height(image);
    if (iw <= 0 || ih <= 0)
        return;

    cairo_save(mCr);
    cairo_translate(mCr, dest.x, dest.y);
    cairo_scale(mCr, dest.w / static_cast<double>(iw), dest.h / static_cast<double>(ih));
    cairo_set_source_surface(mCr, image, 0.0, 0.0);
    // GOOD filtering: the raster layers are imported at @2x and drawn down to
    // 1x, so the downscale quality is visible on every panel edge.
    cairo_pattern_set_filter(cairo_get_source(mCr), CAIRO_FILTER_GOOD);
    cairo_rectangle(mCr, 0.0, 0.0, iw, ih);
    cairo_fill(mCr);
    cairo_restore(mCr);
}

void Canvas::drawImageCentered(cairo_surface_t *image, float cx, float cy, uint8_t alpha)
{
    if (!image)
        return;
    const float w = static_cast<float>(cairo_image_surface_get_width(image));
    const float h = static_cast<float>(cairo_image_surface_get_height(image));
    const Rect dest(cx - w * 0.5f, cy - h * 0.5f, w, h);
    if (alpha >= 255) {
        drawImage(image, dest);
        return;
    }
    // cairo_paint_with_alpha rather than a second drawImage path: it multiplies
    // the source's own alpha, so a partly transparent PNG or SVG stays correctly
    // shaped instead of picking up a rectangular wash.
    cairo_save(mCr);
    cairo_translate(mCr, dest.x, dest.y);
    cairo_set_source_surface(mCr, image, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(mCr), CAIRO_FILTER_GOOD);
    cairo_rectangle(mCr, 0.0, 0.0, w, h);
    cairo_clip(mCr);
    cairo_paint_with_alpha(mCr, alpha / 255.0);
    cairo_restore(mCr);
}

void Canvas::drawImageRotated(cairo_surface_t *image, const Rect &dest, double angleDeg)
{
    if (!image || dest.w <= 0.0f || dest.h <= 0.0f)
        return;
    const int iw = cairo_image_surface_get_width(image);
    const int ih = cairo_image_surface_get_height(image);
    if (iw <= 0 || ih <= 0)
        return;

    cairo_save(mCr);
    // Pivot about the destination centre: move there, rotate, then step back by
    // half the destination so the image lands centred on the pivot.
    cairo_translate(mCr, dest.centerX(), dest.centerY());
    // Cairo rotates clockwise for a positive angle in its default y-down space,
    // which already matches the knob convention — no sign flip.
    cairo_rotate(mCr, angleDeg * 3.14159265358979323846 / 180.0);
    cairo_translate(mCr, -dest.w * 0.5, -dest.h * 0.5);
    cairo_scale(mCr, dest.w / static_cast<double>(iw), dest.h / static_cast<double>(ih));
    cairo_set_source_surface(mCr, image, 0.0, 0.0);
    // GOOD, not BILINEAR. The dial art is stored several times its on-screen
    // size, and bilinear samples only 2x2 texels: at that minification the gold
    // pointer -- a few source pixels wide -- washes out to nothing.
    cairo_pattern_set_filter(cairo_get_source(mCr), CAIRO_FILTER_GOOD);
    cairo_rectangle(mCr, 0.0, 0.0, iw, ih);
    cairo_fill(mCr);
    cairo_restore(mCr);
}

//------------------------------------------------------------------------
void Canvas::applyFont() const
{
    if (mFonts) {
        cairo_font_face_t *face = (mFont == Font::Title) ? mFonts->title() : mFonts->body();
        if (face)
            cairo_set_font_face(mCr, face);
    }
    cairo_set_font_size(mCr, mFontSize);
}

void Canvas::setFont(Font f)
{
    mFont = f;
    applyFont();
}

void Canvas::setFontSize(float px)
{
    mFontSize = px;
    cairo_set_font_size(mCr, px);
}

void Canvas::drawString(const char *text, float x, float y)
{
    if (!text || !*text)
        return;
    cairo_move_to(mCr, x, y);
    cairo_show_text(mCr, text);
    cairo_new_path(mCr); // show_text leaves the current point set
}

float Canvas::stringWidth(const char *text) const
{
    if (!text || !*text)
        return 0.0f;
    cairo_text_extents_t ext;
    cairo_text_extents(mCr, text, &ext);
    // x_advance, not width: this is where the next glyph would start, which is
    // what centring and truncation need.
    return static_cast<float>(ext.x_advance);
}

float Canvas::stringDescent(const char *text) const
{
    if (!text || !*text)
        return 0.0f;
    cairo_text_extents_t ext;
    cairo_text_extents(mCr, text, &ext);
    // y_bearing is the ink box's top relative to the baseline and is NEGATIVE above it, so the
    // ink's bottom edge is y_bearing + height. A string with no descender gives a value at or
    // below zero, which is clamped away: it has nothing under the baseline to clear.
    const double below = ext.y_bearing + ext.height;
    return below > 0.0 ? static_cast<float>(below) : 0.0f;
}

float Canvas::stringAscent(const char *text) const
{
    if (!text || !*text)
        return 0.0f;
    cairo_text_extents_t ext;
    cairo_text_extents(mCr, text, &ext);
    // y_bearing is the ink box's top relative to the baseline and is NEGATIVE above it, so the
    // height above the baseline is its negation. A string whose ink is entirely below the
    // baseline would give a negative value, which is clamped away.
    return ext.y_bearing < 0.0 ? static_cast<float>(-ext.y_bearing) : 0.0f;
}

std::string Canvas::clipToWidth(const std::string &s, float maxW) const
{
    if (stringWidth(s.c_str()) <= maxW)
        return s;

    // Trim whole UTF-8 characters off the end until the string plus an
    // ellipsis fits. Cutting mid-sequence would emit a replacement glyph.
    std::string cut = s;
    while (!cut.empty()) {
        // Step back over one UTF-8 character.
        size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80)
            --n;
        cut.resize(n);
        if (cut.empty())
            break;
        const std::string candidate = cut + "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
        if (stringWidth(candidate.c_str()) <= maxW)
            return candidate;
    }
    return std::string();
}

// Elide the middle, keeping both ends, so a name's distinguishing suffix survives. See the
// declaration for why this exists beside clipToWidth rather than replacing it.
//
// The tail gets the larger share of what is left after the ellipsis, because it is the end that
// identifies the file; the head is kept at all because a name with no head reads as a fragment
// rather than as a shortened name.
//
// Both halves are grown a WHOLE UTF-8 character at a time -- cutting mid-sequence would emit a
// replacement glyph -- and the result is then re-measured and trimmed, because the width of a
// concatenation is not the sum of its parts: kerning and hinting mean head + ellipsis + tail can
// come out a little wider than the three measured separately.
std::string Canvas::elideMiddle(const std::string &s, float maxW) const
{
    if (stringWidth(s.c_str()) <= maxW)
        return s;

    static const char *kEllipsis = "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
    const float ellW = stringWidth(kEllipsis);
    // No room for the ellipsis itself, let alone anything either side of it: fall back rather than
    // return something that would overrun the row it was measured for.
    if (maxW <= ellW)
        return clipToWidth(s, maxW);

    const float budget = maxW - ellW;
    const float headBudget = budget * 0.40f;

    // Grow the head forward over whole characters.
    size_t head = 0;
    while (head < s.size()) {
        size_t n = head + 1;
        while (n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            ++n;
        if (stringWidth(s.substr(0, n).c_str()) > headBudget)
            break;
        head = n;
    }

    // Grow the tail backward over whole characters, spending whatever the head did not.
    const float tailBudget = budget - stringWidth(s.substr(0, head).c_str());
    size_t tail = s.size();
    while (tail > head) {
        size_t n = tail - 1;
        while (n > head && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        if (stringWidth(s.substr(n).c_str()) > tailBudget)
            break;
        tail = n;
    }

    // Nothing was kept at either end: the width is too small for this approach to say anything,
    // so let the plain truncation answer instead of returning a bare ellipsis.
    if (head == 0 && tail == s.size())
        return clipToWidth(s, maxW);

    std::string out = s.substr(0, head) + kEllipsis + s.substr(tail);
    // Re-measure, and give back characters from the HEAD if the join came out wide -- the tail is
    // the half worth keeping.
    while (stringWidth(out.c_str()) > maxW && head > 0) {
        size_t n = head - 1;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
            --n;
        head = n;
        out = s.substr(0, head) + kEllipsis + s.substr(tail);
    }
    // Still too wide with no head left at all, which means the tail alone does not fit: the plain
    // truncation is the only thing that can be guaranteed to.
    if (stringWidth(out.c_str()) > maxW)
        return clipToWidth(s, maxW);
    return out;
}

//------------------------------------------------------------------------
void Canvas::pushClip(const Rect &r)
{
    cairo_save(mCr);
    cairo_rectangle(mCr, r.x, r.y, r.w, r.h);
    cairo_clip(mCr);
}

void Canvas::popClip()
{
    cairo_restore(mCr);
    // cairo_restore also rolls back the font selection made before the save.
    applyFont();
}

} // namespace Rations
