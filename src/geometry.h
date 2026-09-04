// Rations editor geometry.
//
// The head canvas size and the palette are mirrored from gui/geometry.sh (the
// art pipeline's source of truth); keep the two in sync by hand. The control
// rects are placed against the panel art and against the author's mock rather
// than against a grid: the faceplate rectangle below is the measured inside of
// the gold piping in the amp-head photograph, and every control position on the
// head page was measured out of mock-head.png by connected-component scan
// rather than eyeballed. gui/geometry.sh documents how to re-derive the
// faceplate if the art is ever re-exported.
//
// EVERYTHING HERE IS IN LOGICAL UNITS. The editor applies one cairo_scale(s, s)
// at compose time and divides mouse coordinates by s before hit-testing, so a
// scale factor must never be baked into a constant here.

#pragma once

#include "gfx/palette.h"
#include "rationsids.h"

#include "pluginterfaces/vst/vsttypes.h"

namespace Rations
{
namespace geo
{

// --- Pages ------------------------------------------------------------------
// A page is a VIEW, not a parameter: it is editor-local state and is deliberately
// never persisted or automated, because a host recalling a preset must not also
// recall which panel the user happened to be looking at.
enum class Page { Head, Cabinet, Pedalboard, Settings };
constexpr int kPageCount = 4;

// EACH PAGE HAS ITS OWN LOGICAL CANVAS, and changing page changes the window.
//
// The head is 2.81:1 because that is the shape of an amp head. Nothing else here
// is that shape: the cabinet is 1.70:1, the pedalboard placeholder is a caption,
// and the MIDI settings page is a short list of rows. Drawing all four inside the
// head's letterbox leaves the narrow pages with several hundred pixels of black
// down each side — margin that no amount of layout can fill, because the content
// genuinely is not that wide.
//
// So the page swap asks the host for a new window: the editor keeps its current
// scale, applies it to the incoming page's base size and calls
// IPlugFrame::resizeView(), which is the SDK's documented plug-in-initiated
// resize (pluginterfaces/gui/iplugview.h, "Plug-in requested resize" — the host
// then calls back into IPlugView::onSize()). The scale plumbing itself is
// untouched: still one cairo_scale(s, s) at compose, still mouse divided by s,
// still one constrainSize() rule — that rule now reads its base size from the
// current page instead of from a single constant.
//
// Two consequences, both deliberate:
//   * constrainSize() is page-dependent, so the legal size range changes under
//     the host when the page does. That is what canResize/checkSizeConstraint
//     exist for, and the host is told through the resizeView call rather than
//     being left to discover it.
//   * A host with no IPlugFrame cannot be asked to resize. There the editor
//     keeps the size it has and letterboxes the page inside it, which is the
//     old behaviour and is why every page is still drawn centred on its own
//     canvas rather than pinned to a corner.

// The head page IS the trimmed size of head.png, so at scale 1.0 the panel is a
// pixel-exact blit with no resampling. This one is not free to change.
constexpr int kWinW = 1133;
constexpr int kWinH = 403;

// The cabinet: art aspect 1483/872 = 1.7007, drawn as wide as the page allows
// with the two IR rows underneath it and a back button above.
constexpr int kCabPageW = 640;
constexpr int kCabPageH = 460;

// The pedalboard: five enclosures on two rows, PRE above and POST below.
// 3 * kPedalW + 2 * kPedalGap + 2 * 24 of margin across; the header band the back
// button lives in, then the two rows and their legends, down. Mirrored in
// gui/geometry.sh, which lays the reference render out from the same numbers.
constexpr int kPedalPageW = 662;
constexpr int kPedalPageH = 681;

// Settings: four sections, stacked in one column - the capture loaders, the
// channel levels, MIDI learn, and the output section.
//
// Captures come first because a channel with nothing loaded does nothing, so it
// is the section a new user needs before any other one means anything. Output
// comes last because it is set once when a rig is assembled. In between, levels
// before MIDI, because every user has four channels to balance and only some own
// a footswitch.
//
// 1146 units tall, which is taller than any other page here by a long way, and
// the only page whose window may be shorter than the page itself: it is
// width-locked, free in height, and scrolls. See pageScrolls() below for why
// the smaller scale kSettingsScaleMin gives it did not turn out to be enough on
// its own. It grew by 238 when the pedalboard's five footswitches joined the
// MIDI list, and that cost nothing but scrolling, which is exactly what a page
// that already scrolls is for.
constexpr int kSettingsPageW = 640;
constexpr int kSettingsPageH = 1166;

// The shortest that page's viewport may be dragged to, in logical units: the
// fixed header band that carries the back button (kPageContentTop, 50) plus
// three rows of the shared settings grid (3 * kMidiRowPitch, 120) plus a 20-unit
// margin. Spelled as a literal here because it is needed by pageMinH() long
// before those two constants are declared; the static_assert beside them is what
// stops the two spellings drifting apart.
constexpr int kSettingsMinViewH = 190;

// The height the settings page's window OPENS at, in logical units, and it is
// deliberately not the whole page. A window tall enough to show all 1166 units
// is taller than most screens have room for once the host's own furniture is
// accounted for, and opening at full height hands the user a window they have to
// shrink before the scrollbar this page was given is any use at all. So it opens
// at the top of the MIDI list: the capture loaders and the channel levels whole,
// the "MIDI Learn" heading sitting on the bottom edge saying there is more below.
// The rest is scrolled to, and the window is free to be dragged anywhere between
// kSettingsMinViewH and the full page afterwards — this is an opening size, not a
// constraint, and constrainSize() is still the only thing that says what is legal.
//
// Spelled as a literal for the same reason kSettingsMinViewH is: it is needed
// long before kSettingsHeadingY is declared, and the static_assert beside that
// constant is what stops the two spellings drifting apart.
constexpr int kSettingsDefaultViewH = 524;

struct PageSize {
    int w, h;
};
constexpr PageSize kPageSizes[kPageCount] = {
    {kWinW, kWinH},
    {kCabPageW, kCabPageH},
    {kPedalPageW, kPedalPageH},
    {kSettingsPageW, kSettingsPageH},
};

constexpr PageSize pageSize(Page p)
{
    return kPageSizes[static_cast<int>(p)];
}

// Horizontal centre of a page, for everything that is centred on the canvas
// rather than on the faceplate.
constexpr int pageCX(Page p)
{
    return pageSize(p).w / 2;
}

// Host resize range, as a multiple of whichever page is showing. The art is the
// ceiling on the head page: it is 1133 px wide in the source, so anything above
// 1.0 is a genuine upscale and 1.5 is where a photographic faceplate stops
// holding up. The other pages are drawn rather than photographed and could go
// further, but one range for all four keeps the scale continuous across a page
// change — the window changes shape, never apparent size.
constexpr double kScaleMin = 0.66;
constexpr double kScaleMax = 1.50;

// The settings page gets its own floor, because it is 928 units tall where the
// head is 403 and the same multiple would mean a very different window. At 0.66
// it would be 613 device px, which is already more than the usable height of a
// 768-line laptop screen once the host's own furniture is accounted for; at 0.50
// it is 464.
constexpr double kSettingsScaleMin = 0.50;

constexpr double pageScaleMin(Page p)
{
    return p == Page::Settings ? kSettingsScaleMin : kScaleMin;
}

// --- The one page that scrolls ----------------------------------------------
// A LOWER FLOOR WAS NOT ENOUGH, and that is a measurement rather than a change
// of mind. The floor was written to make the whole page fit in a short window;
// what it could not do is make the whole page fit in a short window ON A HOST
// THAT WILL NOT RESIZE. Every other page is 403 units tall or less, so the
// letterbox path — fit the page in whatever window exists and centre it — always
// had room. This one is 928, and when the fitted scale came out below the floor
// it was clamped UP to the floor, which put mOffY negative and cut the top and
// bottom off the page. The sections at the ends are the capture loaders and the
// output mode: the two a user needs most and the two that went missing.
//
// So the settings page is width-locked and free in height. Its scale comes from
// the window's WIDTH alone, its height may be anything from kSettingsMinViewH
// upward, and whatever does not fit is reached by scrolling.
//
// It stays ONE logical canvas. The scroll is a single translate inside the page
// transform, undone before the chrome is drawn, and undone again on the way back
// for hit-testing — the same shape as the cairo_scale it sits inside, and the
// reason the three things the old comment here worried about each cost one line
// rather than a coordinate system:
//
//   * the browser overlay is drawn in CHROME space, after the scroll is undone,
//     so it never moves and its clicks need no offset. What it does need is a
//     height taken from the viewport instead of from the page, or the card would
//     be taller than the window it is centred in;
//   * a drag reads deltas, and a delta is scroll-invariant as long as the scroll
//     does not move under it, which it cannot: the wheel is consumed by the
//     scrollbar drag while one is in progress;
//   * the wheel keeps its jobs. Over a control that answers it, it still nudges
//     that control; anywhere else on the page it now scrolls. A control has to
//     be under the pointer to be nudged, so the two never both apply.
//
// The back button becomes chrome rather than content on a scrolling page: it is
// the way OUT, and scrolling the exit off the screen is the one thing a scroll
// must never do. kPageContentTop already reserved exactly that band.
constexpr bool pageScrolls(Page p)
{
    return p == Page::Settings;
}

// Rounded, not truncated, so these agree with the sizes the editor actually
// produces — constrainSize() rounds, and it is the single authority on what is
// a legal size.
constexpr int pageMinW(Page p)
{
    return static_cast<int>(pageSize(p).w * pageScaleMin(p) + 0.5);
}
constexpr int pageMinH(Page p)
{
    // A scrolling page's window is allowed to be shorter than the page it shows,
    // so its floor is the shortest VIEWPORT rather than the whole page scaled
    // down. Without this the minimum height would be the one number that made
    // the page unreachable in the first place.
    const int h = pageScrolls(p) ? kSettingsMinViewH : pageSize(p).h;
    return static_cast<int>(h * pageScaleMin(p) + 0.5);
}
constexpr int pageMaxW(Page p)
{
    return static_cast<int>(pageSize(p).w * kScaleMax + 0.5);
}
constexpr int pageMaxH(Page p)
{
    return static_cast<int>(pageSize(p).h * kScaleMax + 0.5);
}

// --- Typography -------------------------------------------------------------
// EVERY PANEL LEGEND IS MICHROMA (Font::Title) — the wordmark's face, and the
// only one on the panel. A silkscreen legend on an amp is one typeface used at
// several sizes, and mixing a second face into the same row of knobs is what
// made the first draft read as a dialog box with pictures of knobs on it.
//
// Roboto (Font::Body) is kept for exactly two things, and they are not legends:
// file names in the IR rows, and the value readout that appears while a dial is
// being dragged. Both are variable-length strings that have to stay legible when
// clipped, which is what a proportional text face is for and what Michroma —
// wide, monoline, all-caps in feel — is not.
//
// The sizes below are MEASURED, not chosen, against two things that
// tools/panelrender.cpp checks and fails the art audit on:
//
//   * WIDTH. Every legend is measured against the space this header gives it,
//     so a size raised here has to be justified rather than eyeballed.
//
//   * CAP EVENNESS. Michroma draws its round glyphs with the normal optical
//     overshoot — at 100 px the O is 77 units tall, the D 75 and the 2 76, so
//     the round ones are a shade taller and read as level. At a small size that
//     2.6 % lands across a pixel boundary: at 13 px the O and the 2 grid-fit to
//     11 rows and the D and the 1 to 10, and "OD2" comes out with a short D.
//     Measured across the caps and digits, Michroma is even at 10, 11, 12,
//     15-18, 20-23 and 26-28 and stepped at 13, 14, 19, 24, 25, 29 and 30, so
//     the legend sizes are drawn from the even list. No hint style avoids this
//     — FULL and SLIGHT step at the same sizes and NONE steps at different ones
//     — and it is a property of the size, not of the string, which is why the
//     audit checks the whole alphabet at each size rather than each legend.
//
// One caveat that cannot be designed away: the editor scales, so a legend at
// 15 logical px is 15*s device px, and an intermediate window size can still
// land on a stepped value. Being even at the sizes the window actually rests at
// is what is available; being even everywhere is not.
constexpr int kTitleSize = 28; // "Rations", under the badge — see the wordmark block
// The dial legends and the utility row's, both one step down the cap-even list
// (15 -> 12 and 11 -> 10; 13 and 14 are not available, see the note above).
// They came down when the channel lamps moved out of the band above the dials
// and the EQ switch moved into it: two legend rows now share a band that used
// to hold one, and the room between them is worth more than the extra point of
// size was. Michroma is a wide face, so a legend at 12 is still wider than the
// Roboto readout at 12 that replaces it while a dial is being dragged.
constexpr int kKnobLabelSize = 12;   // dial legends, main row
constexpr int kIoLabelSize = 12;     // Input / Output; one legend size on the faceplate
constexpr int kToggleLabelSize = 10; // BYPASS, EQ
constexpr int kPageButtonTextSize = 15;
constexpr int kBlendLabelSize = 12;
constexpr int kKnobValueSize = 12;   // Roboto: the drag readout
constexpr int kFileRowTextSize = 12; // Roboto: IR file names

// --- Head page --------------------------------------------------------------
// Inner faceplate, inside the gold piping (measured; see gui/geometry.sh).
// Right/bottom are EXCLUSIVE, matching Rect's convention.
constexpr int kFaceL = 59, kFaceT = 62, kFaceR = 1074, kFaceB = 333;
constexpr int kFaceCX = (kFaceL + kFaceR) / 2; // 566

// Palette (geometry.sh). Defined in gfx/palette.h — which is where things that
// draw with Canvas but know nothing about this editor get it from — and
// re-exported here so every geo::kAccent means what it does there.
using pal::kAccent;
using pal::kAccentBright;
using pal::kBgColor;
using pal::kDimColor;
using pal::kFaceColor;
using pal::kGold;
using pal::kPeakColor;
using pal::kTextColor;

// --- The wordmark: the NAMp badge, with "Rations" under it ------------------
// It was text alone — "Rations" in Michroma at 56, the way the author's other
// plug-in draws its own name — and the project's public identity is now
// NAMp-rations, so the head carries the parent's gold badge with the model name
// beneath it. The badge is the mark; the word is what this amp is called, which
// is why the text came DOWN from 56 to 28 rather than the badge being fitted
// around it.
//
// The two are placed independently rather than as a block plus a gap. A gap
// constant would have to be negative: "NAMp" has a descender and "Rations" does
// not, so the two read as one lock-up only when the word's cap height rises
// past the badge's lower edge. Two absolute positions say that plainly; a gap
// of -6 would not.
//
// THE VERTICAL BAND IS THE BUDGET HERE, not the width — which is the reverse of
// how this band has always worked, and is why nothing else on it had to move.
// Stacked, the block is max(badge, text) wide rather than their sum: 162
// against the 318 the wordmark had, so it is NARROWER than the text it
// replaces and the utility row, the Slim icon and the settings button all keep
// the positions they were measured into. What it spends instead is height,
// between the faceplate's top edge (kFaceT, 62) and the dial legends' ink
// (kKnobCY - kKnobLabelDY - kKnobLabelSize, 153). The asserts below hold both
// ends, and panelrender measures the text for real.
constexpr int kBadgeH = 48;
// The stored art is 512x152 (gui/geometry.sh BADGE_W/H, checked there against
// what the source actually trims to), so the drawn width is that aspect at
// kBadgeH. Written as the arithmetic rather than as 162 so that replacing the
// art and updating geometry.sh cannot leave this silently stretched.
constexpr int kBadgeW = kBadgeH * 512 / 152; // 161
constexpr int kBadgeTop = 70;                // 8 below kFaceT, the meters' own margin
constexpr int kBadgeBottom = kBadgeTop + kBadgeH;
constexpr int kBadgeCX = kFaceCX;
// The word's baseline. Its cap top (kTitleBaselineY - the cap height, ~20 at
// this size) sits ABOVE kBadgeBottom, which is the tuck that makes the two one
// mark: "NAMp"'s p hangs into the space either side of which "Rations" is
// narrow enough to clear.
constexpr int kTitleBaselineY = 132;
constexpr int kWordmarkBottom = kTitleBaselineY; // "Rations" has no descender
static_assert(kBadgeTop > kFaceT, "the badge has reached the top of the faceplate");
// The other end of the band is asserted where the dial legends are defined —
// kKnobCY and kKnobLabelDY come later in this file.

// --- The main dial row (8), evenly spaced -----------------------------------
struct KnobSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy, r;
    const char *label;
    const char *unit; // appended to the drag readout (nullptr = none)
};
// --- The lower stack: dial, switch, lamp, in that order ----------------------
// These three rows are ONE column of stacked parts per channel, and they are
// derived DOWNWARDS from the dial rather than each being a measured number,
// because the dial is the one of the three whose position is fixed by something
// outside this group: the band above it now carries the EQ switch's legend (see
// kEqToggle), and how close the dial legends may come to that is what decides
// where the dial row sits. Everything else follows.
//
// The lamps used to sit ABOVE the dials, at y = 147, in the band between the
// wordmark's row and the dial legends. They moved to the BOTTOM of each column,
// under the switch, so that band could take the EQ switch beside BYPASS — and
// the dial row and the switch row moved up by 24 and 19 to open the space at
// the bottom for them. The Input and Output dials, their legends and the page
// buttons did not move: those are placed against the faceplate's own bottom
// edge and there was nothing wrong with where they were.
//
// One consequence, and it is deliberate: the switch row is no longer level with
// the Input and Output dials. It cannot be and also have a lamp under it, and
// the lamp is what a player looks at from six feet away.
constexpr int kKnobCount = 8;
constexpr int kKnobR = 28;
constexpr int kKnobCY = 203;

constexpr int kToggleW = 24, kToggleH = 40;
constexpr int kKnobToToggleGap = 6;
constexpr int kToggleCY = kKnobCY + kKnobR + kKnobToToggleGap + kToggleH / 2; // 257

// --- Channel indicator LEDs (4), under the switches -------------------------
// Red when the channel is the one sounding, black otherwise. Centred on their
// dials.
//
// Four and not five: the gate's switch and lamp moved UP to the utility row
// beside BYPASS and EQ. That is where a switch not tied to a single dial
// belongs — it is the argument kEqToggle already makes for the tone stack — and
// it is what freed the band under the Threshold dial for a value readout, which
// is the whole reason the move happened.
constexpr int kLedR = 9;
constexpr int kToggleToLedGap = 6;
constexpr int kLedCY = kToggleCY + kToggleH / 2 + kToggleToLedGap + kLedR; // 292
constexpr int kLedCount = 4;

// The gaps are named rather than folded into literal centres so the arithmetic
// is checkable and so a part that grows fails the static_asserts below instead
// of silently overlapping. Both are 6, and the DRAWN clearances are not equal:
// dial.png's ink fills its 256x256 box edge to edge, while switch_up_ring.png's
// ink starts 8 rows into a 184-row frame — 1.7 units once it is drawn at 40 —
// so the dial-to-switch gap reads as 7.7 with the bat up and 6 with it down,
// which is the bat travelling and is what it should look like.
constexpr int kKnobPitch = 94;
constexpr int kKnobX0 = 251;

constexpr KnobSpec kKnobs[kKnobCount] = {
    // The four channels, in Channel order. Each dial sweeps its own channel's
    // bank; only the selected channel is heard, but all four dials stay live so
    // a channel can be set up before it is switched to.
    {kCleanGainId, kKnobX0 + 0 * kKnobPitch, kKnobCY, kKnobR, "Clean", nullptr},
    {kCrunchGainId, kKnobX0 + 1 * kKnobPitch, kKnobCY, kKnobR, "Crunch", nullptr},
    {kOd1GainId, kKnobX0 + 2 * kKnobPitch, kKnobCY, kKnobR, "OD1", nullptr},
    {kOd2GainId, kKnobX0 + 3 * kKnobPitch, kKnobCY, kKnobR, "OD2", nullptr},
    // Shared section. The gate has its own bat switch below it like the four
    // channels do; Bass/Middle/Treble share ONE switch and it is not here — it
    // is EQ, up in the utility band beside BYPASS, because a switch that takes
    // three dials out at once belongs with the other whole-signal-path switch
    // and not under whichever of the three it happened to be drawn beneath.
    // "Threshold" and not "Gate", which is what this dial actually is and what
    // the parent plug-in silkscreens: the dial sets a threshold in dB, and the
    // thing called GATE is the switch up in the utility row that takes it in and
    // out. Michroma at kKnobLabelSize measures 76 units against the 86 the art
    // audit allows, so the longer word costs nothing.
    {kNoiseGateThresholdId, kKnobX0 + 4 * kKnobPitch, kKnobCY, kKnobR, "Threshold", "dB"},
    {kBassId, kKnobX0 + 5 * kKnobPitch, kKnobCY, kKnobR, "Bass", nullptr},
    {kMiddleId, kKnobX0 + 6 * kKnobPitch, kKnobCY, kKnobR, "Middle", nullptr},
    {kTrebleId, kKnobX0 + 7 * kKnobPitch, kKnobCY, kKnobR, "Treble", nullptr},
};

// The dial art has a gold pointer baked in, pointing straight up at the middle
// of its travel, so a knob is drawn by rotating the bitmap — there is no arc and
// no code-drawn notch (both would fight the pointer).
constexpr double kKnobSweepDeg = 270.0;

// Label baseline ABOVE the face centre (the mock puts all eight on y = 181).
//
// The legend row. What sits under a dial depends on which dial it is, and the
// split is forced by geometry rather than chosen:
//
//   - The four CHANNEL dials have their bat switch 6 units below them and the
//     channel lamp below that, so there is no second text row to give them. A
//     value row underneath would be drawn across the levers — confirmed by
//     looking at it, because the first version of this editor did exactly that.
//     Their readout therefore still appears HERE, replacing the legend while
//     that dial is dragged, and what it says is a capture name rather than a
//     number.
//   - Threshold, Bass, Middle and Treble have nothing under them since the gate
//     switch moved to the utility row, so they get a permanent value row at
//     kKnobValueDY and their legend never disappears. That is the parent
//     plug-in's arrangement and the reason to prefer it is the obvious one: a
//     player can read the amp without touching it.
//
// 38 rather than the 45 the mock measured, because the row above the dials is
// no longer empty: the EQ switch's legend ends at y = 139 and the dial art now
// begins at y = 174, so the legend is centred in what is left instead of being
// pushed up against the switch's. The static_assert below is what holds that,
// and the smaller legend size is the other half of the same change.
constexpr int kKnobLabelDY = 38; // 203 - 165

// Value baseline BELOW the face centre, for the four dials that have room for
// one. 44 is the parent plug-in's own number and it means the same thing here:
// 16 units below the dial's edge, since both trees draw a 28-unit radius. The
// row lands at y = 247, which clears the page buttons' top edge at 277 by 27 —
// and Bass, Middle and Treble all sit inside those buttons' x span, so that is
// a clearance that had to be checked rather than assumed.
constexpr int kKnobValueDY = 44; // 203 + 44 = 247
static_assert(kKnobValueDY > kKnobR, "a dial's value row is drawn across its own dial");

// --- Bat toggles under the first five dials ---------------------------------
// switch_up_ring.png / switch_down_ring.png are 112x184 and carry no meaning of
// their own; which frame a value selects is decided in the view.
struct ToggleSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy;        // centre of the toggle art = the lever's PIVOT
    int w, h;          // art size; the two rows are NOT the same size, see kTopToggleW
    const char *label; // drawn centred under the art; nullptr = no label
    bool invert;       // true = parameter on means bat DOWN
};
// kToggleW, kToggleH and kToggleCY are declared with the lower stack above,
// because the dial row and the lamp row are derived from them.
constexpr int kToggleLabelDY = 33;
// The CLICK target is deliberately bigger than the art: the lever is 24 px wide
// and at the minimum window scale that is 16 physical px, which is a miss
// waiting to happen. It stops well short of the 94 px pitch so adjacent targets
// can never touch.
constexpr int kToggleHitW = 60;
constexpr int kToggleHitTop = -kToggleH / 2;         // relative to cy
constexpr int kToggleHitBottom = kToggleLabelDY + 5; // just under the label

