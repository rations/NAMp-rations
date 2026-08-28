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

// The pedalboard placeholder: a heading and a caption. It is small because that
// is all there is; it grows when the pedals do.
constexpr int kPedalPageW = 480;
constexpr int kPedalPageH = 220;

// Settings: four channel-level rows, four MIDI learn rows, and two footnotes.
// The levels are first because every user has four channels and only some own a
// footswitch.
constexpr int kSettingsPageW = 560;
constexpr int kSettingsPageH = 512;

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

// Rounded, not truncated, so these agree with the sizes the editor actually
// produces — constrainSize() rounds, and it is the single authority on what is
// a legal size.
constexpr int pageMinW(Page p)
{
    return static_cast<int>(pageSize(p).w * kScaleMin + 0.5);
}
constexpr int pageMinH(Page p)
{
    return static_cast<int>(pageSize(p).h * kScaleMin + 0.5);
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
constexpr int kTitleSize = 56;       // the wordmark; fitted to the mock's cap height
constexpr int kKnobLabelSize = 15;   // dial legends, main row
constexpr int kIoLabelSize = 15;     // Input / Output, same row as the toggles
constexpr int kToggleLabelSize = 11; // BYPASS
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

// --- The wordmark -----------------------------------------------------------
// Drawn as TEXT in Michroma, not blitted from a badge asset — the way the
// author's other plug-in draws its own name. Centred on the faceplate by
// measuring stringWidth at draw time, so the constant here is only the baseline.
// The mock's wordmark occupies x 462..659, y 81..123 with a 43 px cap height;
// kTitleSize is the Michroma size that reproduces that cap height and is checked
// by the offline render rather than assumed.
constexpr int kTitleBaselineY = 123;

// --- The main dial row (8), evenly spaced -----------------------------------
struct KnobSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy, r;
    const char *label;
    const char *unit; // appended to the drag readout (nullptr = none)
};
constexpr int kKnobCount = 8;
constexpr int kKnobR = 28;
constexpr int kKnobCY = 226;
// Measured off the mock: seven of the eight dials sit on a perfect 94 px grid
// whose first position is 251. The eighth — Clean — is drawn 11 px left of it,
// which is the only irregularity in the row and reads as a hand-placement slip
// rather than an intention, so the row is regularised onto the grid the other
// seven agree on. If that gap WAS deliberate, this is the constant to break.
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
    // channels do; Bass/Middle/Treble have none, because unlike the parent
    // plug-in the tone stack here is always on.
    {kNoiseGateThresholdId, kKnobX0 + 4 * kKnobPitch, kKnobCY, kKnobR, "Gate", "dB"},
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
// There is exactly ONE text row per dial, and it is this one. The mock has no
// permanent value row — the silkscreen legend is all it shows — so the readout
// that says where a dial has got to appears only while that dial is being
// dragged, and it appears HERE, replacing the legend for the duration.
//
// Below the dial is not available and never was: the bat switches sit on
// y = 275 with art 20 px either side of that, and the dials' own art ends at
// y = 254, so the gap between them is a single pixel. A value row underneath is
// drawn across the levers. Confirmed by looking at it rather than by arithmetic
// — the first version of this editor did exactly that.
constexpr int kKnobLabelDY = 45; // 226 - 181

// --- Channel / gate indicator LEDs (5), above the dial labels ---------------
// Red when the channel is the one sounding, black otherwise; the gate's LED
// follows its own toggle. Centred on their dials — the mock's are 1 to 3 px
// off, which is hand placement, not a design.
constexpr int kLedR = 9;
constexpr int kLedCY = 147;
constexpr int kLedCount = 5;

// --- Bat toggles under the first five dials ---------------------------------
// switch_up_ring.png / switch_down_ring.png are 112x184 and carry no meaning of
// their own; which frame a value selects is decided in the view.
struct ToggleSpec {
    Steinberg::Vst::ParamID id;
    int cx, cy;        // centre of the toggle art = the lever's PIVOT
    const char *label; // drawn centred under the art; nullptr = no label
    bool invert;       // true = parameter on means bat DOWN
};
constexpr int kToggleW = 24, kToggleH = 40;
// Level with the Input and Output dials, so the bottom of the panel reads as one
// row rather than three.
constexpr int kToggleCY = 275;
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
// real amp head has no all-channels-off position. The gate's switch is an
// ordinary boolean and is deliberately NOT on the MIDI path.
constexpr int kChannelToggleCount = 4;
constexpr int kToggleCount = 5;
constexpr ToggleSpec kToggles[kToggleCount] = {
    {kChannelId, kKnobs[0].cx, kToggleCY, nullptr, false},
    {kChannelId, kKnobs[1].cx, kToggleCY, nullptr, false},
    {kChannelId, kKnobs[2].cx, kToggleCY, nullptr, false},
    {kChannelId, kKnobs[3].cx, kToggleCY, nullptr, false},
    {kNoiseGateOnId, kKnobs[4].cx, kToggleCY, nullptr, false},
};

