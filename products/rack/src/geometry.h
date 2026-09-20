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

#include <cstddef>

namespace Rations
{
namespace geo
{

// --- Pages ------------------------------------------------------------------
// A page is a VIEW, not a parameter: it is editor-local state and is deliberately
// never persisted or automated, because a host recalling a preset must not also
// recall which panel the user happened to be looking at.
enum class Page { Head, Setup };
constexpr int kPageCount = 2;

// ONE WIDTH FOR EVERY PAGE, AND HEIGHT AS THE FREE AXIS.
//
// The parent plug-in gave each page its own canvas width and asked the host for
// a new window on every page change: head 1133x403, cabinet 640x460, settings
// 640x1166. The reason was sound — the head is 2.81:1 because an amp head is,
// and drawing a short list of MIDI rows inside that letterbox leaves hundreds of
// units of black down each side.
//
// It cannot survive here, and the thing that kills it is the rack. This editor
// is one child window in a top-level that also holds the rack strip directly
// below it, and the two necessarily share a width. A page change that took the
// window from 1133 units to 640 would drag the rack down to 640 with it and back
// again — and the rack's list view (name, mix slider, reorder arrows, gear,
// cross) does not survive at that width.
//
// So: the window has ONE width, the head page's 1133, and a page change resizes
// VERTICALLY only. The editor still calls IPlugFrame::resizeView() — the SDK's
// documented plug-in-initiated resize (pluginterfaces/gui/iplugview.h, "Plug-in
// requested resize", after which the host calls back into IPlugView::onSize())
// — but it asks for a new height against the width it already has. The scale
// plumbing is untouched: still one cairo_scale(s, s) at compose, still mouse
// divided by s, still one constrainSize() rule.
//
// AND THAT IS WHY THERE ARE TWO PAGES RATHER THAN THREE. Once every page is
// 1133 wide, a 640-unit cabinet and a 640-unit settings list side by side are
// the natural use of the room, and keeping them apart would mean two page
// changes, two resize paths and two scrollbars for content that fits in one. So
// they are ONE page in two columns — the cabinet on the left, the settings
// sections on the right.
//
// Two consequences, both deliberate:
//   * constrainSize() is still page-dependent, because the HEIGHT range changes
//     with the page even though the width does not. That is what
//     canResize/checkSizeConstraint exist for, and the host is told through the
//     resizeView call rather than being left to discover it.
//   * A host with no IPlugFrame cannot be asked to resize at all. There the
//     editor keeps the size it has and letterboxes the page inside it, which is
//     the parent's behaviour unchanged and is why every page is still drawn
//     centred on its own canvas rather than pinned to a corner. The width lock
//     above is the STANDALONE's policy, expressed in what it asks for; it is
//     never an assumption baked into the drawing code, because the bundle has to
//     keep working under a host that grants any size it likes.

// The head page IS the trimmed size of head.png, so at scale 1.0 the panel is a
// pixel-exact blit with no resampling. This one is not free to change.
constexpr int kWinW = 1133;
constexpr int kWinH = 403;

// --- The setup page's two columns -------------------------------------------
// The page is the window's width, and it is divided once, vertically, DOWN THE
// MIDDLE. Both columns are the same width and each is drawn centred in its own
// half, which is the author's call and is what the page now looks like.
//
// The split was asymmetric first, and the reasoning for that is worth keeping
// because it names the cost this pays. Both columns were designed at 640 as
// pages of their own, so an even split is a 12% squeeze on each. That is free
// for the cabinet, whose whole layout is derived from its column width already —
// the art is a photograph and scales, the blend dial is placed by fractions of
// it, the loader rows span it — and it is not free for the settings column,
// which is a list of rows carrying file paths, channel names and MIDI binding
// text at FIXED sizes with a scrollbar reserved at its right edge. Those strings
// do not shrink with the column; they clip sooner. The first version therefore
// gave the settings column its whole 640 and handed the cabinet the remainder.
//
// An even split reads better and that is what it is for: two halves of one page
// rather than a wide half and a narrow one. Everything below re-derives from
// these two constants, so the change is here and nowhere else — which is the
// payoff for both columns having been written against their own width rather
// than against numbers. The cabinet gains what the settings column gives up: its
// art goes from 449 units wide to 522.
//
// If a settings row ever turns out to clip something a user needs to read, the
// answer is to shorten the string or to elide it in the middle — not to take the
// width back, because the width is now a deliberate symmetry rather than a
// leftover.
constexpr int kSetupColW = kWinW / 2; // 566, and the odd unit falls between them
constexpr int kSetupSetColW = kSetupColW;
constexpr int kSetupSetColX = kWinW - kSetupSetColW; // 567, flush to the right edge
constexpr int kSetupCabColX = 0;
constexpr int kSetupCabColW = kSetupColW; // 566
static_assert(kSetupCabColW == kSetupSetColW, "the setup page's two columns are equal halves");
static_assert(kSetupCabColX + kSetupCabColW <= kSetupSetColX, "the two columns must not overlap");
// Each column is drawn in its OWN coordinates, translated into place by the
// painter — the same one-translate idiom the scroll already uses. That is what
// lets the settings constants stay written against 640 and the cabinet's against
// its own width, instead of every one of them growing a column offset.
constexpr int kSetupCabColCX = kSetupCabColW / 2; // 283
constexpr int kSetupSetColCX = kSetupSetColW / 2; // 283

// The cabinet: art aspect 1483/872 = 1.7007, drawn as wide as its column allows
// with the two IR rows underneath it and the page's chrome above.
constexpr int kCabPageW = kSetupCabColW;
constexpr int kCabPageH = 460;

// Settings: four sections, stacked in one column - the capture loaders, the
// channel levels, MIDI learn, and the output section.
//
// Captures come first because a channel with nothing loaded does nothing, so it
// is the section a new user needs before any other one means anything. Output
// comes last because it is set once when a rig is assembled. In between, levels
// before MIDI, because every user has four channels to balance and only some own
// a footswitch.
//
// 928 units tall, which is taller than any other page here by a long way, and
// the only page whose window may be shorter than the page itself: it is
// width-locked, free in height, and scrolls. See pageScrolls() below for why a
// lower scale floor did not turn out to be enough on its own — and pageScaleMin
// for why that floor is gone now that the window's width is shared.
//
// It was 1166 while the parent plug-in's pedalboard existed: five footswitch
// rows at kMidiRowPitch (200), the half-row of air that separated them from the
// channel rows (20), and the footnote that had to explain why the two halves
// behaved differently (18) came to exactly 238. All three left with the
// pedalboard and the page is back to what it measured before them, which is the
// height every constant below is written against. It then grew by 18 — one
// footnote line — when the output section had to explain that a mode needs its
// metadata in EVERY capture of a bank. That costs nothing but scrolling, which
// is what a page that already scrolls is for, and it reaches the setup page as
// 18 more units of air under the cabinet column, exactly as kSetupPageH intends.
constexpr int kSettingsPageW = kSetupSetColW;
constexpr int kSettingsPageH = 946;

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
// The setup page is the window's width and the taller of its two columns. The
// settings column is that taller one by a wide margin — 928 against the cabinet
// column's content — so the page's height is the settings column's and the
// cabinet simply has air under it.
constexpr int kSetupPageW = kWinW;
constexpr int kSetupPageH = kSettingsPageH;
constexpr PageSize kPageSizes[kPageCount] = {
    {kWinW, kWinH},
    {kSetupPageW, kSetupPageH},
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

// ONE FLOOR FOR BOTH PAGES, and it is the width lock that decides it rather than
// a preference. The parent gave its settings page a floor of its own (0.50,
// against 0.66) because that page was 1166 units tall where the head was 403,
// and the same multiple would have meant a very different window.
//
// That exception cannot exist here, and the reason is arithmetic rather than
// taste. The window has one width, shared with the rack strip below it. A page
// with a lower scale floor has a NARROWER minimum width — 1133 * 0.50 = 566
// against 1133 * 0.66 = 748 — so the two pages would disagree about the smallest
// legal window, and a page change at the bottom of the range would have to
// either move the rack or refuse. Both are the thing the width lock exists to
// prevent.
//
// It costs nothing to give it up. The lower floor was there to make a tall page
// fit in a short window, and the setup page does not need to fit: it is
// width-locked and free in height, and whatever does not fit is scrolled to. At
// 0.66 the page is 748 x 613 device px, and a window shorter than that is
// already legal — see pageMinH, which for a scrolling page is the shortest
// VIEWPORT and not the whole page.
constexpr double pageScaleMin(Page)
{
    return kScaleMin;
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
    return p == Page::Setup;
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
constexpr int kTitleSize = 15; // the title word, under the badge — see the wordmark block
// The word drawn under the badge, named here rather than written at each draw site. It is painted
// by the editor, measured by the art audit's text fit, and measured again by its wordmark
// clearance check — three places that must agree about the same string, and three chances for a
// rename to move two of them. Every other legend on this panel is a constant for exactly this
// reason; the title was the one that had been left as a literal.
constexpr const char *kTitleWord = "Rack";
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
// 11, one step down the cap-even list from the 15 the old pair of page buttons
// used (13 and 14 are not available — see the note above). There is one button
// now and it carries a 32-character legend where each of the old pair carried
// seven; Michroma is a wide face, and the corner button this replaces already
// needed 11 to fit a 24-character legend into 203 units. panelrender's text
// audit is what decides whether it fits, not this comment.
constexpr int kPageButtonTextSize = 11;
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

// --- The wordmark: the NAMp badge, with the title word under it -------------
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
// Stacked, the block is max(badge, text) wide rather than their sum, so what it
// spends is height, between the faceplate's top edge (kFaceT, 62) and the dial
// legends' ink (kKnobCY - kKnobLabelDY - kKnobLabelSize, 153). The asserts
// below hold both ends, and panelrender measures the text for real.
//
// THE BADGE IS THE MARK AND THE WORD IS THE CAPTION, and the balance between
// them is the author's, settled by looking at three renders rather than by
// arithmetic. It first drew at 48 with the word at 28 — a badge and a word of
// comparable weight, where the badge is what actually carries at a glance — and
// then over-corrected to 88 with the word at 12, where the word was smaller than
// the dial legends under it and read as a stray caption. 72 and 15 is the pair
// that was kept: the badge dominates, and the word is a step LARGER than the
// dial legends (kKnobLabelSize, 12) so that it reads as part of the mark rather
// than as another legend. 15 is the next cap-even size above 12 — 13 and 14 are
// stepped, see the note above kTitleSize.
//
// The band, not the width, is what bounds it: at this aspect 72 draws 242 wide
// against the 316 the Slim icon's clearance (asserted below) would allow, while
// the word's baseline is 13 clear of the dial legends' ink.
constexpr int kBadgeH = 72;
// The stored art is 512x152 (gui/geometry.sh BADGE_W/H, checked there against
// what the source actually trims to), so the drawn width is that aspect at
// kBadgeH. Written as the arithmetic rather than as 162 so that replacing the
// art and updating geometry.sh cannot leave this silently stretched.
constexpr int kBadgeW = kBadgeH * 512 / 152; // 242
// 6 below kFaceT, and the ink starts lower again: the art carries 6/152 of its
// height as transparent margin above it, which is 2.8 units here, so the gold
// begins 8.8 below the faceplate's edge — close to the meters' own 8.
constexpr int kBadgeTop = 68;
constexpr int kBadgeBottom = kBadgeTop + kBadgeH;
constexpr int kBadgeCX = kFaceCX;
// The word's baseline. Its cap top (kTitleBaselineY - the cap height, ~11 at
// this size) sits level with where the badge's ink ends UNDER ITS CENTRE, which
// is 0.836 of the art's height rather than the 0.961 its full ink box gives:
// the difference is "NAMp"'s p, which hangs lower than the rest and does so off
// to the right, either side of which the title word is narrow enough to clear. That
// is what makes the two read as one mark rather than as two things stacked, and
// it is measured off the art (magick -crop the centre third, then %@) rather
// than eyeballed.
constexpr int kTitleBaselineY = 140;
constexpr int kWordmarkBottom = kTitleBaselineY; // the title word has no descender
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
// kTopBandRight is the reflection; see there, and panelrender checks it.
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
// The wordmark's other end, deferred from its own block above: the title word
// sits under the badge and is the lowest ink the title carries, and the dial legends
// are what it would reach first.
static_assert(kWordmarkBottom < kKnobCY - kKnobLabelDY - kKnobLabelSize,
              "the wordmark has come down onto the dial legends");
static_assert(kBypassToggleCY + kTopToggleH / 2 <
                  kBypassToggleCY + kToggleLabelDY - kToggleLabelSize,
              "the utility row's legends are drawn on their own switches");
// The title's leftmost ink. This used to be a hand-measured number — the width
// of the title word at 56 — because the title was text and text is only measurable at
// draw time. It is DERIVED now: the badge is the wider of the two things stacked
// here (242 against the word's 72 at kTitleSize), so the block's edge is the
// badge's edge and arithmetic can say where it is. panelrender still measures
// the text for real, which is what catches the case where a larger kTitleSize
// makes the word the wider one and this stops being the block's edge.
constexpr int kWordmarkInkLeft = kFaceCX - kBadgeW / 2; // 445
static_assert(kGateToggleCX + kTopToggleHitW / 2 < kWordmarkInkLeft,
              "the utility row has grown into the wordmark");
// The wordmark's ink ends the same distance the other side of the faceplate
// centre, and the corner cluster is measured against it below.
constexpr int kWordmarkInkRight = 2 * kFaceCX - kWordmarkInkLeft; // 687

// --- The top band's right-hand corner ---------------------------------------
// THERE IS NO SETTINGS BUTTON HERE ANY MORE. The parent carried one — a labelled
// plate reading "Captures, MIDI, Settings", 203 units wide, ending at the mirror
// of the utility row's left edge — because the settings page was one of three
// destinations and the bottom row had no room for a fourth thing 200 units wide.
//
// With the cabinet and the settings folded into one page there is exactly ONE
// destination, and a single button in the faceplate's own button row says so
// better than two controls in two places that go to the same screen. So the
// corner button is gone and the bottom row's button carries the whole label; see
// the page-button block below.
//
// What the corner button LEFT BEHIND is the number: its right edge was the
// mirror of the bypass lamp's left edge through the faceplate centre, and that
// mirror is the established right edge of everything in this band. It outlives
// the control that was measured against it, because the Slim icon now sits
// there and the output meter is still where it was.
struct ButtonSpec {
    int x, y, w, h;
    const char *label;
    Page target;
};
constexpr int kPageButtonH = 30;
// The RIGHT EDGE of the top band's furniture: the mirror of the utility row's
// left edge — the bypass lamp's — through the faceplate centre, which is the
// symmetry described at kBypassLedCX. The two clusters therefore begin and end
// at the same distance from their own meter column.
constexpr int kTopBandRight = 2 * kFaceCX - (kBypassLedCX - kTopLedR); // 988
// Opens an overlay rather than a page: it is one knob, and a page for one knob
// would be a window change and a back button to reach a control most users set
// once and never touch. The file browser is the pattern it follows.
//
// It used to be placed off the settings button's left edge — the two were one
// cluster and what mattered was the gap between them. With that button gone the
// icon MOVES RIGHT into the space it vacated, and it is placed off the band's
// right edge instead: kTopBandRight is where the corner's furniture has always
// ended, and the icon is now the only thing in the corner, so it is what ends
// there. The document is 128x64, so a height of 22 draws 44 wide through
// getByHeight.
// The gear's own height, unchanged: the icon did not become a button and there
// is nothing in the corner for it to be a matched pair WITH, so the size that
// was measured stays the size it is.
constexpr int kSlimIconH = 22;
constexpr int kSlimIconW = 2 * kSlimIconH;                  // the .svg's own 2:1
constexpr int kSlimIconCX = kTopBandRight - kSlimIconW / 2; // 966
constexpr int kSlimIconCY = kTopBandCY;
// The right edge of everything in this corner, which is the icon's now. Named so
// the checks below and the output meter's clearance assert have one thing to
// measure against rather than each re-deriving it.
constexpr int kTopBandInkRight = kSlimIconCX + kSlimIconW / 2;
static_assert(kTopBandInkRight == kTopBandRight,
              "the Slim icon must end on the band's own right edge");
// The corner's leftmost ink. The band it sits in is the wordmark's, and the
// wordmark is drawn as measured text, so this is the clearance that says the two
// do not meet — the assert is the coarse half and panelrender's ink audit, which
// measures the title word at kTitleSize, is the one that decides.
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
// The top band's furniture ends in the gap between the wordmark and this column,
// and that gap is the whole of the room it has; asserted here because this is
// where the number it has to clear is declared. It guarded the settings button's
// right edge before that button was folded into the page-button row; the edge
// itself did not move, and the Slim icon now ends on it — so the guarantee is
// re-expressed against the control that carries it rather than deleted with the
// one that used to.
static_assert(kTopBandInkRight + 8 <= kOutputMeter.x,
              "the top band's furniture has reached the output meter");

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
// ONE button, and it is the editor's only way off the head page.
//
// There were two here — "Pedalboard" and "Cabinet", 147 units each, straddling a
// seam — plus a third in the top-right corner for the settings page. The
// pedalboard went with its feature, and the cabinet and the settings then merged
// into one page, so three destinations became one and the corner button had
// nothing left to be. What remains is a single plate on the seam carrying the
// whole label.
//
// ITS WIDTH IS THE PAIR'S: 2 * 147 plus the 10 units that separated them is
// exactly the span the old pair occupied, so the row's footprint on the
// faceplate is unchanged and nothing around it had to move. The faceplate was
// balanced against that span, and keeping it is cheaper than re-balancing.
//
// The seam is CENTRED ON A DIAL rather than left where the mock happened to put
// it (x 803/813, a 7 px near-miss on Middle, which is the kind of almost-aligned
// that reads as a mistake). Derived from the dial's own cx so the row follows if
// the dial pitch ever changes.
//
// Middle and not Bass, though Bass is the one this looks like it should line up
// with. The reason was the Gate bat switch: at 147 px wide a button on the left
// of the seam would run from 569 to 716 and sit on top of that switch's 597..657
// hit box, and nothing narrower fixed it either — clearing the switch capped the
// button at 59 px against a 108 px label.
//
// That switch has since moved to the utility row, so the constraint is gone and
// Bass is now free. The seam stays on Middle anyway: it is where it has been,
// nothing is wrong with it, and moving a measured position because a reason
// expired is how a panel drifts. Recorded rather than deleted so the next person
// to look at Bass finds out it is available and that this is a choice.
//
// kPageButtonSeamGap is what separated the pair. Nothing uses it while there is
// one button; it is kept because the seam is still the thing this row is
// positioned against, and a second button would want the same 10 units back.
// kPageButtonSeamGap is what separated the pair. There is no seam to hold open
// any more, but the number is still load-bearing: the single button's width is
// the pair's span, and that span is two buttons plus this gap.
constexpr int kPageButtonSeamGap = 10;
constexpr int kPageButtonSeamCX = kKnobs[6].cx;            // Middle
constexpr int kPageButtonW = 2 * 147 + kPageButtonSeamGap; // 304
constexpr int kPageButtonY = 277;
constexpr int kPageButtonCount = 1;
constexpr const char *kPageButtonLabel = "Captures, Cabinet, Midi, Settings";
constexpr ButtonSpec kPageButtons[kPageButtonCount] = {
    {kPageButtonSeamCX - kPageButtonW / 2, kPageButtonY, kPageButtonW, kPageButtonH,
     kPageButtonLabel, Page::Setup},
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

// --- Cabinet column ---------------------------------------------------------
// The cabinet does NOT wear the amp head's faceplate: it is a picture of a
// different object, so it is drawn on the same dark ground the head's letterbox
// uses.
//
// The cabinet is drawn as wide as the column allows, and the two loader rows are
// then sized so the pair spans EXACTLY the cabinet's width. That is what stops
// it reading as a picture with two unrelated widgets under it: cabinet and rows
// are one block, and the margin around them is even on all four sides.
constexpr int kCabMargin = 22;
constexpr int kCabW = kCabPageW - 2 * kCabMargin;      // 522
constexpr int kCabH = (kCabW * 872 + 1483 / 2) / 1483; // 307, the art's own aspect
constexpr int kCabX = kCabMargin;

// The height of one file-loader row. Declared here rather than beside the two IR
// rows it describes because the block's total height is needed to place the
// block, and a row is part of that height.
constexpr int kFileRowH = 28;
// Between the cabinet art and the loader rows under it.
constexpr int kCabRowGap = 12;
// Between the two stacked loader rows. Much smaller than the 18 that used to
// separate them side by side: horizontally that gap was holding two widgets
// apart, and vertically it is holding two rows of one list together.
constexpr int kIrRowVGap = 8;
// Cabinet plus gap plus BOTH rows of loaders: the whole column, as one object.
// The rows are stacked rather than side by side so each spans the cabinet's full
// width -- see kIrRowW for the measurement that decided it. Height is the axis
// this page has to spend, and it is the one it has: the two asserts under kCabY
// are what check the taller block still fits the window the page opens at.
constexpr int kCabBlockH = kCabH + kCabRowGap + 2 * kFileRowH + kIrRowVGap;

// --- Where that block sits vertically ---------------------------------------
// CENTRED IN THE WINDOW THE PAGE OPENS AT, not pinned under the back button.
//
// It used to be pinned, and that was right when the cabinet was a page of its
// own: the window was 460 units tall, the block filled it, and there was nothing
// to centre in. As one column of a page that opens at kSettingsDefaultViewH and
// scrolls past it, pinning leaves the cabinet hard against the top of a window
// two hundred units taller than it, with all the air below — which reads as a
// picture that failed to load the rest of itself.
//
// The band is from kPageContentTop (under the back button, which is chrome and
// does not scroll) to the height the window OPENS at. Not to the page's full 928:
// the settings column next to it is a scrolling list whose length is its own
// business, and centring the cabinet against that would put it below the fold on
// a page it is the left half of.
//
// The consequence is that the cabinet is centred exactly at the opening size and
// drifts from centre as the window is dragged taller or shorter, which is the
// correct trade: the opening size is the one a user sees every time, and a page
// that re-centred as it was dragged would move its own contents under the
// pointer.
//
// AND THIS COLUMN DOES NOT SCROLL. It is pinned while the settings list beside
// it moves, so these two constants place it in the WINDOW rather than in the
// page — which is why the band above is measured to the height the window opens
// at and not to the page's 928. The one exception is a window too short to show
// the block at all, where it slides up just far enough to bring its own bottom
// edge on screen and then stops; that clamp is RationsEditorView::cabinetScroll,
// and the second assert below is what guarantees the clamp is zero at every
// window from the opening size up.
constexpr int kCabY = kPageContentTop + (kSettingsDefaultViewH - kPageContentTop - kCabBlockH) / 2;
static_assert(kCabY >= kPageContentTop,
              "the cabinet block is taller than the window the setup page opens at, so centring "
              "it would push it up under the back button");
static_assert(kCabY + kCabBlockH <= kSettingsDefaultViewH,
              "the cabinet block must be whole in the window the setup page opens at — it is the "
              "half of that page that does not scroll to reveal more");

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
constexpr int kIrRowY = kCabY + kCabH + kCabRowGap;
// EACH row spans the cabinet's full width, because the two are stacked. Measured
// on a real IR collection (256 files) rather than judged by eye: an IR pack names
// its files by a long fixed prefix and a short varying suffix
// ("V30 LL 4FB 4x12 SM57 0.00in 0.0in OA30 7603.wav"), so what identifies a file
// is the part at the END. Two side-by-side rows left 150 units for the name here,
// which truncated every one of those before the suffix -- 192 distinct files
// rendered as 12 distinct labels, so the prev/next arrows changed the sound
// without changing a pixel of the text. Full width gives 420 against a longest
// measured name of 328: 192 of 192 distinct.
constexpr int kIrRowW = kCabW; // 522
constexpr FileRow kIrRowA = {kCabX, kIrRowY, kIrRowW, kFileRowH, "Select IR...", "wav"};
constexpr FileRow kIrRowB = {kCabX,     kIrRowY + kFileRowH + kIrRowVGap, kIrRowW,
                             kFileRowH, "Select IR (optional)...",        "wav"};
static_assert(kIrRowA.x + kIrRowA.w == kCabX + kCabW && kIrRowB.x + kIrRowB.w == kCabX + kCabW,
              "each IR row must span exactly the cabinet's width -- cabinet and rows are one "
              "block, and a row that stops short reads as a widget parked under a picture");
static_assert(kIrRowB.y + kIrRowB.h == kCabY + kCabBlockH,
              "the cabinet block's height must end exactly at the lower IR row -- it is what "
              "places the block, and the two asserts at kCabY check it against the window");
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

// The pedalboard page's geometry lived here: the enclosure grid, the per-face
// knob columns, the LED and footswitch positions, and the label-allowance
// arithmetic the art audit used to catch a legend running past a pedal's edge.
// All of it went with the feature. The pedals are separate plug-ins in the rack
// now and each one carries its own layout, in its own project.
//

// --- Settings page ----------------------------------------------------------
// Four sections down one column: the capture loaders, the channel trims, MIDI
// learn, and the output section. The first three are four rows each and share
// one grid; the fourth is a different shape and is laid out below them.
//
// All of them share kMidiRowX / kMidiRowW / kMidiRowH / kMidiRowPitch and the
// same kMidiTextX for their second column, so the channel names and the controls
// beside them line up down the whole page rather than forming four grids that
// nearly agree.
constexpr int kSettingsHeadingSize = 18;
constexpr int kMidiRowX = 24;
constexpr int kMidiRowW = kSettingsPageW - 2 * kMidiRowX; // 518
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
constexpr int kScrollBarX = kSettingsPageW - kScrollBarInset - kScrollBarW; // 550
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
              "the scrollbar must sit kScrollBarInset in from the column's right edge");
// The scrollbar in PAGE coordinates. It is chrome — it scrolls the whole page, not one column —
// so it is hit-tested and drawn outside the column translate and needs its own x. The settings
// column is flush against the page's right edge, so this lands kScrollBarInset in from the
// window's edge, which is where a scrollbar belongs and is the same number either way.
constexpr int kScrollBarPageX = kSetupSetColX + kScrollBarX; // 1117
static_assert(kScrollBarPageX + kScrollBarW + kScrollBarInset == kSetupPageW,
              "the scrollbar must sit kScrollBarInset in from the PAGE's right edge");

// Which column of the setup page an x falls in. Hit-testing subtracts the column's origin exactly
// as the painter added it, so a handler written against its column's own coordinates never learns
// that it is not alone on the page. The boundary is the settings column's left edge: the cabinet
// column owns everything to its left, including the air under the shorter column.
constexpr bool inSetupSettingsColumn(float x)
{
    return x >= static_cast<float>(kSetupSetColX);
}
// Four rows, one per channel. There were nine while the pedalboard existed — these four, then
// its five footswitches, with half a row of air between the two halves because the halves did
// different things with a press. The footswitches, the gap and the footnote that explained the
// difference all left together. Spelled here in the editor's own vocabulary and checked against
// kMidiLearnRowCount, which is the processor's, at the one site that includes both.
constexpr int kMidiRowCount = kChannelToggleCount;
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
constexpr int kCaptureTextX = kCaptureNameX + kCaptureNameW + 12; // 222
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

// The top edge of one MIDI row. It is plain arithmetic now that the list is one uniform run, and
// it stays a function for the same reason contentY() is one: the painter, the hit test and the
// art audit have to agree about where a row is. It carried a gap in the middle of the list while
// the pedal rows were there, which was exactly the kind of detail two of the three would keep and
// the third would forget.
constexpr int midiRowY(int row)
{
    return kMidiRowY0 + row * kMidiRowPitch;
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
// The MIDI section's footnotes, 220 units up from where they sat: five pedal rows at
// kMidiRowPitch and the half-row of air above them left the list together.
constexpr int kSettingsFootnoteY = 704;
constexpr int kSettingsFootnote2Y = 722;
constexpr int kSettingsFootnoteSize = 12;
constexpr int kSettingsFootnoteCount = 2;

// Section 4: the output section. Three radio rows for the mode, then the input
// calibration pair beside each other on one row.
//
// Every y below is 238 units up from where it was: the 220 the pedal rows and their gap took
// with them, plus the 18 of the footnote that existed only to explain the difference between the
// two halves of a list that now has one half.
//
// It is last because it is set once when a rig is assembled, and it is here at
// all because it was previously nowhere: the plug-in was hard-wired to
// Normalized with no way to see or change it, while both plug-ins it descends
// from expose exactly these three controls.
constexpr int kOutputHeadingY = 750;
constexpr int kOutputRowY0 = 768;
constexpr int kOutputRowPitch = 26;
constexpr int kOutputRowH = 22;
constexpr int kOutputRowW = 300;
// The radio dot, as an offset from the row's left edge, and its text.
constexpr float kOutputDotCX = 10.0f;
constexpr float kOutputDotR = 6.0f;
constexpr float kOutputDotFillR = 3.0f;
constexpr int kOutputTextX = 26;
// The calibration row: a PILL toggle on the left and the dBu value box to its
// right. Only the TOGGLE is drawn as far as the loaded captures can honour it;
// the value beside it is the user's own interface level and is always live.
//
// A pill rather than the faceplate's bat, and that is a decision about WHERE
// this control is rather than about what it does. The bats on the head are amp
// hardware: they sit in a row under the channel dials, and the four below them
// are played. This one sits in a list of settings rows, at the bottom of a page
// that is read rather than performed, and a lever drawn in that company reads as
// a stray piece of another panel. The shape is the grandparent's own slide
// switch (the grandparent project NAMix); the colour is this project's gold rather than its
// azure. See gfx/widgets.h, which both the editor and panelrender draw it with.
//
// The pair sits well to the RIGHT of the column the rows above start in, and
// close to the value box, which is the one place on this page that breaks the
// left-column convention. It is deliberate and it is about what the row IS: the
// toggle and the level are one control in two halves - the level is the number,
// the toggle is whether it is applied - and drawn against the left margin they
// read as a fourth item in the radio list above with a box stranded off to the
// right. This is as far across as they go: the legend between them is 155 units
// of Michroma and the box's left edge is fixed, so the gap in front of it is
// what is left over.
constexpr int kCalRowY = 860;
constexpr int kCalPillW = 44, kCalPillH = 22;
constexpr int kCalToggleCX = kMidiRowX + 118;
constexpr int kCalToggleCY = kCalRowY + 20;
constexpr int kCalLabelX = kMidiRowX + 152;
constexpr int kCalValueX = kMidiRowX + 320;
constexpr int kCalValueW = 110;
constexpr int kCalValueH = 26;
constexpr int kCalValueY = kCalRowY + 7;
static_assert(kCalToggleCX - kCalPillW / 2 >= kMidiRowX,
              "the calibration pill must start no further left than the rows above it");
static_assert(kCalToggleCX + kCalPillW / 2 + 8 <= kCalLabelX,
              "the calibration pill must clear its own legend by a readable gap - the pill is "
              "nearly twice the width of the bat it replaced, so this is the clearance that moved");
static_assert(kCalLabelX < kCalValueX,
              "the calibration legend must start left of the value box it labels; whether it FITS "
              "in what is left is measured with real glyph ink by panelrender's text audit, which "
              "is the sharper of the two gates and the one that moves when the face does");
// One wheel click on the calibration level, in dB. Whole decibels: an interface's
// stated level is a round number and this is how a user lands on theirs.
//
// The wheel is now the SECOND way to set this, and the reason it is kept is that
// it is the only one that needs no keyboard: whether keys reach an embedded view
// is the host's policy (D14), so a field that is typed into has to have a way
// round it that is not a drag. It is not a fallback for the value being
// unreachable - it lands on whole decibels, which is what an interface states.
constexpr double kCalWheelDb = 1.0;
// The most characters the dBu field will hold. Long enough for "-60.0" and a
// little slack, short enough that nothing typed into it can outrun the box.
constexpr std::size_t kCalEditMaxChars = 8;
constexpr int kOutputFootnoteY = 906;
constexpr int kOutputFootnote2Y = kOutputFootnoteY + 18; // the MIDI footnotes' own pitch
static_assert(kOutputFootnote2Y + 22 == kSettingsPageH,
              "the output footnote block must keep the page's bottom margin - if a line is added "
              "or removed here, kSettingsPageH moves with it");

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
// There was a third footnote here. It existed only because the list had two
// halves that did different things with a press, and with the pedal rows gone
// every row selects a channel — so the note now has nothing to distinguish and
// saying it would be worse than silence.

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
// "not in EVERY capture" rather than the older "not in this channel's captures",
// which was true but read as "none of them have it" — and the case a user is most
// likely to hit is the opposite one, a single capture dropped into an otherwise
// complete folder. That is ModelBank's bank-wide AND: a mode is offered only when
// every entry can honour it, because a dial sweeping across an entry that cannot
// would step in level as it crossed. The rule is deliberate; what was missing was
// any way for a player to find it out.
constexpr const char *kOutputUnsupported = "  [not in every capture]";
constexpr const char *kOutputFootnote =
    "Greyed options need the metadata in every capture of the sounding channel's bank.";
// The second line is the reason, and it is here because the rule above is
// surprising without it: one file out of ten is enough to grey a whole channel.
constexpr const char *kOutputFootnote2 =
    "One capture without it greys the option, because the dial would step in level there.";

// --- File browser overlay ---------------------------------------------------
// Drawn over the cabinet page (the only page with anything to load), so it is
// sized to that page rather than to the head's.
constexpr int kBrowserX = 16;
constexpr int kBrowserY = 16;
constexpr int kBrowserW = kCabPageW - 2 * kBrowserX; // 534
constexpr int kBrowserH = kCabPageH - 2 * kBrowserY; // 428

// The settings page opens the same browser for its four capture rows, and it is
// a much taller page — the cabinet's card centred on it would leave 230 units of
// dimmed panel above and below, and the card is a LIST, so the height is the one
// dimension it can actually use. Inset the same 16 either side and give it the
// page it is drawn over.
constexpr int kCaptureBrowserX = 16;
constexpr int kCaptureBrowserY = 40;
constexpr int kCaptureBrowserW = kSettingsPageW - 2 * kCaptureBrowserX; // 534
constexpr int kCaptureBrowserH = kSettingsPageH - 2 * kCaptureBrowserY; // 866
// That height is a CEILING now, not the size: the settings page scrolls, so the
// window showing it may be shorter than the page, and the card is sized to the
// viewport instead (RationsEditorView::boundCaptureBrowser). This is the floor
// for that — the card's own header and footer plus three rows, below which it
// stops being a list and becomes a pair of buttons.
constexpr int kCaptureBrowserMinH = 160;

} // namespace geo
} // namespace Rations