// The four channel switches select kChannelId rather than each toggling a
// boolean of their own, so exactly one is ever up. The view maps switch i to
// "kChannelId == i"; clicking the one that is already up is a no-op, because a
// real amp head has no all-channels-off position.
//
// FOUR, not five. The gate's switch used to be the fifth entry here, under the
// Threshold dial, and it moved to the utility row beside BYPASS and EQ. The
// lower row is now one rule with no exception — a switch and a lamp under each
// channel dial and nothing under any other — and the gate sits with the other
// two switches that are not tied to a single dial. Its behaviour is unchanged:
// an ordinary boolean, deliberately NOT on the MIDI path.
constexpr int kChannelToggleCount = 4;
constexpr int kToggleCount = kChannelToggleCount;
constexpr ToggleSpec kToggles[kToggleCount] = {
    {kChannelId, kKnobs[0].cx, kToggleCY, kToggleW, kToggleH, nullptr, false},
    {kChannelId, kKnobs[1].cx, kToggleCY, kToggleW, kToggleH, nullptr, false},
    {kChannelId, kKnobs[2].cx, kToggleCY, kToggleW, kToggleH, nullptr, false},
    {kChannelId, kKnobs[3].cx, kToggleCY, kToggleW, kToggleH, nullptr, false},
};
static_assert(kLedCount == kChannelToggleCount,
              "there must be exactly one channel lamp per channel switch");

