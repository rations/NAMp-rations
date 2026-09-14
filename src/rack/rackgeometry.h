// Rack strip geometry.
//
// The rack sits directly under the amp's own editor in one window, and the two share a single
// logical canvas: the window applies one cairo_scale(s, s) where s = windowWidth / kRackW, and the
// rack is drawn in the same logical units the editor is. That is why kRackW is asserted equal to
// the editor's canvas width rather than being a second independent number — if they ever disagreed
// the strip would drift out of alignment with the panel above it at every scale but 1.0.
//
// WIDTH BELONGS TO THE WINDOW, HEIGHT BELONGS TO THE PAGE, which is this project's own resolution
// of a conflict between the two designs it is built out of. The editor has two pages of different
// heights and the amp it was ported from asked the host for a new WIDTH on every page change; the
// strip below it cannot survive that, because its list rows are laid out against kRackW. So the
// top-level window takes one width — this one — a page change moves only the bottom edge, and the
// strip follows vertically and never sideways. kWindowH below is therefore the HEAD page's window
// height and not a fixed one: the setup page makes the same window taller by its own difference.
//
// EVERYTHING HERE IS IN LOGICAL UNITS. Layout lives in logical units in one header and scaling is a
// single cairo_scale at compose time, so a scale factor must never be baked into a constant here;
// mouse coordinates are divided by the scale before they reach the hit-testing.
//
// Two views share these constants because they show the same chain:
//
//   * LIST — the routed path, top to bottom, one row per node, with the amp fixed in the middle
//     where the signal turns from mono to stereo. Reordering is up/down on a row.
//   * NODES — the same path laid out left to right as cards joined by cables, with unrouted nodes
//     parked on a shelf below it. Dropping a card on a cable splices it in; dropping it on the
//     shelf, or right-clicking a cable, takes it out.
//
// The list is the primary view. It is the one that can express every edit unambiguously, it needs
// no pointer precision, and it is what a rack of ten pedals is actually readable in.

#pragma once

#include "geometry.h"