// --- Gear (settings) button, top-right of the faceplate ---------------------
// Opens Page::Settings, which is a page of its own rather than an overlay: the
// MIDI rows want a narrow window, and drawing them over the head page would put
// them in the middle of 1133 px of faceplate.
//
// Declared ahead of the bypass pair because the bypass LED is derived from it —
// see there.
constexpr int kGearCX = 981, kGearCY = 106, kGearR = 11;

// --- Bypass, in the empty band left of the wordmark -------------------------
// NOT IN THE MOCK. The author asked for a bypass toggle after the mock was
// drawn, and this band — between the input meter and the wordmark — is the only
// empty space on the faceplate large enough to hold one without disturbing a
// measured position.
//
// The LED IS THE GEAR'S MIRROR, and it is written that way rather than as a
// number: the author's instruction was that the light sit at the same distance
// from the input meter as the gear sits from the output meter, and since the
// two meter columns are placed symmetrically about kFaceCX (both at kSideDX),
// reflecting the gear through that centre is exactly that condition. Written as
// a literal 151 it would be a number that silently stops meaning anything the
// moment the gear or the meter columns move; written like this it cannot.
constexpr int kBypassLedCX = 2 * kFaceCX - kGearCX; // 151
constexpr int kBypassLedCY = kGearCY;
// The switch sits inboard of its lamp, so the row reads meter, light, switch —
// the same order, and the same gap, as before it moved.
constexpr int kBypassLedToToggleDX = 40;
constexpr int kBypassToggleCX = kBypassLedCX + kBypassLedToToggleDX; // 191
constexpr int kBypassToggleCY = kBypassLedCY;
constexpr ToggleSpec kBypassToggle = {kBypassId, kBypassToggleCX, kBypassToggleCY, "BYPASS",
                                      // Bypass on means the plug-in is OUT of
                                      // circuit, which on an amp is the bat down.
                                      true};

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
constexpr int kMeterW = 27, kMeterH = 146, kMeterY = 89;
constexpr MeterRect kInputMeter = {kInputMeterId, kSideCXL - kMeterW / 2, kMeterY, kMeterW, kMeterH,
                                   nullptr};
constexpr MeterRect kOutputMeter = {
    kOutputMeterId, kSideCXR - kMeterW / 2, kMeterY, kMeterW, kMeterH, nullptr};

// The Input and Output dials sit UNDER their meters, which is what the author
// asked for and what puts each level control beside the thing that reports it.
// Smaller than the main row (the mock draws them at 46 px, not 56).
constexpr int kIoKnobR = 23;
constexpr int kIoKnobCY = 275;
// Close under the dial rather than down on the piping: at kIoLabelSize the cap
// height is about 9 px, and a baseline left where a 25 px legend needed it
// leaves the word floating clear of the control it names.
constexpr int kIoLabelBaselineY = 312;
constexpr KnobSpec kIoKnobs[2] = {
    {kInputGainId, kSideCXL, kIoKnobCY, kIoKnobR, "Input", "dB"},
    {kOutputGainId, kSideCXR, kIoKnobCY, kIoKnobR, "Output", "dB"},
};