// --- The top band -----------------------------------------------------------
// The band between the faceplate's top edge and the dial legends carries three
// things and they share one centre line: the utility row on the left, the
// wordmark in the middle, and the settings button on the right. It was a single
// number inside the gear's declaration until the gear became a button; it is its
// own constant now because three separate groups read it.
constexpr int kTopBandCY = 106;

// --- Bypass, in the empty band left of the wordmark -------------------------
// NOT IN THE MOCK. The author asked for a bypass toggle after the mock was
// drawn, and this band — between the input meter and the wordmark — is the only
// empty space on the faceplate large enough to hold one without disturbing a
// measured position.
//
// THE MIRROR STILL HOLDS AND IT HAS TURNED ROUND. The author's instruction was
// that this light sit at the same distance from the input meter as the settings
// control sits from the output meter, and since the two meter columns are placed
// symmetrically about kFaceCX (both at kSideDX), reflecting one through that
// centre is exactly that condition. It used to be written the other way — the
// gear was the measured one and this lamp was 2 * kFaceCX - kGearCX — and the
// dependency reversed when the gear became a button, because the utility row
// grew rightwards off this lamp and is now the fixed half of the pair while the
// button's width is the part free to move. So the number is measured here and
// kSettingsButtonRight is the reflection; see there, and panelrender checks it.
constexpr int kBypassLedCX = 151;
constexpr int kBypassLedCY = kTopBandCY;
// --- The utility row: three (lamp, switch) pairs ----------------------------
// BYPASS, EQ and GATE. These are the faceplate's whole-signal-path switches —
// one takes the plug-in out of circuit, one takes the tone stack out, one takes
// the noise gate out — so none of them belongs under a single dial and they
// share a row. GATE's did sit under the Threshold dial and moved here, which is
// what freed that dial's value row.
//
// The row is ONE repeating unit: lamp, then its switch, then the next lamp,
// then its switch. Every lamp is on the same side of the switch it reports, so
// there is no side to learn, and the first pair's lamp is the one whose place
// was not this row's to choose (see kBypassLedCX above), so the whole row is
// laid out from it rightwards.
//
// The parts are SMALLER than the channel switches below — 20x34 against 24x40,
// with a 7-unit lamp against 9 — and that is the row saying what it is. These
// three are set when a rig is put together; the four below them are played. The
// sizes are the parent plug-in's utility row, which is where this row's whole
// shape comes from, and so is kTopPairPitch.
constexpr int kTopToggleW = 20, kTopToggleH = 34;
constexpr int kTopLedR = 7;
// Lamp to its own switch, and pair to pair. What makes the row readable is the
// RATIO of the two: 20 within a pair against 36 between them, so a lamp is
// nearly twice as close to the switch it belongs to as to the one before it,
// and proximity says which is which without a rule having to be learned.
constexpr int kTopLedToToggleDX = 20;
constexpr int kTopPairPitch = 56;
static_assert(kTopPairPitch - kTopLedToToggleDX > kTopLedToToggleDX + kTopLedR,
              "a utility lamp is no nearer its own switch than the previous one");

constexpr int kBypassToggleCX = kBypassLedCX + kTopLedToToggleDX; // 171
constexpr int kBypassToggleCY = kBypassLedCY;
// Kept under its old name because a dozen places refer to the EQ switch on its
// own. It is no longer kKnobX0: aligning it with the Clean dial's column was
// what spread this row across 250 units, and the row reads as one group of
// three rather than as three things scattered along the band.
constexpr int kEqToggleCX = kBypassToggleCX + kTopPairPitch; // 227
constexpr int kGateToggleCX = kEqToggleCX + kTopPairPitch;   // 283

// The click target, sized from the pitch rather than from the art, exactly as
// the parent plug-in sizes its own: pitch less a gap, so adjacent targets never
// touch and the whole row is coverable by the pointer.
constexpr int kTopToggleHitW = kTopPairPitch - 6;     // 50
constexpr int kTopToggleHitTop = -kTopToggleH / 2;    // relative to cy
constexpr int kTopToggleHitBottom = kToggleHitBottom; // still just under the label

constexpr int kTopToggleCount = 3;
constexpr ToggleSpec kTopToggles[kTopToggleCount] = {
    // Bypass on means the plug-in is OUT of circuit, which on an amp is the bat
    // down. EQ and GATE read the natural way round: on is up.
    {kBypassId, kBypassToggleCX, kBypassToggleCY, kTopToggleW, kTopToggleH, "BYPASS", true},
    {kToneStackOnId, kEqToggleCX, kBypassToggleCY, kTopToggleW, kTopToggleH, "EQ", false},
    {kNoiseGateOnId, kGateToggleCX, kBypassToggleCY, kTopToggleW, kTopToggleH, "GATE", false},
};
// Named because the bypass switch is referred to on its own in a dozen places
// that predate the pair.
// `inline` is load-bearing: a constexpr REFERENCE has external linkage, unlike a
// constexpr object, so without it every translation unit that includes this
// header defines the same symbol and the link fails.
inline constexpr const ToggleSpec &kBypassToggle = kTopToggles[0];

constexpr int kTopLedCX[kTopToggleCount] = {kBypassLedCX, kBypassLedCX + kTopPairPitch,
                                            kBypassLedCX + 2 * kTopPairPitch}; // 151, 207, 263
constexpr int kTopLedCY = kBypassLedCY;

// A lamp falls INSIDE its own switch's click target, and that is deliberate
// rather than tolerated: at 20 units apart the two read as one control, so a
// click on the lamp toggling the switch beside it is what anyone would expect.
// What must NOT happen is a lamp reaching back into the PREVIOUS switch's
// target, because then one pair is stealing another's clicks — that is the
// clearance worth asserting, and it is the tight one.
static_assert(kTopLedCX[1] - kTopLedR > kBypassToggleCX + kTopToggleHitW / 2,
              "the EQ lamp is inside the BYPASS switch's click target");
static_assert(kTopLedCX[2] - kTopLedR > kEqToggleCX + kTopToggleHitW / 2,
              "the GATE lamp is inside the EQ switch's click target");
static_assert(kEqToggleCX - kTopToggleHitW / 2 > kBypassToggleCX + kTopToggleHitW / 2,
              "the EQ and BYPASS click targets overlap — one would swallow the other's clicks");
static_assert(kGateToggleCX - kTopToggleHitW / 2 > kEqToggleCX + kTopToggleHitW / 2,
              "the GATE and EQ click targets overlap — one would swallow the other's clicks");