namespace NAMp
{
namespace rackgeo
{

//--- the strip -----------------------------------------------------------
// Same logical width as the editor: one canvas, one scale.
constexpr int kRackW = 1133;
static_assert(kRackW == Rations::geo::kWinW,
              "the rack and the editor must share one logical canvas width");

// Tall enough for six list rows plus the header and the footer, which is a deeper rack than
// a serial chain on one core has any business running.
constexpr int kRackH = 260;

// The whole window with the HEAD page showing, editor above and rack below. Every other page is
// this plus its own extra height; see the note at the top of this file.
constexpr int kWindowW = kRackW;
constexpr int kWindowH = Rations::geo::kWinH + kRackH;

//--- palette -------------------------------------------------------------
// Deliberately NOT the faceplate's photographic browns: the rack is host chrome, not part of the
// amp, and it reads better as a dark deck the head is standing on. The accent is the editor's, so
// the two still belong to one product.
constexpr uint32_t kRackBg = 0x0E0D0D;
constexpr uint32_t kRackTopEdge = 0x2A2624; // hairline between the editor and the rack
constexpr uint32_t kPanelBg = 0x161314;
constexpr uint32_t kPanelBorder = 0x2E2A28;
constexpr uint32_t kRowBgOdd = 0x1C1819;
constexpr uint32_t kRowBgEven = 0x191516;
constexpr uint32_t kRowBgHover = 0x24201F;
constexpr uint32_t kAnchorBg = 0x1B2419; // the amp's own row/card: green-tinted, not neutral
constexpr uint32_t kCableColor = 0x4B5F49;
constexpr uint32_t kCableLive = Rations::geo::kAccent;
constexpr uint32_t kOffColor = 0x6A6460;  // a node that is out of the path
constexpr uint32_t kIconColor = 0x9A9490; // control glyphs at rest
constexpr uint32_t kIconHot = 0xFFFFFF;   // ...and under the pointer
constexpr uint32_t kDangerColor = 0xFF3B30;

//--- header --------------------------------------------------------------
constexpr float kHeaderH = 30.0f;
constexpr float kMargin = 14.0f;
constexpr float kTitleSize = 13.0f;
constexpr float kLabelSize = 12.0f;
constexpr float kSmallSize = 10.0f;

// The footer's card-view hint, and the least room the device line is allowed to be left with.
//
// HERE RATHER THAN AT THE DRAW SITE so the offline audit measures the string the strip actually
// paints, which is the same reason the panel's own legends live beside its geometry. The footer is
// three pieces on one line — the pedal count, this hint, and the audio device between them — and
// the device line is clipped to whatever the other two leave. Clipping means it can never overlap
// them; what it cannot do on its own is notice that the gap has shrunk to nothing and the device
// has silently stopped being reported. That is what the minimum is for, and why the audit measures
// it at the width the strip is actually drawn at rather than trusting the three to fit.
constexpr const char *kFooterNodesHint = "drag a card onto a cable to splice it in  ·  right-click "
                                         "a cable to take the pedal after it out";
// Enough for a backend's name and its rate — "WASAPI exclusive, 48000 Hz" — so that a clipped line
// still says which device is sounding, which is the question it exists to answer.
constexpr float kFooterDeviceMinW = 150.0f;

// The one gap between two adjacent header buttons. Every pair uses it — "+ Before"/"+ After",
// Scan/Presets, Presets/the toggle, and the toggle's own two segments — so it is a constant rather
// than a literal repeated at each pair. A pair that touched would read as one control with a seam
// down it, which is exactly what the view toggle used to read as.
constexpr float kBtnGap = 8.0f;

// Presets, top-right and left of the view toggle. A rack that cannot be saved from the rack is a
// rack you have to quit to keep, so this is chrome rather than a nicety.
constexpr float kPresetsW = 72.0f;

// Scan, left of Presets. Discovery happens by itself at every launch, so this is not the only way
// to find a plug-in — it is how you find one you installed while the rack was already running, and
// how you get at the list of folders being searched.
constexpr float kScanW = 60.0f;

// Audio, left of Scan. THE DEVICE BELONGS TO THE HOST AND NOT TO THE AMP, which is why the picker
// is here rather than on the amp's own setup page: the amp is a plug-in that also runs inside a
// DAW, where it has no device to choose and the question does not arise. The strip is where this
// program's own settings live — the plug-in search paths and the saved racks are already here for
// the same reason — so the interface it opens the device with belongs beside them.
constexpr float kAudioW = 62.0f;

// View toggle, top-right: two pills, "List" and "Nodes". kToggleW is the pair's total extent, so
// moving the pair does not depend on knowing how it is divided; kToggleSegW is one segment.
constexpr float kToggleW = 116.0f;
constexpr float kToggleH = 18.0f;
constexpr float kToggleSegW = (kToggleW - kBtnGap) * 0.5f;
constexpr float kToggleX = kRackW - kMargin - kToggleW;
constexpr float kToggleY = (kHeaderH - kToggleH) * 0.5f;
constexpr float kToggleNodesX = kToggleX + kToggleSegW + kBtnGap;
constexpr float kPresetsX = kToggleX - kBtnGap - kPresetsW;
constexpr float kPresetsY = kToggleY;
constexpr float kScanX = kPresetsX - kBtnGap - kScanW;
constexpr float kScanY = kToggleY;
constexpr float kAudioX = kScanX - kBtnGap - kAudioW;
constexpr float kAudioY = kToggleY;

// "+ Before" / "+ After", top-left. Adding is per section because the section fixes a node's
// channel count for the life of the instance (mono before the amp, stereo after it), so it is not
// a property the user can change later by dragging.
constexpr float kAddW = 74.0f;
constexpr float kAddH = 18.0f;
constexpr float kAddY = kToggleY;
// Clear of the "RACK" wordmark. The title face is Michroma, which is far wider per character than
// the body face, so this cannot be eyeballed from the letter count — rackrender measures the drawn
// string against this constant and fails if they ever collide.
constexpr float kTitleGutter = 76.0f;
constexpr float kAddBeforeX = kMargin + kTitleGutter;
constexpr float kAddAfterX = kAddBeforeX + kAddW + kBtnGap;

// Where the anchor row's subtitle starts, measured from the row's left inset. Same reasoning: the
// amp's wordmark is set in the title face and rackrender checks it fits.
constexpr float kAnchorSubtitleDX = 78.0f;

//--- list view -----------------------------------------------------------
constexpr float kListX = kMargin;
constexpr float kListY = kHeaderH + 6.0f;
constexpr float kListW = kRackW - 2.0f * kMargin;
constexpr float kListH = kRackH - kListY - 26.0f; // leaves the footer its line
constexpr float kRowH = 30.0f;
constexpr float kRowGap = 2.0f;
constexpr float kRowRadius = 5.0f;

// Row layout, left to right. The name gets whatever the controls do not.
constexpr float kOrderW = 26.0f;  // "1", "2", ... or the anchor's rule
constexpr float kEnableW = 26.0f; // the in-circuit dot
constexpr float kCtlW = 26.0f;    // one square control cell
constexpr int kRowCtlCount = 4;   // up, down, editor, remove

// The wet/dry slider, sitting immediately left of the control cells. There is NO per-node latency
// column: a node's own latency is not something anyone acts on — the chain compensates the dry path
// with it automatically, and the rack's total is in the footer — so a number that changes nothing
// was taking the width the one control on the row actually needs.
//
// "dry" at one end, "wet" at the other, because a bare track with a dot on it says a value is being
// set and not which way round it is — and the two ends are opposite claims about what the pedal is
// doing, not more and less of one thing. The labels are inside the slider's hit rectangle
// deliberately: clicking "dry" is the fastest way to get to 0, and clicking "wet" to 1, which are
// the two values anyone actually reaches for.
//
// rackrender measures the drawn strings against kMixLabelW and fails if they ever outgrow it, the
// same guard the title gutter gets — these are two more constants that cannot be eyeballed from a
// letter count.
constexpr float kMixTrackW = 110.0f;
constexpr float kMixLabelW = 22.0f;
constexpr float kMixLabelGap = 6.0f;
constexpr float kMixPadR = 10.0f; // clear of the control cells
constexpr float kMixW = 2.0f * (kMixLabelW + kMixLabelGap) + kMixTrackW + kMixPadR;

//--- the diagnostics cost readout ---------------------------------------
// Drawn only when diagnostics are armed, and drawn INSIDE the row rather than beside it: the strip
// has no spare width, and a diagnostic that must not move a control the moment someone sets an
// environment variable. The bar rides the row's bottom edge; the number takes the right-hand end of
// the NAME column, which is the only place with room to give and the only thing on the row that can
// give it — a name is clipped to fit, a control is not. So arming diagnostics shortens the longest
// plug-in names by kDiagTextW and moves nothing.
constexpr float kDiagTextW = 62.0f;
constexpr float kDiagBarH = 2.0f;
constexpr float kDiagBarInset = 6.0f;
// A serial chain has one core, so these are fractions of ONE callback's budget, not of a machine.
// Past three quarters there is no headroom left for the next pedal, which is the thing worth
// seeing before an xrun rather than after one.
constexpr float kDiagWarnFrac = 0.35f;
constexpr float kDiagHotFrac = 0.75f;
constexpr uint32_t kDiagOkColor = 0x4E9A4E;
constexpr uint32_t kDiagWarnColor = 0xC8A02E;
constexpr uint32_t kDiagHotColor = 0xFF3B30;

//--- node view -----------------------------------------------------------
constexpr float kCardW = 118.0f;
// Tall enough for the wet/dry slider as well as the name, the side and the bottom row of controls.
// The node view carries the same controls as the list, because a view you have to leave to change a
// value is a view you cannot work in — and the mix is the one continuous value a pedal has here.
constexpr float kCardH = 82.0f;
constexpr float kCardRadius = 7.0f;
constexpr float kCardGap = 34.0f;   // the cable run between two cards
constexpr float kCardMixDY = 52.0f; // the slider's centre line, from the card's top
constexpr float kCardMixInset = 8.0f;
constexpr float kCardMixBandH = 16.0f; // what counts as aiming at it
// Cards shrink when the chain is long. Below this the end labels are dropped rather than the track:
// an unlabelled slider is worse than a labelled one, and a slider too short to drag is worse than
// both.
constexpr float kCardMixMinTrackW = 40.0f;
// The remove cross, top-right of every card that stands for a pedal. The node view could take a
// pedal out of the routed path but never out of the rack, so a pedal added there had to be deleted
// from the list — and a shelved card, which is the one you are most likely to be finished with, had
// no control on it at all. Drawn on shelf cards too for that reason.
constexpr float kCardRemoveSize = 10.0f;
constexpr float kCardRemoveDX = 7.0f; // right edge of the cross, from the card's right edge
constexpr float kCardRemoveDY = 6.0f;
constexpr float kCardRemoveHitPad = 4.0f; // 10 px of glyph is not 10 px of target
constexpr float kPathY = kHeaderH + 26.0f;
constexpr float kShelfY = kPathY + kCardH + 34.0f;
constexpr float kPortR = 4.0f;
// How far the cable leaves a port horizontally before it starts bending. A third of the gap keeps
// the S-curve inside its own run at every card spacing the strip can produce.
constexpr float kCableSlack = kCardGap * 0.55f;
// A click within this of a cable's midpoint counts as hitting it.
constexpr float kCableHitR = 9.0f;
// The terminals: where the guitar goes in and the speakers come out.
constexpr float kTerminalW = 40.0f;

//--- picker overlay ------------------------------------------------------
// Covers the rack only, never the editor: the amp stays visible and playable while a pedal is
// being chosen.
constexpr float kPickerX = 90.0f;
constexpr float kPickerY = 8.0f;
constexpr float kPickerW = kRackW - 2.0f * kPickerX;
constexpr float kPickerH = kRackH - 2.0f * kPickerY;
constexpr float kPickerHeaderH = 34.0f;
constexpr float kPickerRowH = 22.0f;
// The preset overlay reuses the picker's box and rows, with two action rows above the list: save
// over the rack that is loaded, then save a new one under a name typed into the row. TWO rows and
// not one, because a single row that did both could only say so in a hint - and the hint it had
// ("click to rename") described the wrong operation, which is a fair reading of a row whose label
// names an existing preset. A row that says "New preset" is the feature; a hint is not.
constexpr int kPresetSaveRow = 0;
constexpr int kPresetNewRow = 1;
constexpr int kPresetFirstRow = 2;

// The search-path overlay reuses it too, with two action rows above the list: scan, then add. Both
// are rows rather than floating buttons for the reason the save row is — the box already scrolls,
// and a button laid out around a scrolling list has to be laid out around a scrolling list.
constexpr int kPathScanRow = 0;
constexpr int kPathAddRow = 1;
constexpr int kPathFirstRow = 2;
// The remove cross on a user row, measured from the row's right edge.
constexpr float kPathRemoveDX = 20.0f;

// The delete control on a saved-preset row: the WORD, not a cross. A cross is the right glyph for
// forgetting a search path, which is what the row above uses it for, but deleting a preset unlinks
// a file the user built and there is no undo anywhere in this program — so it says what it does.
// One box, used by the painter and by the hit test, so the label and its target cannot drift apart;
// rackrender asserts the words fit inside it.
constexpr float kPresetDeleteW = 46.0f;
constexpr float kPresetDeleteDX = 12.0f; // from the row's right edge to the box's right edge

// The folder browser opens in exactly the picker's box (see RackView::browserBounds): the box is
// already the overlay, and a second overlay of a different size reads as a different application.

//--- scan progress -------------------------------------------------------
// Drawn across the picker's header while a scan runs. A scan is synchronous — the run loop is
// inside it — so this is the only thing on screen that says the application is still alive.
constexpr float kScanBarH = 3.0f;

} // namespace rackgeo
} // namespace NAMp
