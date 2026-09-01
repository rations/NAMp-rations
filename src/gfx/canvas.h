// Canvas — the drawing surface the editor paints through.
//
// This is a thin, deliberately small wrapper over Cairo that exposes exactly
// the primitives the panel needs, named so the panel code reads the same way
// it does in the sibling Haiku port (which paints through a BView). Keeping
// the surface this narrow is what makes the two implementations comparable
// line for line.
//
// Two conventions are pinned down HERE, once, so no drawing code has to think
// about them:
//
//   * Rect is {x, y, w, h} with EXCLUSIVE edges — a Rect{10, 75, 30, 200}
//     covers x in [10, 40) and y in [75, 275), i.e. exactly 30x200 pixels.
//     This matches the original plug-in's IRECT semantics and the constants in
//     geometry.h. (Be's BRect is inclusive on both edges, so a rect built
//     as BRect(x, y, x+w, y+h) there is one pixel wider and taller than the
//     geometry says. Use Rect::fromLTRB() when porting such a construction so
//     the extra pixel does not silently come along.)
//
//   * Arc angles use the KNOB convention: degrees, 0 = straight up, positive =
//     clockwise. strokeArc() converts to Cairo's radians-clockwise-from-east
//     internally.
//
// Colours are 0xRRGGBB (as in geometry.h) plus an optional 0..255 alpha.

#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <string>

namespace Rations
{

class FontStack;

//------------------------------------------------------------------------
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    constexpr Rect() = default;
    constexpr Rect(float ax, float ay, float aw, float ah) : x(ax), y(ay), w(aw), h(ah)
    {
    }

    // Build from left/top/right/bottom with EXCLUSIVE right/bottom.
    static constexpr Rect fromLTRB(float l, float t, float r, float b)
    {
        return Rect(l, t, r - l, b - t);
    }

    constexpr float left() const
    {
        return x;
    }
    constexpr float top() const
    {
        return y;
    }
    constexpr float right() const
    {
        return x + w;
    }
    constexpr float bottom() const
    {
        return y + h;
    }
    constexpr float centerX() const
    {
        return x + w * 0.5f;
    }
    constexpr float centerY() const
    {
        return y + h * 0.5f;
    }

    constexpr bool contains(float px, float py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }

    constexpr Rect inset(float d) const
    {
        return Rect(x + d, y + d, w - 2 * d, h - 2 * d);
    }
};

//------------------------------------------------------------------------
// Which of the two bundled faces subsequent text uses. Sizes are set
// separately, mirroring SetFont/SetFontSize.
enum class Font { Body, Title };

//------------------------------------------------------------------------
class Canvas
{
public:
    // Does not take ownership of either argument; both must outlive the Canvas.
    Canvas(cairo_t *cr, const FontStack *fonts, float width, float height);

    Rect bounds() const
    {
        return Rect(0, 0, mWidth, mHeight);
    }
    cairo_t *cr() const
    {
        return mCr;
    }

    //--- state ---------------------------------------------------------
    void setColor(uint32_t rgb, int alpha = 255);
    void setPenSize(float px);

    //--- shapes --------------------------------------------------------
    void fillRect(const Rect &r);
    void fillRoundRect(const Rect &r, float radius);
    void strokeRoundRect(const Rect &r, float radius);
    void strokeLine(float x0, float y0, float x1, float y1);
    // Cubic Bézier from (x0, y0) to (x3, y3) with the two control points between. The rack draws
    // its signal connections with this; a straight line between two node ports reads as a wire
    // crossing the canvas rather than as a cable leaving one socket and entering another.
    void strokeBezier(float x0, float y0, float cx1, float cy1, float cx2, float cy2, float x3,
                      float y3);
    // The common case: a horizontal S-curve from one port to another. The control points are pushed
    // out along x by `slack`, so the curve leaves and arrives horizontally whatever the vertical
    // offset — which is what makes a connection to a node on another row still read as a cable.
    void strokeConnector(float x0, float y0, float x1, float y1, float slack);
    void fillEllipse(const Rect &r);
    void fillEllipse(float cx, float cy, float rx, float ry);
    void strokeEllipse(float cx, float cy, float rx, float ry);

    // Knob convention: degrees, 0 = up, positive = clockwise. Sets the colour
    // and pen size itself and restores the pen size afterwards, matching how
    // the panel calls it.
    void strokeArc(float cx, float cy, float radius, double a0Deg, double a1Deg, float pen,
                   uint32_t rgb);

    //--- images --------------------------------------------------------
    // Scale `image` to fill `dest`, alpha-composited. No-op when null, so a
    // failed art load degrades to whatever was drawn underneath.
    void drawImage(cairo_surface_t *image, const Rect &dest);
    // Draw at its own pixel size, centred on (cx, cy). `alpha` is 0..255 and
    // multiplies the image's own, which is how a control is drawn as disabled
    // without needing a second, greyed-out copy of its art.
    void drawImageCentered(cairo_surface_t *image, float cx, float cy, uint8_t alpha = 255);
    // Scale to fill `dest`, then rotate about the CENTRE of `dest`. Angle in
    // degrees, knob convention: 0 = the art's own orientation, positive =
    // clockwise. This is how knobs are drawn — the dial art has a pointer baked
    // in pointing straight up, so there is nothing to draw on top of it.
    // The art must be square and centred on its own pivot (gui/make_knob.sh
    // guarantees that), or the knob orbits instead of spinning.
    void drawImageRotated(cairo_surface_t *image, const Rect &dest, double angleDeg);

    //--- text ----------------------------------------------------------
    void setFont(Font f);
    void setFontSize(float px);
    // x, y is the baseline origin of the first glyph (as BView::DrawString).
    void drawString(const char *text, float x, float y);
    float stringWidth(const char *text) const;
    // How far the INK of this string falls below its baseline, in logical units. Not the font's
    // nominal descent, which is a property of the face and is the same whether or not the string
    // has a descender in it: this is cairo's ink extent for these actual glyphs, so "Tone" reports
    // 0 and "Depth" reports what its p really costs. The pedalboard's layout audit needs the
    // difference, because a label row's clearance is set by the strings that are in it.
    float stringDescent(const char *text) const;
    // The mirror of stringDescent: how far this string's INK rises above its baseline. Again
    // cairo's ink extent for these actual glyphs and not the face's nominal ascent, so a legend
    // in caps reports its cap height and one with no ascender reports its x-height. The head
    // panel's clearance audit needs it for the same reason the pedalboard's needs the descent:
    // what a row has to clear is the ink that is in it.
    float stringAscent(const char *text) const;
    // Truncate with an ellipsis until it fits maxW at the current font.
    std::string clipToWidth(const std::string &s, float maxW) const;

    //--- clipping ------------------------------------------------------
    void pushClip(const Rect &r);
    void popClip();

private:
    void applyFont() const;

    cairo_t *mCr = nullptr;
    const FontStack *mFonts = nullptr;
    float mWidth = 0, mHeight = 0;
    Font mFont = Font::Body;
    float mFontSize = 12.0f;
};

} // namespace Rations