// The utility row's legends sit directly above the dial legends. Measured with
// the font size as a conservative stand-in for the cap height, which is about
// three quarters of it, so real ink has more clearance than this asks for.
static_assert(kBypassToggleCY + kToggleLabelDY + 4 <= kKnobCY - kKnobLabelDY - kKnobLabelSize,
              "the utility row's legends have come down onto the dial legends");
// The wordmark's other end, deferred from its own block above: "Rations" sits
// under the badge and is the lowest ink the title carries, and the dial legends
// are what it would reach first.
static_assert(kWordmarkBottom < kKnobCY - kKnobLabelDY - kKnobLabelSize,
              "the wordmark has come down onto the dial legends");
static_assert(kBypassToggleCY + kTopToggleH / 2 <
                  kBypassToggleCY + kToggleLabelDY - kToggleLabelSize,
              "the utility row's legends are drawn on their own switches");
// The title's leftmost ink. This used to be a hand-measured number — the width
// of "Rations" at 56 — because the title was text and text is only measurable at
// draw time. It is DERIVED now: the badge is the wider of the two things stacked
// here (161 against the word's 141 at kTitleSize), so the block's edge is the
// badge's edge and arithmetic can say where it is. panelrender still measures
// the text for real, which is what catches the case where a larger kTitleSize
// makes the word the wider one and this stops being the block's edge.
constexpr int kWordmarkInkLeft = kFaceCX - kBadgeW / 2; // 486
static_assert(kGateToggleCX + kTopToggleHitW / 2 < kWordmarkInkLeft,
              "the utility row has grown into the wordmark");
// The wordmark's ink ends the same distance the other side of the faceplate
// centre, and the corner cluster is measured against it below.
constexpr int kWordmarkInkRight = 2 * kFaceCX - kWordmarkInkLeft; // 646

// --- The settings button, top-right of the faceplate ------------------------
// A LABELLED BUTTON, NOT AN ICON, and that is the whole of this decision. It was
// a 22-unit gear in the corner, and loading captures — which is the first thing
// anyone must do with this plug-in, since it ships none (D11) — lives behind it
// along with the MIDI rows, the channel trims and the output section. New users
// did not find it. An icon is a good handle for someone on their tenth session
// and a poor one for someone on their first, so the three things behind it are
// written on it and the icon is gone.
//
// It stays in the CORNER rather than joining Pedalboard and Cabinet in the
// bottom row: nothing 200 units wide is free there (the Output dial's column
// ends that row), and the corner is where the hand of everyone who already has
// the plug-in installed goes. Style, size and behaviour are the page buttons'
// exactly — same rounded gold-edged plate, same height, same drawButton — so it
// reads as a third destination and not as a fourth kind of control.
struct ButtonSpec {
    int x, y, w, h;
    const char *label;
    Page target;
};
constexpr int kPageButtonH = 30;
// One step down the cap-even list from the page buttons' 15 (see the typography
// note: 13 and 14 are not available). The legend is 24 characters against
// "Pedalboard"'s ten, and Michroma is a wide face — at 15 it is 236 units, which
// does not fit between the wordmark and the output meter with the Slim icon
// beside it. At 11 it is 173.
constexpr int kSettingsButtonTextSize = 11;
constexpr const char *kSettingsButtonLabel = "Captures, MIDI, Settings";
constexpr int kSettingsButtonW = 203;
constexpr int kSettingsButtonH = kPageButtonH;
// The RIGHT EDGE is the mirror of the utility row's left edge — the bypass
// lamp's — through the faceplate centre, which is the same symmetry the gear
// used to carry and is described at kBypassLedCX. The two clusters therefore
// begin and end at the same distance from their own meter column.
constexpr int kSettingsButtonRight = 2 * kFaceCX - (kBypassLedCX - kTopLedR); // 988
constexpr int kSettingsButtonX = kSettingsButtonRight - kSettingsButtonW;     // 785
constexpr int kSettingsButtonY = kTopBandCY - kSettingsButtonH / 2;           // 91
static_assert(kSettingsButtonX > kWordmarkInkRight,
              "the settings button has grown into the wordmark");
constexpr ButtonSpec kSettingsButton = {kSettingsButtonX, kSettingsButtonY,     kSettingsButtonW,
                                        kSettingsButtonH, kSettingsButtonLabel, Page::Settings};
// Opens an overlay rather than a page: it is one knob, and a page for one knob
// would be a window change and a back button to reach a control most users set
// once and never touch. The file browser is the pattern it follows.
//
// It is placed off the SETTINGS BUTTON rather than off the faceplate centre:
// the two are one cluster in the corner, and what matters is the gap between
// them, not where either lands relative to anything on the left. It sat off the
// gear on the same rule and moved left with it when the gear became a wider
// button. The document is 128x64, so a height of 20 draws 40 wide through
// getByHeight.
constexpr int kSlimToButtonGap = 12;
// The gear's own height, unchanged: the icon did not become a button and there
// is no longer anything in the corner for it to be a matched pair WITH, so the
// size that was measured stays the size it is.
constexpr int kSlimIconH = 22;
constexpr int kSlimIconW = 2 * kSlimIconH; // the .svg's own 2:1
constexpr int kSlimIconCX = kSettingsButtonX - kSlimToButtonGap - kSlimIconW / 2; // 751
constexpr int kSlimIconCY = kTopBandCY;
static_assert(kSlimIconCX + kSlimIconW / 2 + 4 < kSettingsButtonX,
              "the Slim icon has reached the settings button's click target");
// The corner's leftmost ink. The band it sits in is the wordmark's, and the
// wordmark is drawn as measured text, so this is the clearance that says the two
// do not meet — the assert is the coarse half and panelrender's ink audit, which
// measures "Rations" at kTitleSize, is the one that decides.
static_assert(kSlimIconCX - kSlimIconW / 2 - 4 > kWordmarkInkRight,
              "the Slim icon has reached the wordmark");

// The overlay: a card on the head page, centred on the faceplate, with one dial
// on it. Sized to the dial plus its title and readout rather than to a fraction
// of the window, because it holds exactly one control and a bigger card would
// only be more black.
constexpr int kSlimKnobR = 42;
constexpr int kSlimOverlayW = 220, kSlimOverlayH = 212;
constexpr int kSlimOverlayX = kFaceCX - kSlimOverlayW / 2;
constexpr int kSlimOverlayY = (kWinH - kSlimOverlayH) / 2;
constexpr int kSlimKnobCX = kFaceCX;
constexpr int kSlimKnobCY = kSlimOverlayY + 96;
constexpr int kSlimTitleSize = 18;
constexpr int kSlimTitleBaselineY = kSlimOverlayY + 34;
constexpr int kSlimValueBaselineY = kSlimKnobCY + kSlimKnobR + 24;
constexpr int kSlimHintSize = 11;
constexpr int kSlimHintBaselineY = kSlimOverlayY + kSlimOverlayH - 18;
inline constexpr const char *kSlimTitle = "Slim";
// What the control is FOR, in the one place a user meets it. "Model size" and
// not "slimmable container variant": the panel says what it buys, not what it
// is called in the file format.
inline constexpr const char *kSlimHint = "Smaller model, less CPU";
static_assert(kSlimValueBaselineY + kKnobValueSize < kSlimHintBaselineY - kSlimHintSize,
              "the Slim overlay's readout and hint have run together");
static_assert(kSlimKnobCY - kSlimKnobR > kSlimTitleBaselineY,
              "the Slim overlay's dial is drawn on its own title");
// The utility row's legends sit directly above the dial legends. Measured with
// the font size as a conservative stand-in for the cap height, which is about
// three quarters of it, so real ink has more clearance than this asks for.
static_assert(kBypassToggleCY + kToggleLabelDY + 4 <= kKnobCY - kKnobLabelDY - kKnobLabelSize,
              "the utility row's legends have come down onto the dial legends");
// The lower stack, top to bottom: dial art, switch art, lamp. Derived, so these
// can only fail if a part grows — and the last one is the one that is not
// derived and so is the one that can actually be broken: the lamp is the lowest
// thing on the faceplate and there is nothing under it but the piping.
static_assert(kKnobCY + kKnobR < kToggleCY - kToggleH / 2,
              "the bat switches are drawn on the dials");
static_assert(kToggleCY + kToggleH / 2 < kLedCY - kLedR,
              "the channel lamps are drawn on the bat switches");
static_assert(kLedCY + kLedR < kFaceB - 8, "the channel lamps have reached the faceplate's edge");
static_assert(kKnobLabelDY > kKnobR, "a dial legend is drawn across its own dial");

// --- Level meters and the Input / Output dials, on the outer edges ----------
// One column per side, shared by the meter, the dial under it and the dial's
// label. The mock's two columns are 467 and 473 px from the faceplate centre —
// 6 px of hand placement — so they are made symmetric at the mean.
constexpr int kSideDX = 470;
constexpr int kSideCXL = kFaceCX - kSideDX; // 96
constexpr int kSideCXR = kFaceCX + kSideDX; // 1036

struct MeterRect {
    Steinberg::Vst::ParamID id;
    int x, y, w, h;
    const char *label;
};
// The meters moved UP by 12 (they were at y = 89) and did not change size: the
// Input and Output dials needed a legend above them as well as a value below,
// and the 17 units between the meter's bottom edge and the dial's top were not
// a text row. Raising them costs nothing — there is nothing else in these two
// columns above the dial — and it keeps the meter art at its measured size,
// which shortening it would not.
constexpr int kMeterW = 27, kMeterH = 146, kMeterY = 77;
static_assert(kMeterY > kFaceT + 8, "the meters have reached the top of the faceplate");
constexpr MeterRect kInputMeter = {kInputMeterId, kSideCXL - kMeterW / 2, kMeterY, kMeterW, kMeterH,
                                   nullptr};
constexpr MeterRect kOutputMeter = {
    kOutputMeterId, kSideCXR - kMeterW / 2, kMeterY, kMeterW, kMeterH, nullptr};
// The settings button ends in the gap between the wordmark and this column, and
// that gap is the whole of the room it has; asserted here because this is where
// the number it has to clear is declared.
static_assert(kSettingsButtonRight + 8 <= kOutputMeter.x,
              "the settings button has reached the output meter");

// The Input and Output dials sit UNDER their meters, which is what the author
// asked for and what puts each level control beside the thing that reports it.
// Smaller than the main row (the mock draws them at 46 px, not 56).
constexpr int kIoKnobR = 23;
constexpr int kIoKnobCY = 275;
// These two dials are the only ones on the faceplate whose legend used to sit
// BELOW them, at 312, which was the whole of the text they had. They now read
// the same way round as the other four — legend above, value below — so the
// legend moved above the dial and the value took the row it vacated.
//
// Above and not below-the-legend: 312 plus a second row lands 4 units off the
// faceplate's bottom bezel, which reads as cramped and is the sort of thing the
// ink audit passes and a person does not.
constexpr int kIoLabelBaselineY = 244;
constexpr int kIoValueBaselineY = 312;
// The legend's descender must clear the dial's own art, and the raised meter
// must clear the legend's cap. Michroma's cap height is about three quarters of
// its size and it descends about a quarter, so both are checked conservatively
// against the size itself and real ink has more room than this asks for —
// panelrender's ink audit is what measures the ink.
static_assert(kIoLabelBaselineY + kIoLabelSize / 4 < kIoKnobCY - kIoKnobR,
              "the Input/Output legends are drawn on their own dials");
