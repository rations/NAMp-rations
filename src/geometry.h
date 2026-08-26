// Rations editor geometry.
//
// The canvas size and palette are mirrored from gui/geometry.sh (the art
// pipeline's source of truth); keep the two in sync by hand. The control rects
// are placed against the panel art and against the author's mock rather than
// against a grid: the faceplate rectangle below is the measured inside of the
// gold piping in the amp-head photograph, and every control position here was
// measured out of mock-head.png / mock-cabinet.png by connected-component scan
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

// Editor canvas (geometry.sh: WIN_W/WIN_H). This is exactly the trimmed size of
// head.png, so at scale 1.0 the panel is a pixel-exact blit with no resampling.
constexpr int kWinW = 1133;
constexpr int kWinH = 403;

// Host resize range, as a multiple of the canvas. The art is the ceiling: the
// head is 1133 px wide in the source, so anything above 1.0 is a genuine
// upscale and 1.5 is where a photographic faceplate stops holding up.
constexpr double kScaleMin = 0.66;
constexpr double kScaleMax = 1.50;
// Rounded, not truncated, so these agree with the sizes the editor actually
// produces — constrainSize() rounds, and it is the single authority on what is
// a legal size. These four are the documented range.
constexpr int kMinW = static_cast<int>(kWinW * kScaleMin + 0.5); // 748
constexpr int kMinH = static_cast<int>(kWinH * kScaleMin + 0.5); // 266
constexpr int kMaxW = static_cast<int>(kWinW * kScaleMax + 0.5); // 1700
constexpr int kMaxH = static_cast<int>(kWinH * kScaleMax + 0.5); // 605

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

// --- Pages ------------------------------------------------------------------
// A page is a VIEW, not a parameter: it is editor-local state and is deliberately
// never persisted or automated, because a host recalling a preset must not also
// recall which panel the user happened to be looking at.
enum class Page { Head, Cabinet, Pedalboard };

// --- The wordmark -----------------------------------------------------------
// Drawn as TEXT in Michroma, not blitted from a badge asset — the way the
// author's other plug-in draws its own name. Centred on the faceplate by
// measuring stringWidth at draw time, so the constant here is only the baseline.
// The mock's wordmark occupies x 462..659, y 81..123 with a 43 px cap height;
// kTitleSize is the Michroma size that reproduces that cap height and is checked
// by the offline render rather than assumed.
constexpr int kTitleBaselineY = 123;
constexpr float kTitleSize = 56.0f;

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

// Label baseline ABOVE the face centre (the mock puts all eight on y = 181),
// value readout BELOW it. The mock has no permanent value row — the silkscreen
// label is all it shows — so the readout appears only while a dial is being
// dragged, which is what tells the user where they have got to without adding a
// second permanent row of text the design does not have.
constexpr int kKnobLabelDY = 45; // 226 - 181
constexpr int kKnobValueDY = 45;
constexpr int kKnobLabelSize = 25;
constexpr int kKnobValueSize = 14;

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
constexpr int kToggleLabelSize = 12;
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

// --- Bypass, in the empty band left of the wordmark -------------------------
// NOT IN THE MOCK. The author asked for a bypass toggle after the mock was
// drawn, and this band — between the input meter and the wordmark — is the only
// empty space on the faceplate large enough to hold one without disturbing a
// measured position. The LED sits to its left on the lever's pivot line, which
// is the parent plug-in's arrangement. Say so if it belongs somewhere else;
// nothing else depends on these four numbers.
constexpr int kBypassToggleCX = 208;
constexpr int kBypassToggleCY = 102;
constexpr int kBypassLedCX = 168;
constexpr int kBypassLedCY = kBypassToggleCY;
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
constexpr int kIoLabelBaselineY = 320;
constexpr int kIoLabelSize = 25;
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
constexpr int kPageButtonTextSize = 20;
constexpr int kPageButtonCount = 2;
constexpr ButtonSpec kPageButtons[kPageButtonCount] = {
    {656, kPageButtonY, kPageButtonW, kPageButtonH, "Pedalboard", Page::Pedalboard},
    {813, kPageButtonY, kPageButtonW, kPageButtonH, "Cabinet", Page::Cabinet},
};

// The way back, drawn on every page that is not the head. Top-left, clear of
// the cabinet art and of the pedalboard placeholder.
constexpr ButtonSpec kBackButton = {40, 20, 130, 30, "Amp", Page::Head};

// --- Gear (settings) button, top-right of the faceplate ---------------------
constexpr int kGearCX = 981, kGearCY = 106, kGearR = 11;

// --- Cabinet page -----------------------------------------------------------
// The cabinet page does NOT wear the amp head's faceplate: it is a picture of a
// different object, so it is drawn on the same dark ground the head's letterbox
// uses. That also buys the full 403 px of canvas height instead of the 271 px
// inside the piping, which matters — the canvas is the head's 2.81:1 and the
// cabinet art is 1.70:1, so this page is height-limited and every pixel of
// height is 1.7 px of cabinet.
//
// The cabinet is drawn as tall as the canvas allows above the loader rows, and
// the two rows are then sized so the pair spans EXACTLY the cabinet's width.
// That is what stops the page reading as a small picture with two unrelated
// widgets under it: cabinet and rows are one column, and the black either side
// is margin rather than a gap something should have filled.
constexpr int kCabH = 340;
constexpr int kCabW = 578;                 // round(kCabH * 1483 / 872), the art's own aspect
constexpr int kCabX = kFaceCX - kCabW / 2; // 277
constexpr int kCabY = 8;

// The Blend dial is drawn OVER the knob painted into the cabinet art, at the
// same place and a shade larger so it covers it rather than sitting beside it.
// The two fractions are measured off the source art (knob centre 739/1483 and
// 634/872 of the trimmed cabinet), so the dial follows the art if the cabinet is
// ever drawn at a different size.
constexpr float kCabBlendFX = 739.0f / 1483.0f;
constexpr float kCabBlendFY = 634.0f / 872.0f;
constexpr float kCabBlendLabelFY = 703.0f / 872.0f;
constexpr int kBlendCX = kCabX + static_cast<int>(kCabBlendFX * kCabW + 0.5f);
constexpr int kBlendCY = kCabY + static_cast<int>(kCabBlendFY * kCabH + 0.5f);
constexpr int kBlendR = 14;
// The dial is small because the cabinet is small — see above. The hit box is
// not: it is sized for a finger on a trackpad, not for the art.
constexpr int kBlendHitR = 24;
constexpr int kBlendLabelBaselineY = kCabY + static_cast<int>(kCabBlendLabelFY * kCabH + 0.5f);
constexpr int kBlendLabelSize = 14;

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
constexpr int kIrRowY = kCabY + kCabH + 10; // 358
constexpr int kIrRowGap = 18;
constexpr int kIrRowW = (kCabW - kIrRowGap) / 2; // 280
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
// are built into the plug-in later; nothing here hosts anyone else's.
constexpr int kPedalPlaceholderSize = 26;
constexpr int kPedalPlaceholderY = 200;

// --- Settings overlay -------------------------------------------------------
constexpr int kSettingsX = 300, kSettingsY = 80, kSettingsW = 533, kSettingsH = 250;

// --- File browser overlay ---------------------------------------------------
constexpr int kBrowserX = 180, kBrowserY = 56, kBrowserW = 773, kBrowserH = 291;

} // namespace geo
} // namespace Rations
