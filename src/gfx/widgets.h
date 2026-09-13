// Widgets drawn from code rather than from art.
//
// This header exists for the same reason gfx/palette.h does: two binaries draw the panel — the
// editor and tools/panelrender — and anything they both draw has to be ONE piece of code or the
// art audit is auditing a second implementation. The switches on the faceplate are art (a bat
// lever photographed at two positions), so they need nothing here; the settings page's pill is
// not, and would otherwise have been written twice.

#pragma once

#include "canvas.h"
#include "palette.h"

namespace Rations
{

//------------------------------------------------------------------------
// A pill toggle: a rounded track with a round handle that slides to the end that is on.
//
// The shape is the grandparent's slide switch (/home/human/NAMix, src/namview.cpp:235) and the
// proportions are its: a track twice as wide as it is tall, and a handle inset two units at each
// end, so the handle's diameter is the track's height less four and there is no second constant
// to keep in step with the first. What differs is the colour — NAMix's azure is its own theme,
// and this panel's accent for a thing that is ON is the gold its piping is drawn in.
//
// `available` is the calibration block's own distinction and is drawn rather than merely stated:
// the toggle reports whether the SOUNDING channel's captures carry the recording level it works
// against, so when they do not it goes grey at both ends and the legend beside it dims with it.
// A pill that stayed bright while its legend greyed would be the same mistake the output radios
// made before D7's revision — drawing availability and state on top of each other.
inline void drawPillToggle(Canvas &c, const Rect &pill, bool on, bool available = true)
{
    const float radius = pill.h * 0.5f;
    c.setColor(on && available ? pal::kGold : 0x2A2724);
    c.fillRoundRect(pill, radius);
    c.setColor(available ? pal::kGold : 0x4A4740, on && available ? 255 : 190);
    c.setPenSize(1.0f);
    c.strokeRoundRect(pill, radius);

    const float d = pill.h - 4.0f;
    const float hx = on ? pill.right() - d - 2.0f : pill.left() + 2.0f;
    c.setColor(available ? 0xE8EAEC : 0x9A9EA2);
    c.fillEllipse(Rect(hx, pill.y + 2.0f, d, d));
}

} // namespace Rations