static_assert(kMeterY + kMeterH < kIoLabelBaselineY - kIoLabelSize,
              "the meters have come down onto the Input/Output legends");
static_assert(kIoValueBaselineY > kIoKnobCY + kIoKnobR,
              "the Input/Output value row is drawn on its own dial");
static_assert(kIoValueBaselineY + kKnobValueSize / 4 < kFaceB - 8,
              "the Input/Output value row has reached the faceplate's edge");
constexpr KnobSpec kIoKnobs[2] = {
    {kInputGainId, kSideCXL, kIoKnobCY, kIoKnobR, "Input", "dB"},
    {kOutputGainId, kSideCXR, kIoKnobCY, kIoKnobR, "Output", "dB"},
};

// --- Page buttons, bottom-centre-right of the faceplate ---------------------
// ButtonSpec and kPageButtonH are declared further up, with the settings button:
// that one is drawn in the same style but has to be declared before the Slim
// icon, which hangs off its left edge. The width and the row's y stay here,
// where the row they describe is.
constexpr int kPageButtonW = 147, kPageButtonY = 277;
constexpr int kPageButtonCount = 2;
// The seam between the two buttons is CENTRED ON A DIAL rather than left where
// the mock happened to put it (x 803/813, a 7 px near-miss on Middle, which is
// the kind of almost-aligned that reads as a mistake). Derived from the dial's
// own cx so the pair follows the row if the pitch ever changes.
//
// Middle and not Bass, though Bass is the one this looks like it should line up
// with. The reason was the Gate bat switch: at 147 px wide the Pedalboard button
// would run from 569 to 716 and sit on top of that switch's 597..657 hit box,
// and nothing narrower fixed it either — clearing the switch capped the button
// at 59 px against a 108 px "Pedalboard".
//
// That switch has since moved to the utility row, so the constraint is gone and
// Bass is now free. The seam stays on Middle anyway: it is where it has been,
// nothing is wrong with it, and moving a measured position because a reason
// expired is how a panel drifts. Recorded rather than deleted so the next person
// to look at Bass finds out it is available and that this is a choice.
constexpr int kPageButtonSeamGap = 10;
constexpr int kPageButtonSeamCX = kKnobs[6].cx; // Middle
constexpr ButtonSpec kPageButtons[kPageButtonCount] = {
    {kPageButtonSeamCX - kPageButtonSeamGap / 2 - kPageButtonW, kPageButtonY, kPageButtonW,
     kPageButtonH, "Pedalboard", Page::Pedalboard},
    {kPageButtonSeamCX + kPageButtonSeamGap / 2, kPageButtonY, kPageButtonW, kPageButtonH,
     "Cabinet", Page::Cabinet},
};
// The channel lamps' row and the page buttons' row share a centre line, which is
// what gives the bottom of the faceplate one reading line across its whole
// width now that the switch row has moved off the Input/Output dials. It fell
// out of the stack above rather than being aimed at, and it is asserted rather
// than left as a coincidence so that moving either one is a decision: if the
// lamps are meant to sit somewhere else, delete this.
static_assert(kLedCY == kPageButtonY + kPageButtonH / 2,
              "the channel lamps no longer line up with the page buttons");

// The way back, drawn top-left on every page that is not the head. Its position
// is the same on all three, and it is deliberately clear of every page's own
// content, so the window changing shape underneath it does not move it.
constexpr ButtonSpec kBackButton = {16, 12, 110, 28, "Amp", Page::Head};
// Everything below the back button on a non-head page starts here.
constexpr int kPageContentTop = kBackButton.y + kBackButton.h + 10; // 50

// --- Cabinet page -----------------------------------------------------------
// The cabinet page does NOT wear the amp head's faceplate: it is a picture of a
// different object, so it is drawn on the same dark ground the head's letterbox
// uses — and on a window of its own shape, so the cabinet is the page rather
// than a stamp in the middle of one.
//
// The cabinet is drawn as wide as the page allows, and the two loader rows are
// then sized so the pair spans EXACTLY the cabinet's width. That is what stops
// the page reading as a picture with two unrelated widgets under it: cabinet and
// rows are one column, and the margin around them is even on all four sides.
constexpr int kCabMargin = 22;
constexpr int kCabW = kCabPageW - 2 * kCabMargin;      // 596
constexpr int kCabH = (kCabW * 872 + 1483 / 2) / 1483; // 350, the art's own aspect
constexpr int kCabX = kCabMargin;
constexpr int kCabY = kPageContentTop;

// The Blend dial is drawn OVER the knob painted into the cabinet art, at the
// same place and a shade larger so it covers it rather than sitting beside it.
// The two fractions are measured off the source art (knob centre 739/1483 and
// 634/872 of the trimmed cabinet), so the dial follows the art whatever size the
// cabinet is drawn at.
constexpr float kCabBlendFX = 739.0f / 1483.0f;
constexpr float kCabBlendFY = 634.0f / 872.0f;
constexpr float kCabBlendLabelFY = 703.0f / 872.0f;
constexpr int kBlendCX = kCabX + static_cast<int>(kCabBlendFX * kCabW + 0.5f);
constexpr int kBlendCY = kCabY + static_cast<int>(kCabBlendFY * kCabH + 0.5f);
constexpr int kBlendR = 14;
// The dial is small because the knob painted under it is. The hit box is not:
// it is sized for a finger on a trackpad, not for the art.
constexpr int kBlendHitR = 24;
constexpr int kBlendLabelBaselineY = kCabY + static_cast<int>(kCabBlendLabelFY * kCabH + 0.5f);

// --- IR loader rows ---------------------------------------------------------
// The parent plug-in's row design, unchanged, twice: an icon, a file name, and
// prev/next arrows that step through the loaded IR's own folder. Slot A alone is
// the normal case and must behave exactly as it does there; slot B is opt-in,
// and while it is empty the Blend dial above is inert and drawn disabled.
struct FileRow {
    int x, y, w, h;
    const char *placeholder;
    const char *ext; // browser filter (no dot); empty = directories only
};
constexpr int kFileRowH = 28;
constexpr int kIrRowY = kCabY + kCabH + 12; // 412
constexpr int kIrRowGap = 18;
constexpr int kIrRowW = (kCabW - kIrRowGap) / 2; // 289
constexpr FileRow kIrRowA = {kCabX, kIrRowY, kIrRowW, kFileRowH, "Select IR...", "wav"};
constexpr FileRow kIrRowB = {kCabX + kIrRowW + kIrRowGap, kIrRowY, kIrRowW, kFileRowH,
                             "Select IR (optional)...",   "wav"};
constexpr int kRowIconW = 20; // status icon inset at the left of a row
// How tall that icon is drawn. Here rather than in the view, because the
// settings page's capture rows draw the same icon and the art audit measures
// both — three copies of one number is how two of them drift.
constexpr int kRowIconH = 16;

// Prev/next arrows, as offsets from a row's x so they travel with it.
// The two are 16 apart rather than 20, and the reason is the art rather than
// taste: ArrowLeft.svg draws its chevron across only the middle third of an
// 800x800 viewBox, so at kIrArrowH = 14 each arrow is about 5 px of actual ink
// in a 14 px box, and centre spacing reads as roughly three times its own value.
constexpr int kIrArrowH = 14;
constexpr float kIrArrowPrevCX = 40.0f;
constexpr float kIrArrowNextCX = 56.0f;
constexpr float kIrArrowHitW = 16.0f;
constexpr float kIrTextDX = 72.0f;

// --- Pedalboard page --------------------------------------------------------
// Five pedals built into the plug-in — Boost, Chorus, Flanger, Delay, Reverb —
// on two rows: PRE feeds the amp, POST follows the cabinet. Nothing here hosts
// anyone else's plug-in.
//
// EVERY NUMBER BELOW IS MEASURED, NOT CHOSEN. The five enclosure exports are
// blank — no knobs, no LED, no lettering — and the control positions were
// recovered by differencing pedal-boost-mock.png against pedal-boost-base.png
// pixel by pixel and taking the bounding box of each connected component. Those
// came out symmetric about the body centre to within half a pixel, which is what
// says the mock was laid out on purpose rather than by hand:
//
//   knob 1 / knob 2   centre (135, 130) and (359, 130), diameter 99
//   knob 3            centre (249, 210), diameter 99
//   LED               centre (246.5, 369.5), diameter 26
//   footswitch        centre (246, 484.5), diameter 107
//   name baseline     y 638
//
// in the 494x740 export's own pixels. gui/make_pedals.sh trims that export to
// 468x691 at +13+21, and the art is drawn at kPedalW x kPedalH, so a source
// coordinate becomes a logical one as (v - 13 or 21) * kPedalW / 468. The
// constants below are that arithmetic, rounded, and re-deriving them means
// re-running the difference rather than trusting this comment.
constexpr int kPedalArtW = 468; // gui/geometry.sh PEDAL_SRC_W - the trim decides it
constexpr int kPedalArtH = 691;
constexpr int kPedalW = 190; // drawn 2.46x down from the art, as the cabinet is
constexpr int kPedalH = 281;
static_assert(kPedalH == (kPedalW * kPedalArtH + kPedalArtW / 2) / kPedalArtW,
              "pedal draw size must keep the enclosure art's own aspect");

// The grid. Two pedals on the PRE row centred as a pair, three on POST centred
// as a triple, both about the page centre, one gap between neighbours.
constexpr int kPedalGap = 22;
constexpr int kPedalCX = kPedalPageW / 2; // 331
constexpr int kPedalRow1Y = 70;           // below kPageContentTop plus a legend
constexpr int kPedalRow2Y = 380;
constexpr int kPedalPitch = kPedalW + kPedalGap; // 212
constexpr int kPedalPreCX[2] = {kPedalCX - kPedalPitch / 2, kPedalCX + kPedalPitch / 2};
constexpr int kPedalPostCX[3] = {kPedalCX - kPedalPitch, kPedalCX, kPedalCX + kPedalPitch};
static_assert(kPedalPostCX[0] - kPedalW / 2 == 24, "left margin follows from the grid");
static_assert(kPedalPostCX[2] + kPedalW / 2 == kPedalPageW - 24, "and so does the right");