// --- Page buttons, bottom-centre-right of the faceplate ---------------------
struct ButtonSpec {
    int x, y, w, h;
    const char *label;
    Page target;
};
constexpr int kPageButtonW = 147, kPageButtonH = 30, kPageButtonY = 277;
constexpr int kPageButtonCount = 2;
// The seam between the two buttons is CENTRED ON A DIAL rather than left where
// the mock happened to put it (x 803/813, a 7 px near-miss on Middle, which is
// the kind of almost-aligned that reads as a mistake). Derived from the dial's
// own cx so the pair follows the row if the pitch ever changes.
//
// Middle and not Bass, though Bass is the one this looks like it should line up
// with: at 147 px wide the Pedalboard button would then run from 569 to 716 and
// sit on top of the Gate bat switch, whose hit box is 597..657 on the same row.
// Nothing narrower fixes it either — clearing the switch would cap the button at
// 59 px, and "Pedalboard" is 108 px at kPageButtonTextSize. The Gate switch is
// what owns that stretch of the row. panelrender checks the clearance.
constexpr int kPageButtonSeamGap = 10;
constexpr int kPageButtonSeamCX = kKnobs[6].cx; // Middle
constexpr ButtonSpec kPageButtons[kPageButtonCount] = {
    {kPageButtonSeamCX - kPageButtonSeamGap / 2 - kPageButtonW, kPageButtonY, kPageButtonW,
     kPageButtonH, "Pedalboard", Page::Pedalboard},
    {kPageButtonSeamCX + kPageButtonSeamGap / 2, kPageButtonY, kPageButtonW, kPageButtonH,
     "Cabinet", Page::Cabinet},
};

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
// A placeholder in this build. The overdrive, flanger, chorus, delay and reverb
// are built into the plug-in later; nothing here hosts anyone else's. The page
// is sized to the caption because that is the whole content — when there are
// pedals to draw, kPedalPageW/H grow with them and nothing else here changes.
constexpr int kPedalPlaceholderSize = 26;
constexpr int kPedalPlaceholderY = 112;
constexpr int kPedalCaptionSize = 14;
constexpr int kPedalCaptionDY = 28;

// --- Settings page ----------------------------------------------------------
// Two sections, each four rows: the channel trims, then MIDI learn. Levels are
// on top because every user has four channels to balance and only some own a
// footswitch, so the section that is always useful is the one that does not
// have to be scrolled past.
//
// Both sections share kMidiRowX / kMidiRowW / kMidiRowH / kMidiRowPitch and the
// same kMidiTextX for their second column, so the channel names and the controls
// beside them line up down the whole page rather than forming two grids that
// nearly agree.
constexpr int kSettingsHeadingSize = 18;
constexpr int kMidiRowX = 24;
constexpr int kMidiRowW = kSettingsPageW - 2 * kMidiRowX; // 512
constexpr int kMidiRowH = 32;
constexpr int kMidiRowPitch = 40;
constexpr int kMidiRowCount = kChannelToggleCount;
constexpr int kMidiRowTextSize = 15;

// Section 1: channel levels.
constexpr int kLevelHeadingY = 58;
constexpr int kLevelRowY0 = 76;
constexpr int kLevelRowCount = kChannelToggleCount;
// The slider's track, as an offset from the row's left edge. Starts at
// kMidiTextX so it begins where the MIDI section's binding text does.
constexpr int kLevelSliderX = 116;
constexpr int kLevelSliderW = 302;
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
// Within this many logical units of the centre, a drag snaps to exactly 0 dB.
// A trim whose default is the middle has to be returnable to the middle by hand;
// right-clicking the row does it exactly, and this makes dragging do it too.
constexpr float kLevelCentreSnap = 4.0f;
// One wheel click on a level row, in dB. Half a decibel: fine enough to land on
// a round number, coarse enough that the whole range is a manageable number of
// clicks away.
constexpr double kLevelWheelDb = 0.5;
// The levels' own footnote, between the two sections.
constexpr int kLevelFootnoteY = 248;

// Section 2: MIDI learn. The gate is deliberately absent from it: the gate is not
// on the MIDI path at all.
constexpr int kSettingsHeadingY = 278;
constexpr int kMidiRowY0 = 296;
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
constexpr int kSettingsFootnoteY = 470;
constexpr int kSettingsFootnote2Y = 488;
constexpr int kSettingsFootnoteSize = 12;

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

// --- File browser overlay ---------------------------------------------------
// Drawn over the cabinet page (the only page with anything to load), so it is
// sized to that page rather than to the head's.
constexpr int kBrowserX = 16;
constexpr int kBrowserY = 16;
constexpr int kBrowserW = kCabPageW - 2 * kBrowserX; // 608
constexpr int kBrowserH = kCabPageH - 2 * kBrowserY; // 428

} // namespace geo
} // namespace Rations