// Positions WITHIN one pedal, relative to its top-left corner. All five faces
// share them, which is why make_pedals.sh insists all five enclosures trim to
// the same size.
constexpr int kPedalKnobR = 20;
// Column offset from the pedal's own centre line. TWO of them, and the second is forced by the
// legends rather than chosen: a label is centred on its knob, so an outer column at +-45 leaves
// 168 - 140 = 28 units to the face's edge and can therefore carry 46 units of lettering. The
// three-knob faces are inside that - their widest is "Depth" at 45 - and they keep the mock's
// measured spacing. The four-knob faces are not: "Repeats" is 61 and "Manual" is 52, both
// measured, and at +-45 they printed over the border trim. At +-37 an outer column has 64 units,
// which clears the widest of them by 3.
//
// This is the same split as the rows above and rests on the same fact: the mock is a THREE-knob
// pedal whose three legends are Drive, Tone and Level, none of them wider than 37. It never had
// to hold a word like "Repeats", so it never measured a spacing that could.
constexpr int kPedalKnobDX = 45;  // three-knob faces: measured off the mock
constexpr int kPedalKnob4DX = 37; // four-knob faces: set by the widest legend they carry

// THE MOCK IS A THREE-KNOB FACE, so it measures a three-knob face and nothing else. Its two rows
// below are the measured ones; the four-knob pair after them is CHOSEN, because there was never a
// four-knob mock to measure and the enclosure is the same box either way.
//
// The four-knob rows are 60 apart rather than the 52 they were first given, and the eight units
// are a defect being fixed rather than taste. A legend's baseline sits at knob edge +
// kPedalLabelDY, so 44 + 20 + 12 was exactly 96 - 20: the upper row's lettering landed flush on
// the tops of the lower row's dials, and "Depth", "Repeats" and "Decay" then hung 2.1 units of
// descender into them. It was seen on the rendered page before any check caught it, which is why
// panelrender now measures every legend's real glyph ink against whatever sits under it.
constexpr int kPedalKnobRow1Y = 44; // three-knob: measured, the upper pair
constexpr int kPedalKnobMidY = 77;  // three-knob: measured, the centred lower dial
constexpr int kPedalKnob4Row1Y = 40;
constexpr int kPedalKnob4Row2Y = 100;
constexpr int kPedalLabelDY = 12;    // baseline below the knob's lower edge
// Bare board between a legend's INK and whatever is under it. Measured against the real glyph
// extents by panelrender, not assumed: several of these legends carry descenders.
constexpr int kPedalLabelClearance = 4;
// The mock measures the LED at 26 art pixels, which is 5.3 logical units, and this is 7 - the one
// number on the face that deliberately departs from it. The mock is a DESIGN reference, not a
// photograph of an object, and this indicator carries more than the object's does: the silkscreen
// here never dims (see drawPedal), so the lamp is the only thing on the enclosure that says
// whether the pedal is in circuit. At the 0.66 scale floor the measured radius is 3.3 physical
// pixels, which is not enough for the page's only state indicator; 7 is 9.2, which is.
constexpr int kPedalLedR = 7;
// 148, not the 141 the mock measures. The lamp had to move with the rows above it: a four-knob
// face's lower legends now end at 134, and a lamp at 141 leaves them 7 units on a face where the
// Delay also needs its two mini slots on that same line. Seven units lower is still well inside
// the plain band between the dials and the footswitch, and it is one height for all five.
constexpr int kPedalLedY = 148;
constexpr int kPedalSwitchR = 22;
constexpr int kPedalSwitchY = 188;
constexpr int kPedalNameY = 250;      // baseline of the pedal's own name
constexpr int kPedalNameSize = 20;
constexpr int kPedalLabelSize = 11;
constexpr int kPedalJackY = 125;     // where a patch cable meets the enclosure
constexpr int kPedalBodyLeft = 14;   // the body without its jack lugs, so a
constexpr int kPedalBodyRight = 175; // cable starts at the lug and not in space

// THE PRINTABLE FACE, which is NOT the body, and confusing the two is what put a legend on the
// enclosure's edge. kPedalBodyRight above is the outermost opaque PIXEL - the outside of the
// black border trim - and it is the right number for deciding where a patch cable meets the box.
// It is the wrong number for deciding where lettering may go, because between the coloured face
// and that pixel there is a border. Sampled across a scanline of the Delay:
//
//   ..168.5  the blue face          168.5..170.1  black inner border
//   170.5..172.9  a narrow rim      173.4..175.4  black outer border
//
// All five enclosures give exactly 21.1 .. 168.5, symmetric about the centre line, measured at
// five different heights - so this is one number for the whole page and panelrender re-derives it
// from the art rather than trusting the two below.
constexpr int kPedalFaceLeft = 22;
constexpr int kPedalFaceRight = 168;
constexpr int kPedalFaceW = kPedalFaceRight - kPedalFaceLeft;

// A four-knob face must not put its lower label row into the LED. This is the
// tightest vertical relationship on the page, so it is asserted rather than
// eyeballed; panelrender re-checks it against the real text metrics.
static_assert(kPedalKnob4Row1Y + kPedalKnobR < kPedalKnob4Row2Y - kPedalKnobR,
              "the four-knob rows are too close for their dials, never mind their labels");
static_assert(kPedalKnob4Row2Y + kPedalKnobR < kPedalLedY - kPedalLedR,
              "four-knob lower dials collide with the LED");
static_assert(kPedalLedY + kPedalLedR < kPedalSwitchY - kPedalSwitchR,
              "LED collides with the footswitch");
static_assert(kPedalSwitchY + kPedalSwitchR < kPedalNameY - kPedalNameSize,
              "footswitch collides with the pedal name");

// The two row legends, in the band above each row.
constexpr int kPedalRowLegendSize = 11;
constexpr int kPedalRowLegendX = 24;
constexpr int kPedalRowLegendDY = 8; // above the row's top edge

// The five, in signal order: two into the amp, three after the cabinet. This is
// the order the audio actually runs in, and the row a pedal is on is a fact about
// the DSP graph rather than a layout choice, so the table carries both and the
// editor never decides either.
// SILKSCREEN - PLAIN WHITE, and nothing behind it.
//
// The head panel's kTextColor and kDimColor are chosen against a dark faceplate and are unreadable
// on a saturated enclosure: dim grey on the Chorus's yellow is very nearly invisible. So the
// pedals letter themselves in their own ink, and that ink is white.
//
// WHAT THE ART MEASURES, recorded because it disagrees and the disagreement should stay visible.
// WCAG contrast of white against each enclosure's own mean face pixels -
//   Boost   green  3.10     Chorus  yellow 1.86     Flanger red  4.57
//   Delay   blue   4.07     Reverb  lime   1.95
// Three of the five are under the 4.5 : 1 the guideline asks of text this size and two are far
// under it. That is a property of bright enclosures, not of the choice: nothing in the palette
// reads well on a mid-luminance yellow. panelrender prints all five on every run, so a re-export
// that makes one worse is visible in the build output rather than silent.
//
// TWO WAYS OF PROPPING THE WHITE UP WERE BUILT, RENDERED AND REJECTED, and they are recorded here
// so nobody builds them again. An offset drop shadow GREYS SMALL TEXT OUT: at kPedalLabelSize
// Michroma's stems are about one pixel, so nearly every pixel of a legend is an antialiased blend
// and the offset copy lands underneath it rather than beside it. Replacing it with a true outward
// edge - stroking the glyph path, then filling it, so the white core keeps its full width - fixed
// the greying (peak ink coverage became identical at every edge alpha) but not the look: 0.8 of a
// unit of dark around a one-unit stem reads as a black outline, which is what it is. Rendered side
// by side at 2x, plain white is crisper than either, on all five enclosures including the yellow.
//
// Black lettering with a white outline was rendered too, and is worse: it goes muddy on the
// Flanger's red, where black measures 4.57 : 1 against white's own 4.57 and looks far weaker.
constexpr uint32_t kPedalInk = 0xFFFFFF;
// The lettering ON A FILLED PLATE, which is the one place this page is not white on colour: a mini
// control that is LIVE inverts - white plate, dark text - and so does a knob's readout while it is
// being dragged. Outlined is idle, filled is live. That is used rather than the accent colour
// because kAccent is a green that disappears on the Boost and shouts on the Chorus.
constexpr uint32_t kPedalInkPlate = 0x101214;

struct PedalSpec {
    const char *name; // drawn on the enclosure, Michroma
    const char *art;  // ImageCache key; resources/img/<art>.png
    int cx;           // centre line; the blit's left edge is cx - kPedalW / 2
    int y;            // top edge
    bool post;        // false = before the amp, true = after the cabinet
};
// The count lives in rationsids.h, with the parameter table: there are five pedals because there
// are five effects, which is a fact about the DSP and not about the layout. Re-exported so
// geo::kPedalCount still names it (qualified lookup does not reach an enclosing namespace).
using Rations::kPedalCount;
constexpr PedalSpec kPedals[kPedalCount] = {
    {"Boost", "pedal-boost", kPedalPreCX[0], kPedalRow1Y, false},
    {"Chorus", "pedal-chorus", kPedalPreCX[1], kPedalRow1Y, false},
    {"Flanger", "pedal-flanger", kPedalPostCX[0], kPedalRow2Y, true},
    {"Delay", "pedal-delay", kPedalPostCX[1], kPedalRow2Y, true},
    {"Reverb", "pedal-reverb", kPedalPostCX[2], kPedalRow2Y, true},
};
constexpr int kPedalLeft(int i)
{
    return kPedals[i].cx - kPedalW / 2;
}
// The patch cables run between neighbours WITHIN a row. They are not drawn from
// the PRE row to the POST row: what sits between those two is the amplifier, and
// a cable implying otherwise would be describing the wrong signal path.
constexpr float kPedalCableSag = 7.0f;
constexpr float kPedalCablePen = 2.0f;

// --- the face, generated from the parameter slice ---------------------------
// The enclosures are blank, so a face is a blit plus controls drawn over it, and
// which controls those are comes out of kPedalParams (see pedalKnobCount and
// pedalMiniCount in rationsids.h) rather than out of a per-pedal layout here.
// Five faces, one piece of arithmetic: two knobs across the top, then either a
// third centred below them or a second pair, and the LED, the footswitch and the
// name down the middle.
//
// The grid is 2-up because the enclosure is 190 units wide and a legible knob is
// 40 across with a legend under it; three across would put "Feedback" into its
// neighbour's column at the label size the rest of the panel uses.
constexpr int kPedalKnobCX = kPedalW / 2; // 95 - the pedal's own centre line
constexpr int kPedalMaxKnobs = 4;

struct PedalPoint {
    int x, y; // relative to the pedal's own top-left corner
};
constexpr PedalPoint pedalKnobPos(int nKnobs, int k)
{
    if (nKnobs <= 1)
        return {kPedalKnobCX, kPedalKnobRow1Y};
    if (nKnobs <= 3) {
        // An ODD count puts its last knob on the centre line rather than leaving a hole: three
        // knobs is a triangle, which is the Tube Screamer's own layout and the layout of most
        // three-knob pedals, and it is what kPedalKnobMidY exists for. It is also why the
        // clearance audit works by COLUMN and not by row - here the upper legends sit BESIDE the
        // lower dial, not above it, so nothing they could collide with is in their column.
        if (k == 2)
            return {kPedalKnobCX, kPedalKnobMidY};
        return {kPedalKnobCX + (k == 0 ? -kPedalKnobDX : kPedalKnobDX), kPedalKnobRow1Y};
    }
    return {kPedalKnobCX + ((k % 2 == 0) ? -kPedalKnob4DX : kPedalKnob4DX),
            (k < 2) ? kPedalKnob4Row1Y : kPedalKnob4Row2Y};
}

// The two mini slots, either side of the LED: a text control, not a knob, because
// what goes here is a list (the Delay's Sync) and a two-state switch (its
// Ping-Pong) and neither reads as a rotation. They sit ON the LED's row, which is
// the only band on the enclosure with full width and nothing else in it - the
// knob labels end below kPedalKnob4Row2Y + kPedalKnobR + kPedalLabelDY and the
// footswitch begins at kPedalSwitchY - kPedalSwitchR, and both are asserted below.
constexpr int kPedalMiniCount = 2;
// Sized and placed against the FACE, not the body: at 62 wide and centred on 141 the right-hand
// box ran to 172, which is out on the border trim. The face leaves 66 units either side of the
// lamp, so a 58-unit box with 4 of margin at each end sits inside both.
constexpr int kPedalMiniW = 58;
constexpr int kPedalMiniH = 16;
constexpr int kPedalMiniSize = 10;
constexpr int kPedalMiniRadius = 3;
constexpr int kPedalMiniCX[kPedalMiniCount] = {55, 135};
constexpr int kPedalMiniY = kPedalLedY; // centred on the LED's own row

// A footswitch is stomped, not clicked, so its hit box is the enclosure's full
// width and deliberately larger than the cap - the same reasoning that made the
// head's bat switches kToggleHitW = 60 against art 24 wide. Nothing else lives
// in that band, so there is nothing for a generous box to steal from.
constexpr int kPedalSwitchHitH = 56;
constexpr int kPedalKnobHitR = kPedalKnobR + 4;

static_assert(kPedalMiniCX[0] - kPedalMiniW / 2 >= kPedalFaceLeft,
              "the left mini slot runs on to the enclosure's border trim");
static_assert(kPedalMiniCX[1] + kPedalMiniW / 2 <= kPedalFaceRight,
              "the right mini slot runs on to the enclosure's border trim");
static_assert(kPedalMiniCX[0] + kPedalMiniW / 2 < kPedalKnobCX - kPedalLedR,
              "the left mini slot runs into the LED");
static_assert(kPedalMiniCX[1] - kPedalMiniW / 2 > kPedalKnobCX + kPedalLedR,
              "the right mini slot runs into the LED");
static_assert(kPedalMiniY - kPedalMiniH / 2 > kPedalKnob4Row2Y + kPedalKnobR,
              "the mini slots run into a four-knob face's lower dials");
static_assert(kPedalMiniY + kPedalMiniH / 2 < kPedalSwitchY - kPedalSwitchR,
              "the mini slots run into the footswitch");

// The one thing this whole scheme rests on: that no pedal asks for more controls
// than the enclosure has places to put them. It is checked at COMPILE TIME against
// kPedalParams, so adding a fifth knob to a pedal is a build error rather than a
// face that silently draws four of them and drops the rest.
constexpr bool pedalFacesFit()
{
    for (int p = 0; p < kPedalCount; ++p)
        if (pedalKnobCount(p) > kPedalMaxKnobs || pedalMiniCount(p) > kPedalMiniCount)
            return false;
    return true;
}
static_assert(pedalFacesFit(), "a pedal has more controls than its enclosure has places");

// The same points in PAGE coordinates. Every draw call and every hit test goes through these four,
// so a pedal's corner offset is applied in exactly one place and the click can never land where
// the paint did not.
constexpr PedalPoint pedalKnobCenter(int pedal, int k)
{
    const PedalPoint p = pedalKnobPos(pedalKnobCount(pedal), k);
    return {kPedalLeft(pedal) + p.x, kPedals[pedal].y + p.y};
}
constexpr PedalPoint pedalMiniCenter(int pedal, int slot)
{
    return {kPedalLeft(pedal) + kPedalMiniCX[slot], kPedals[pedal].y + kPedalMiniY};
}
constexpr PedalPoint pedalLedCenter(int pedal)
{
    return {kPedalLeft(pedal) + kPedalKnobCX, kPedals[pedal].y + kPedalLedY};
}
constexpr PedalPoint pedalSwitchCenter(int pedal)
{
    return {kPedalLeft(pedal) + kPedalKnobCX, kPedals[pedal].y + kPedalSwitchY};
}

// A knob legend's allowance is set by the ENCLOSURE, not by the knob pitch. The outer columns sit
// 45 units off the centre line and the body's edge is only 80 units from it, so a legend centred
// on an outer knob has 35 units of room on its outside and runs over the edge - on to the jack lug
// and the patch cable - long before it reaches its neighbour. Symmetric, because the text is
// centred on the knob, so the tighter side governs both.
//
// The first version of this measured against the pitch and passed "Feedback", which then drew five
// units past the Delay's right-hand edge. It was the rendered page that showed it, and this is the
// arithmetic that makes the audit catch the next one.
constexpr int kPedalLabelMargin = 3; // bare FACE between a legend and the border trim
constexpr int pedalKnobLabelAllowance(int pedal, int k)
{
    const int cx = pedalKnobPos(pedalKnobCount(pedal), k).x;
    const int l = cx - kPedalFaceLeft;
    const int r = kPedalFaceRight - cx;
    return 2 * (l < r ? l : r) - 2 * kPedalLabelMargin;
}

// --- Settings page ----------------------------------------------------------
// Four sections down one column: the capture loaders, the channel trims, MIDI
// learn, and the output section. The first two are four rows each, MIDI learn is
// nine (four channels and five pedals), and all three share one grid; the fourth
// is a different shape and is laid out below them.
//
// All of them share kMidiRowX / kMidiRowW / kMidiRowH / kMidiRowPitch and the
// same kMidiTextX for their second column, so the channel names and the controls
// beside them line up down the whole page rather than forming four grids that
// nearly agree.
constexpr int kSettingsHeadingSize = 18;
constexpr int kMidiRowX = 24;
constexpr int kMidiRowW = kSettingsPageW - 2 * kMidiRowX; // 592
constexpr int kMidiRowH = 32;
constexpr int kMidiRowPitch = 40;

// kSettingsMinViewH is spelled as a literal up beside kSettingsPageH, because
// pageMinH() needs it long before this grid is declared. This is the arithmetic
// it stands for, checked rather than trusted.
static_assert(kSettingsMinViewH == kPageContentTop + 3 * kMidiRowPitch + 20,
              "kSettingsMinViewH must stay the header band plus three rows plus a margin");

// --- The settings page's scrollbar ------------------------------------------
// Down the right-hand margin, in the 24 units between the rows' right edge
// (kMidiRowX + kMidiRowW = 616) and the page edge, so it takes no width from
// anything and no row had to move to make space for it.
//
// Wider than the file browser's own 3-unit scroll INDICATOR, and the difference
// is the point: that one only reports a position, this one is dragged. Same two
// colours, so they read as the same idea at two jobs.
constexpr int kScrollBarW = 10;
constexpr int kScrollBarInset = 6;
constexpr int kScrollBarX = kSettingsPageW - kScrollBarInset - kScrollBarW; // 624
constexpr float kScrollBarRadius = 5.0f;
// A thumb proportional to the visible fraction, floored so that a long page
// still leaves something to catch hold of.
constexpr float kScrollThumbMinH = 28.0f;
// One wheel click, in logical units: one row of the grid above, so the page
// steps by a row rather than by an arbitrary distance.
constexpr double kScrollWheelStep = kMidiRowPitch;

// The bar takes its width out of the page's right margin and out of nothing
// else, so no row had to move and no row may grow into it later. Checked here
// rather than audited at render time, because it is arithmetic on two constants
// and a compile error is a better report than a picture.
static_assert(kMidiRowX + kMidiRowW <= kScrollBarX,
              "the settings rows must not reach into the scrollbar's margin");
static_assert(kScrollBarX + kScrollBarW + kScrollBarInset == kSettingsPageW,
              "the scrollbar must sit kScrollBarInset in from the page's right edge");
// Nine rows: the four channels, then the pedalboard's five footswitches. Spelled here in the
// editor's own vocabulary and checked against kMidiLearnRowCount, which is the processor's, at the
// one site that includes both.
constexpr int kMidiRowCount = kChannelToggleCount + kPedalCount;
// Half a row of air between the two halves, because they are two halves: a channel row selects
// and a pedal row toggles, and a footnote saying so under nine identical rows is a footnote
// nobody connects to anything. Drawn as a gap rather than as a second heading because the
// section already has one and the rows are self-labelling.
constexpr int kMidiPedalGap = 20;
constexpr int kMidiPedalFirstRow = kChannelToggleCount;
constexpr int kMidiRowTextSize = 15;

// Section 1: the capture loaders. One row per channel, each carrying the
// channel's name on the left and what is loaded on the right, with a clear box
// at the far end - the same shape as the cabinet page's IR rows, because it is
// the same act.
constexpr int kCaptureHeadingY = 76;
constexpr int kCaptureRowY0 = 94;
constexpr int kCaptureRowCount = kChannelToggleCount;
// The name field, as an offset from the row's left edge. It sits where the level
// sliders and the MIDI binding text start, so the second column is one column
// down the whole page.
constexpr int kCaptureNameX = 40;
// Wide enough for a real amp name rather than for the four defaults. Measured
// against the art audit's own fixtures: "Deluxe Reverb" in Michroma at
// kMidiRowTextSize is the case that decided this, because it is an entirely
// ordinary thing to call a channel and it did not fit at 116. What is left over
// still leaves the path column more room than a basename needs.
constexpr int kCaptureNameW = 170;
// Where the loaded path is written, and how much room it has before the clear
// box. Roboto rather than Michroma: a path is a variable-length string that has
// to stay legible when clipped, which is the one thing this panel keeps a
// proportional face for.
constexpr int kCaptureTextX = kCaptureNameX + kCaptureNameW + 12; // 168
constexpr int kCaptureClearW = 24;
constexpr int kCaptureClearInset = 6;
constexpr int kCaptureTextW = kMidiRowW - kCaptureClearInset - kCaptureClearW - kCaptureTextX - 8;
// The build-progress bar along the bottom edge of a row, as the parent plug-in
// draws it: two units of the row's own border rather than a widget, because what
// it reports is transient and a bank that is built has nothing to say.
constexpr int kCaptureProgressH = 2;
constexpr int kCaptureFootnoteY = 268;

// Section 2: channel levels.
constexpr int kLevelHeadingY = 296;
constexpr int kLevelRowY0 = 314;
constexpr int kLevelRowCount = kChannelToggleCount;
// The slider's track, as an offset from the row's left edge. Starts at
// kMidiTextX so it begins where the MIDI section's binding text does.
constexpr int kLevelSliderX = 116;
// The width is not a free choice: it is what CENTRES the track, and it was got
// wrong first time. The left edge is a column decision (above), but the row is
// centred on the page and so is the heading over it, so a track inset by
// kLevelSliderX on the left must be inset by the same on the right - otherwise
// its 0 dB mark, the one thing on this page with a visible centre of its own,
// sits somewhere the eye reads as a mistake. It was 302, which put the tick 29
// units left of the heading above it. Written as the arithmetic rather than as
// the number so it cannot drift out of step with either constant it depends on.
constexpr int kLevelSliderW = kMidiRowW - 2 * kLevelSliderX;
constexpr int kLevelTrackH = 5;
constexpr int kLevelThumbW = 12;
constexpr int kLevelThumbH = 20;
// The centre mark, drawn through the track at 0 dB so the default position is
// findable by eye rather than only by the readout.
constexpr int kLevelCentreTickH = 13;
// The dB readout, right-aligned to the row's right edge less this inset.
constexpr int kLevelReadoutInset = 12;
constexpr int kLevelReadoutW = 70;
// The thumb's centre travels over the track less its own width, so it never
// overhangs either end of the track. That travel is also the drag range, so the
// pointer and the thumb move one for one.
constexpr float kLevelTravel = static_cast<float>(kLevelSliderW - kLevelThumbW);
constexpr float kLevelDragRange = kLevelTravel;
static_assert(2 * kLevelSliderX + kLevelSliderW == kMidiRowW,
              "the level slider's track is no longer centred in its row");
// Within this many logical units of the centre, a drag snaps to exactly 0 dB.
// A trim whose default is the middle has to be returnable to the middle by hand;
// right-clicking the row does it exactly, and this makes dragging do it too.
constexpr float kLevelCentreSnap = 4.0f;
// One wheel click on a level row, in dB. Half a decibel: fine enough to land on
// a round number, coarse enough that the whole range is a manageable number of
// clicks away.
constexpr double kLevelWheelDb = 0.5;
// The levels' own footnote, between the sections.
constexpr int kLevelFootnoteY = 486;

// Section 3: MIDI learn. The gate is deliberately absent from it: the gate is not
// on the MIDI path at all.
constexpr int kSettingsHeadingY = 514;
// The opening height of the page's window, up beside kSettingsPageH where
// pageMinH()'s neighbours are, is this heading's baseline plus enough room for
// its descenders — so the heading is whole on the bottom edge rather than half
// off it.
static_assert(kSettingsDefaultViewH == kSettingsHeadingY + 10,
              "kSettingsDefaultViewH must stay the MIDI heading's baseline plus its descender");
constexpr int kMidiRowY0 = 532;

// The top edge of one MIDI row. One function rather than the arithmetic written at each site,
// for the same reason contentY() is one function: the painter, the hit test and the art audit
// have to agree about where a row is, and a gap in the middle of the list is exactly the kind of
// detail two of the three would keep and the third would forget.
constexpr int midiRowY(int row)
{
    return kMidiRowY0 + row * kMidiRowPitch + (row >= kMidiPedalFirstRow ? kMidiPedalGap : 0);
}
// Where the learned binding is written, as an offset from the row's left edge.
// Far enough right to clear the widest channel name at kMidiRowTextSize, which
// the art audit measures rather than assumes.
constexpr int kMidiTextX = 116;
// The Learn button, as an offset from a row's right edge, and the Clear button
// to its left. Clear is drawn only on a row that HAS a binding: an always-there
// Clear on an unlearned row is a control with nothing to do, and the row is not
// wide enough to spend on one.
constexpr int kMidiLearnW = 108;
constexpr int kMidiLearnInset = 6;
constexpr int kMidiClearW = 70;
constexpr int kMidiButtonGap = 6;
// The widest the binding text may be: from kMidiTextX to the Clear button, since
// Clear is the one that appears once there is a binding to describe.
constexpr int kMidiTextW =
    kMidiRowW - kMidiLearnInset - kMidiLearnW - kMidiButtonGap - kMidiClearW - kMidiTextX - 8;
constexpr int kSettingsFootnoteY = 924;
constexpr int kSettingsFootnote2Y = 942;
constexpr int kSettingsFootnote3Y = 960;
constexpr int kSettingsFootnoteSize = 12;
constexpr int kSettingsFootnoteCount = 3;

// Section 4: the output section. Three radio rows for the mode, then the input
// calibration pair beside each other on one row.
//
// It is last because it is set once when a rig is assembled, and it is here at
// all because it was previously nowhere: the plug-in was hard-wired to
// Normalized with no way to see or change it, while both plug-ins it descends
// from expose exactly these three controls.
constexpr int kOutputHeadingY = 988;
constexpr int kOutputRowY0 = 1006;
constexpr int kOutputRowPitch = 26;
constexpr int kOutputRowH = 22;
constexpr int kOutputRowW = 300;
// The radio dot, as an offset from the row's left edge, and its text.
constexpr float kOutputDotCX = 10.0f;
constexpr float kOutputDotR = 6.0f;
constexpr float kOutputDotFillR = 3.0f;
constexpr int kOutputTextX = 26;
// The calibration row: a bat toggle on the left and the dBu value box to its
// right, both drawn only as far as the loaded captures can honour them.
constexpr int kCalRowY = 1098;
constexpr int kCalToggleCX = kMidiRowX + 40;
constexpr int kCalToggleCY = kCalRowY + 20;
constexpr int kCalLabelX = kMidiRowX + 70;
constexpr int kCalValueX = kMidiRowX + 320;
constexpr int kCalValueW = 110;
constexpr int kCalValueH = 26;
constexpr int kCalValueY = kCalRowY + 7;
// One wheel click on the calibration level, in dB. Whole decibels: an interface's
// stated level is a round number and this is how a user lands on theirs.
constexpr double kCalWheelDb = 1.0;
constexpr int kOutputFootnoteY = 1144;

// The Learn button's two states, named here so the panel and the art audit
// cannot disagree about which strings have to fit inside it.
constexpr const char *kMidiLearnLabel = "Learn";
constexpr const char *kMidiListenLabel = "Listening";
constexpr const char *kMidiClearLabel = "Clear";
// What an armed row says while it waits. It replaces the binding text rather
// than sitting beside it, so it shares that space and that allowance.
constexpr const char *kMidiListeningText = "press a pedal...";

// The two footnotes, here rather than at the draw site so the art audit measures
// the strings the panel actually paints. The second is not decoration: a learned
// CC or Program Change answers on EVERY MIDI channel, because VST3 hands those
// over as parameter changes with the channel already discarded, and a player
// needs to know that before they discover it by playing.
constexpr const char *kSettingsFootnote =
    "The gate switch is not learnable and stays where you leave it.";
// The two headings, here rather than at the draw site so the art audit measures
// what the panel paints.
constexpr const char *kLevelHeading = "Channel Levels";
constexpr const char *kMidiHeading = "MIDI Learn";
// Said once, under the levels, because a trim that can only be reset by dragging
// back to the middle is a trim nobody quite returns to zero.
constexpr const char *kLevelFootnote = "Right-click a slider to return it to 0.0 dB.";
constexpr const char *kSettingsFootnote2 =
    "CC and Program Change answer on any MIDI channel; a note answers on its own.";
// The third footnote is the one the pedal rows made necessary, and it says the
// thing a player would otherwise have to work out by stamping: the two halves of
// this list do different things with a press. See midilearn.h for why a pedal has
// to toggle and a channel must not.
constexpr const char *kSettingsFootnote3 =
    "A channel row selects that channel; a pedal row toggles that pedal on and off.";

// The capture section's heading and footnote. The footnote is the one place the
// two ways of naming a channel are explained, because neither is discoverable:
// nothing about a row says that loading a folder renames the channel after it,
// and nothing says the name can then be typed over.
constexpr const char *kCaptureHeading = "Load Captures";
constexpr const char *kCaptureFootnote = "A folder of captures, or a single one. The channel takes "
                                         "the folder's name; click it to rename.";
constexpr const char *kCapturePlaceholder = "Select a capture, or a folder of captures...";

// The output section. The three mode names are the upstream plug-in's and are in
// its order, which is also the order the parameter's value space is in.
constexpr const char *kOutputHeading = "Output";
constexpr const char *kOutputModeNames[3] = {"Raw", "Normalized", "Calibrated"};
constexpr const char *kCalibrateLabel = "Calibrate Input";
// What a greyed mode says instead of nothing. Which channel's captures it is
// talking about is the SOUNDING one, which is what the footnote is for: without
// it a player watches an option grey itself on a footswitch stomp with no way to
// know why.
constexpr const char *kOutputUnsupported = "  [not in this channel's captures]";
constexpr const char *kOutputFootnote =
    "Greyed options are ones the sounding channel's own captures do not carry the metadata for.";

// --- File browser overlay ---------------------------------------------------
// Drawn over the cabinet page (the only page with anything to load), so it is
// sized to that page rather than to the head's.
constexpr int kBrowserX = 16;
constexpr int kBrowserY = 16;
constexpr int kBrowserW = kCabPageW - 2 * kBrowserX; // 608
constexpr int kBrowserH = kCabPageH - 2 * kBrowserY; // 428

// The settings page opens the same browser for its four capture rows, and it is
// a much taller page — the cabinet's card centred on it would leave 230 units of
// dimmed panel above and below, and the card is a LIST, so the height is the one
// dimension it can actually use. Inset the same 16 either side and give it the
// page it is drawn over.
constexpr int kCaptureBrowserX = 16;
constexpr int kCaptureBrowserY = 40;
constexpr int kCaptureBrowserW = kSettingsPageW - 2 * kCaptureBrowserX; // 608
constexpr int kCaptureBrowserH = kSettingsPageH - 2 * kCaptureBrowserY; // 1086
// That height is a CEILING now, not the size: the settings page scrolls, so the
// window showing it may be shorter than the page, and the card is sized to the
// viewport instead (RationsEditorView::boundCaptureBrowser). This is the floor
// for that — the card's own header and footer plus three rows, below which it
// stops being a list and becomes a pair of buttons.
constexpr int kCaptureBrowserMinH = 160;

} // namespace geo
} // namespace Rations
