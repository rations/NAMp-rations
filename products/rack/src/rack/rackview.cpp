// RackView implementation. See rackview.h for the shared-layout rule this file rests on.

#include "rackview.h"
#include "rackgeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace NAMp::rack
{

using namespace rackgeo;

// The rack's accent is the editor's, so the strip and the panel above it read as one product rather
// than as a host with somebody else's amp inside it. Font is the editor's too: one set of faces
// across the whole window.
namespace geo = Rations::geo;
using Rations::Font;

namespace
{

//------------------------------------------------------------------------
// One row of the list view. `node` is null for the anchor, which is NAMp itself: it is drawn in the
// list because the chain is not readable without knowing where the amp sits, and it carries no
// controls because it cannot be moved, bypassed from here, or removed.
struct ListRow {
    const RackNode *node = nullptr;
    bool anchor = false;
    Rect rect;
};

//------------------------------------------------------------------------
struct CardBox {
    const RackNode *node = nullptr;
    bool anchor = false;
    Rect rect;
};

//------------------------------------------------------------------------
// A cable between two things on the path, and the slot a card dropped on it would take.
struct Cable {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    host::ChainSection section = host::ChainSection::Pre;
    int slot = 0;

    float midX() const
    {
        return (x0 + x1) * 0.5f;
    }
    float midY() const
    {
        return (y0 + y1) * 0.5f;
    }
};

//------------------------------------------------------------------------
struct NodeLayout {
    std::vector<CardBox> path;  // terminals excluded, anchor included
    std::vector<CardBox> shelf; // everything routed out
    std::vector<Cable> cables;
    Rect inTerm, outTerm;
    float cardW = kCardW;
};

//------------------------------------------------------------------------
float rowPitch()
{
    return kRowH + kRowGap;
}

int visibleListRows()
{
    return static_cast<int>(kListH / rowPitch());
}

//------------------------------------------------------------------------
// The i-th control cell from the RIGHT edge of a row. Right-aligned so the name column absorbs
// every width change and the controls never move.
Rect rowCtlRect(const Rect &row, int fromRight)
{
    const float x = row.right() - (fromRight + 1) * kCtlW;
    return Rect(x, row.y, kCtlW, row.h);
}

Rect rowMixRect(const Rect &row)
{
    const float x = row.right() - kRowCtlCount * kCtlW - kMixW;
    return Rect(x, row.y, kMixW, row.h);
}

// The track itself, inset from the mix cell by the "dry" and "wet" labels. Drawing and hit-testing
// both go through this: they disagreed by the cell's right padding before it existed, so the dot
// sat a couple of pixels off wherever the pointer had put it.
Rect rowMixTrackRect(const Rect &row)
{
    const Rect mix = rowMixRect(row);
    return Rect(mix.x + kMixLabelW + kMixLabelGap, mix.centerY() - 2.0f, kMixTrackW, 4.0f);
}

// The same slider on a card. A card is a seventh of a row's width, so the end labels are the first
// thing to go when a long chain squeezes the cards — the track keeps its width and loses its names,
// rather than keeping its names and becoming undraggable.
bool cardMixLabelled(const Rect &card)
{
    const float inner = card.w - 2.0f * kCardMixInset;
    return inner - 2.0f * (kMixLabelW + kMixLabelGap) >= kCardMixMinTrackW;
}

Rect cardMixTrackRect(const Rect &card)
{
    const float inner = card.w - 2.0f * kCardMixInset;
    const float w = cardMixLabelled(card) ? inner - 2.0f * (kMixLabelW + kMixLabelGap) : inner;
    return Rect(card.centerX() - w * 0.5f, card.y + kCardMixDY - 2.0f, w, 4.0f);
}

// What counts as aiming at it: the full inner width, so the labels are part of the target here for
// the same reason they are in the list.
Rect cardMixBandRect(const Rect &card)
{
    return Rect(card.x + kCardMixInset, card.y + kCardMixDY - kCardMixBandH * 0.5f,
                card.w - 2.0f * kCardMixInset, kCardMixBandH);
}

// A card only carries controls at full height; the shelf draws them shorter, and a pedal that is
// out of circuit has no mix to set anyway.
bool cardHasControls(const Rect &card)
{
    return card.h > kCardH * 0.8f;
}

// The remove cross, on every card that stands for a pedal — the shelf included, since a card that
// is out of circuit is the one you are most likely to be finished with. Not gated on
// cardHasControls() for exactly that reason.
Rect cardRemoveRect(const Rect &card)
{
    return Rect(card.right() - kCardRemoveDX - kCardRemoveSize, card.y + kCardRemoveDY,
                kCardRemoveSize, kCardRemoveSize);
}

Rect rowEnableRect(const Rect &row)
{
    return Rect(row.x + kOrderW, row.y, kEnableW, row.h);
}

Rect rowNameRect(const Rect &row)
{
    const float x = row.x + kOrderW + kEnableW;
    return Rect(x, row.y, rowMixRect(row).x - x - 8.0f, row.h);
}

//------------------------------------------------------------------------
// Build the list: every pre node in order, the anchor, then every post node. Disabled nodes are
// included — the list is the whole rack, not just what is sounding, and a bypassed pedal you cannot
// see is a pedal you cannot un-bypass.
void layoutList(const RackModel &model, std::vector<ListRow> &out, int &totalRows)
{
    out.clear();

    std::vector<ListRow> all;
    for (const RackNode *node : model.section(host::ChainSection::Pre, false))
        all.push_back({node, false, Rect()});
    all.push_back({nullptr, true, Rect()});
    for (const RackNode *node : model.section(host::ChainSection::Post, false))
        all.push_back({node, false, Rect()});

    totalRows = static_cast<int>(all.size());

    const int rows = visibleListRows();
    const int maxScroll = std::max(0, totalRows - rows);
    const int scroll = std::min(model.listScroll(), maxScroll);

    for (int i = 0; i < rows; ++i) {
        const int index = scroll + i;
        if (index >= totalRows)
            break;
        ListRow row = all[static_cast<size_t>(index)];
        row.rect = Rect(kListX, kListY + i * rowPitch(), kListW, kRowH);
        out.push_back(row);
    }
}

//------------------------------------------------------------------------
// Lay the routed path out left to right. The card width shrinks before the gap does, because a
// narrower cable run reads as a tighter board while a narrower card starts losing the name — but
// both have a floor, and past that point the path is clipped rather than squeezed into
// illegibility.
void layoutNodes(const RackModel &model, NodeLayout &out)
{
    out = NodeLayout();

    const std::vector<const RackNode *> pre = model.section(host::ChainSection::Pre, true);
    const std::vector<const RackNode *> post = model.section(host::ChainSection::Post, true);
    const int cards = static_cast<int>(pre.size() + post.size()) + 1; // + the anchor

    const float avail = kRackW - 2.0f * kMargin - 2.0f * kTerminalW;
    float gap = kCardGap;
    float cardW = kCardW;
    // cards * cardW + (cards + 1) * gap <= avail
    if (cards * cardW + (cards + 1) * gap > avail) {
        gap = std::max(12.0f, (avail - cards * cardW) / (cards + 1));
        if (cards * cardW + (cards + 1) * gap > avail)
            cardW = std::max(64.0f, (avail - (cards + 1) * gap) / cards);
    }
    out.cardW = cardW;

    const float pitch = cardW + gap;
    const float y = kPathY;
    float x = kMargin;

    out.inTerm = Rect(x, y + kCardH * 0.5f - 9.0f, kTerminalW, 18.0f);
    x += kTerminalW + gap;

    auto emit = [&](const RackNode *node, bool anchor) {
        CardBox box;
        box.node = node;
        box.anchor = anchor;
        box.rect = Rect(x, y, cardW, kCardH);
        out.path.push_back(box);
        x += pitch;
    };

    for (const RackNode *node : pre)
        emit(node, false);
    emit(nullptr, true);
    for (const RackNode *node : post)
        emit(node, false);

    // `x` has already advanced one full pitch past the last card, which puts it exactly one gap
    // clear of that card's right edge — where the output terminal belongs.
    out.outTerm = Rect(x, y + kCardH * 0.5f - 9.0f, kTerminalW, 18.0f);

    // Cables: one before each card and one after the last. The slot a drop lands in is the index
    // within that side's routed list, so the cable before pre-card k is slot k, and the cable
    // before post-card k is slot k of the post list.
    const float portY = y + kCardH * 0.5f;
    const int preCount = static_cast<int>(pre.size());
    float prevRight = out.inTerm.right();

    for (size_t i = 0; i < out.path.size(); ++i) {
        const CardBox &box = out.path[i];
        Cable cable;
        cable.x0 = prevRight;
        cable.y0 = portY;
        cable.x1 = box.rect.left();
        cable.y1 = portY;
        // A cable BEFORE the anchor, or before any pre card, is a pre slot. Everything from the
        // anchor onwards is a post slot.
        if (static_cast<int>(i) <= preCount) {
            cable.section = host::ChainSection::Pre;
            cable.slot = static_cast<int>(i);
        } else {
            cable.section = host::ChainSection::Post;
            cable.slot = static_cast<int>(i) - preCount - 1;
        }
        out.cables.push_back(cable);
        prevRight = box.rect.right();
    }

    Cable tail;
    tail.x0 = prevRight;
    tail.y0 = portY;
    tail.x1 = out.outTerm.left();
    tail.y1 = portY;
    tail.section = host::ChainSection::Post;
    tail.slot = static_cast<int>(post.size());
    out.cables.push_back(tail);

    // The shelf: everything the user has routed out, parked in the order it sits in its section.
    float sx = kMargin + kTerminalW + gap;
    for (const auto s : {host::ChainSection::Pre, host::ChainSection::Post}) {
        for (const RackNode *node : model.section(s, false)) {
            if (node->enabled)
                continue;
            CardBox box;
            box.node = node;
            box.rect = Rect(sx, kShelfY, cardW, kCardH * 0.62f);
            out.shelf.push_back(box);
            sx += cardW + gap;
        }
    }
}

//------------------------------------------------------------------------
Rect pickerRowRect(int visibleIndex)
{
    return Rect(kPickerX + 1.0f, kPickerY + kPickerHeaderH + visibleIndex * kPickerRowH,
                kPickerW - 2.0f, kPickerRowH);
}

int pickerVisibleRows()
{
    return static_cast<int>((kPickerH - kPickerHeaderH - 8.0f) / kPickerRowH);
}

Rect pickerCloseBox()
{
    return Rect(kPickerX + kPickerW - 28.0f, kPickerY + 11.0f, 14.0f, 14.0f);
}

// The remove cross on a user-added search-path row.
Rect pathRowRemoveBox(const Rect &row)
{
    return Rect(row.right() - kPathRemoveDX, row.centerY() - 5.0f, 10.0f, 10.0f);
}

// The delete control on a saved-preset row. A word rather than a cross — see kPresetDeleteW.
Rect presetRowRemoveBox(const Rect &row)
{
    return Rect(row.right() - kPresetDeleteDX - kPresetDeleteW, row.centerY() - 7.0f,
                kPresetDeleteW, 14.0f);
}

// The folder browser fills the picker's box exactly — see rackgeometry.h.
Rect browserBounds()
{
    return Rect(kPickerX, kPickerY, kPickerW, kPickerH);
}

//------------------------------------------------------------------------
// Small glyphs, all drawn rather than loaded. Each fills the cell it is given.
void drawChevron(Canvas &c, const Rect &cell, bool up, uint32_t colour)
{
    const float cx = cell.centerX(), cy = cell.centerY();
    const float w = 5.0f, h = 3.5f;
    c.setColor(colour);
    c.setPenSize(1.6f);
    if (up) {
        c.strokeLine(cx - w, cy + h * 0.5f, cx, cy - h * 0.5f);
        c.strokeLine(cx, cy - h * 0.5f, cx + w, cy + h * 0.5f);
    } else {
        c.strokeLine(cx - w, cy - h * 0.5f, cx, cy + h * 0.5f);
        c.strokeLine(cx, cy + h * 0.5f, cx + w, cy - h * 0.5f);
    }
}

void drawCross(Canvas &c, const Rect &cell, uint32_t colour)
{
    const float cx = cell.centerX(), cy = cell.centerY(), r = 4.0f;
    c.setColor(colour);
    c.setPenSize(1.6f);
    c.strokeLine(cx - r, cy - r, cx + r, cy + r);
    c.strokeLine(cx - r, cy + r, cx + r, cy - r);
}

// A ring with teeth: enough of a gear at 26 px to read as "open this plug-in's own window".
void drawGear(Canvas &c, const Rect &cell, uint32_t colour, bool filled)
{
    const float cx = cell.centerX(), cy = cell.centerY();
    const float r = 5.0f;
    c.setColor(colour);
    if (filled) {
        c.fillEllipse(cx, cy, r, r);
        c.setColor(kPanelBg);
        c.fillEllipse(cx, cy, r * 0.42f, r * 0.42f);
        c.setColor(colour);
    } else {
        c.setPenSize(1.4f);
        c.strokeEllipse(cx, cy, r, r);
    }
    c.setPenSize(1.4f);
    for (int i = 0; i < 6; ++i) {
        const double a = i * (3.14159265358979323846 / 3.0);
        const float sx = cx + static_cast<float>(std::cos(a)) * r;
        const float sy = cy + static_cast<float>(std::sin(a)) * r;
        const float ex = cx + static_cast<float>(std::cos(a)) * (r + 2.6f);
        const float ey = cy + static_cast<float>(std::sin(a)) * (r + 2.6f);
        c.strokeLine(sx, sy, ex, ey);
    }
}

// The in-circuit indicator: a filled dot when the node is in the path, a hollow ring when it is
// not.
void drawEnableDot(Canvas &c, const Rect &cell, bool on, bool hot)
{
    const float cx = cell.centerX(), cy = cell.centerY();
    if (on) {
        c.setColor(geo::kAccent, hot ? 255 : 220);
        c.fillEllipse(cx, cy, 5.0f, 5.0f);
        c.setColor(geo::kAccentBright, 90);
        c.fillEllipse(cx, cy - 1.0f, 2.2f, 2.2f);
    } else {
        c.setColor(hot ? kIconHot : kOffColor);
        c.setPenSize(1.5f);
        c.strokeEllipse(cx, cy, 5.0f, 5.0f);
    }
}

//------------------------------------------------------------------------
//------------------------------------------------------------------------
// $NAMP_DIAG's per-node readout: a bar along the row's bottom edge showing what fraction of one
// audio callback this pedal ate, and the number itself in the latency cell.
//
// The fraction is of ONE callback, because a serial chain runs on one core (the plan's first
// flagged risk) — so these bars are additive and the rack is out of headroom when they sum to the
// row width, not when the machine is busy. Without a known period there is no fraction to draw and
// only the microseconds are shown, which is honest rather than a bar against an invented budget.
uint32_t diagColour(float fraction)
{
    if (fraction >= kDiagHotFrac)
        return kDiagHotColor;
    if (fraction >= kDiagWarnFrac)
        return kDiagWarnColor;
    return kDiagOkColor;
}

// One wet/dry slider, drawn wherever it is asked for. Both views call it: the node view carries the
// same controls as the list, and two drawings of one control drift apart the first time either is
// touched.
void drawMixSlider(Canvas &c, const Rect &track, float mix, bool enabled, bool labelled,
                   float baseline)
{
    if (labelled) {
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        c.drawString("dry", track.x - kMixLabelGap - c.stringWidth("dry"), baseline);
        c.drawString("wet", track.right() + kMixLabelGap, baseline);
    }

    c.setColor(kPanelBorder);
    c.fillRoundRect(track, 2.0f);
    c.setColor(enabled ? geo::kAccent : kOffColor);
    c.fillRoundRect(Rect(track.x, track.y, track.w * mix, track.h), 2.0f);
    c.setColor(enabled ? geo::kAccentBright : kOffColor);
    c.fillEllipse(track.x + track.w * mix, track.centerY(), 4.0f, 4.0f);
}

void drawDiagCost(Canvas &c, const Rect &row, const Rect &cell, const RackNode &node,
                  double periodMicros)
{
    char text[32];
    const float fraction =
        periodMicros > 0.0 ? static_cast<float>(static_cast<double>(node.diagMicros) / periodMicros)
                           : 0.0f;

    if (periodMicros > 0.0)
        snprintf(text, sizeof(text), "%.1f%%", fraction * 100.0f);
    else
        snprintf(text, sizeof(text), "%lld us", static_cast<long long>(node.diagMicros));

    const uint32_t costColour = diagColour(fraction);

    // Two signals, kept apart. The BAR is always coloured by cost, because its length already means
    // cost and colouring it by anything else makes a short bar look like a long one. The TEXT takes
    // the allocation warning, because a node that is cheap and allocating on the audio thread is a
    // worse problem than a node that is expensive and does not — and it is the one that would
    // otherwise be invisible.
    c.setColor(node.diagMicros > 0 ? (node.diagAllocs > 0 ? kDiagHotColor : costColour)
                                   : kPanelBorder);
    c.drawString(text, cell.right() - c.stringWidth(text) - 6.0f, row.centerY() + 3.5f);

    if (node.diagMicros <= 0 || periodMicros <= 0.0)
        return;

    const float trackW = row.w - 2.0f * kDiagBarInset;
    const float y = row.bottom() - kDiagBarH - 2.0f;
    c.setColor(kPanelBorder);
    c.fillRoundRect(Rect(row.x + kDiagBarInset, y, trackW, kDiagBarH), kDiagBarH * 0.5f);
    c.setColor(costColour);
    const float filled = trackW * (fraction > 1.0f ? 1.0f : fraction);
    c.fillRoundRect(Rect(row.x + kDiagBarInset, y, filled, kDiagBarH), kDiagBarH * 0.5f);
}

//------------------------------------------------------------------------
void drawPill(Canvas &c, const Rect &r, bool active, bool hot, const char *label)
{
    c.setColor(active ? geo::kAccent : kPanelBg, active ? 55 : 255);
    c.fillRoundRect(r, r.h * 0.5f);
    c.setColor(active ? geo::kAccent : (hot ? kIconColor : kPanelBorder));
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, r.h * 0.5f);
    c.setFontSize(kSmallSize);
    c.setColor(active ? geo::kTextColor : (hot ? kIconHot : kIconColor));
    c.drawString(label, r.centerX() - c.stringWidth(label) * 0.5f, r.centerY() + 3.5f);
}

} // namespace

//------------------------------------------------------------------------
void RackView::draw(Canvas &c)
{
    if (!mModel)
        return;
    mModel->refresh();

    c.setColor(kRackBg);
    c.fillRect(Rect(0, 0, kRackW, kRackH));
    c.setColor(kRackTopEdge);
    c.setPenSize(1.0f);
    c.strokeLine(0, 0.5f, kRackW, 0.5f);

    c.setFont(Font::Body);
    drawHeader(c);

    if (mModel->viewMode() == ViewMode::List)
        drawList(c);
    else
        drawNodes(c);

    drawFooter(c);

    if (mModel->picker().open)
        drawPicker(c);

    // Above the picker: it is a modal step within it, and it dismisses back to it.
    if (mBrowser.isOpen())
        mBrowser.draw(c);

    // Above everything, including the browser — a scan blocks the run loop, so whatever was on
    // screen when it started is frozen behind this.
    if (mModel->scan().running)
        drawScanProgress(c);
}

//------------------------------------------------------------------------
void RackView::drawHeader(Canvas &c)
{
    const HitTarget &hover = mModel->hover();

    c.setFont(Font::Title);
    c.setFontSize(kTitleSize);
    c.setColor(geo::kGold);
    c.drawString("RACK", kMargin, kHeaderH * 0.5f + 4.5f);
    c.setFont(Font::Body);

    drawPill(c, Rect(kAddBeforeX, kAddY, kAddW, kAddH), false,
             hover.part == HitTarget::Part::AddBefore, "+ Before");
    drawPill(c, Rect(kAddAfterX, kAddY, kAddW, kAddH), false,
             hover.part == HitTarget::Part::AddAfter, "+ After");

    drawPill(c, Rect(kPresetsX, kPresetsY, kPresetsW, kToggleH),
             mModel->picker().open && mModel->picker().mode == PickerState::Mode::Presets,
             hover.part == HitTarget::Part::Presets, "Presets");

    // "Scan" carries a count when the last scan found something that was not there before, because
    // the whole point of remembering a baseline across restarts is to be able to say so.
    const int fresh = mModel->freshCount();
    char scanLabel[24] = "Scan";
    if (fresh > 0)
        snprintf(scanLabel, sizeof(scanLabel), "Scan  %d", fresh);
    drawPill(c, Rect(kScanX, kScanY, kScanW, kToggleH),
             mModel->picker().open && mModel->picker().mode == PickerState::Mode::Paths,
             hover.part == HitTarget::Part::Scan, scanLabel);

    drawPill(c, Rect(kAudioX, kAudioY, kAudioW, kToggleH),
             mModel->picker().open && mModel->picker().mode == PickerState::Mode::Devices,
             hover.part == HitTarget::Part::Audio, "Audio");

    const bool list = mModel->viewMode() == ViewMode::List;
    drawPill(c, Rect(kToggleX, kToggleY, kToggleSegW, kToggleH), list,
             hover.part == HitTarget::Part::ViewToggleList, "List");
    drawPill(c, Rect(kToggleNodesX, kToggleY, kToggleSegW, kToggleH), !list,
             hover.part == HitTarget::Part::ViewToggleNodes, "Nodes");
}

//------------------------------------------------------------------------
void RackView::drawList(Canvas &c)
{
    std::vector<ListRow> rows;
    int totalRows = 0;
    layoutList(*mModel, rows, totalRows);

    const HitTarget &hover = mModel->hover();

    if (rows.empty()) {
        c.setFontSize(kLabelSize);
        c.setColor(kOffColor);
        c.drawString("The rack is empty. Add a pedal before or after the amp.", kListX,
                     kListY + 24.0f);
        return;
    }

    int ordinal = 0;
    for (const ListRow &row : rows) {
        if (row.anchor) {
            // The amp. Full width, tinted, and deliberately without a single control: it cannot be
            // moved (the chain is built around it), and bypassing or removing it is not something a
            // rack strip should offer.
            c.setColor(kAnchorBg);
            c.fillRoundRect(row.rect, kRowRadius);
            c.setColor(geo::kAccent, 90);
            c.setPenSize(1.0f);
            c.strokeRoundRect(row.rect, kRowRadius);

            c.setFont(Font::Title);
            c.setFontSize(kLabelSize + 1.0f);
            c.setColor(geo::kAccent);
            c.drawString("NAMp", row.rect.x + kOrderW, row.rect.centerY() + 4.0f);
            c.setFont(Font::Body);

            c.setFontSize(kSmallSize);
            c.setColor(kOffColor);
            const char *note = "the amp  ·  mono in, stereo out";
            c.drawString(note, row.rect.x + kOrderW + kAnchorSubtitleDX, row.rect.centerY() + 3.5f);
            continue;
        }

        const RackNode *node = row.node;
        ++ordinal;

        const bool hovered = hover.nodeId == node->id;
        c.setColor(hovered ? kRowBgHover : (ordinal % 2 ? kRowBgOdd : kRowBgEven));
        c.fillRoundRect(row.rect, kRowRadius);
        if (!node->enabled) {
            c.setColor(kPanelBorder);
            c.setPenSize(1.0f);
            c.strokeRoundRect(row.rect, kRowRadius);
        }

        // Order number, and which side of the amp it is on.
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        char ord[16];
        snprintf(ord, sizeof(ord), "%d", ordinal);
        c.drawString(ord, row.rect.x + 9.0f, row.rect.centerY() + 3.5f);

        const Rect enableCell = rowEnableRect(row.rect);
        drawEnableDot(c, enableCell, node->enabled,
                      hover.part == HitTarget::Part::RowEnable && hover.nodeId == node->id);

        // Name, plus the reason it is not sounding when that is the case. Diagnostics take the
        // right-hand end of this cell, so the name is clipped shorter while they are armed and no
        // control moves — see the note in rackgeometry.h.
        const Rect nameRect = rowNameRect(row.rect);
        const float notesRight = nameRect.right() - (mModel->diagArmed() ? kDiagTextW : 0.0f);
        c.setFontSize(kLabelSize);
        c.setColor(node->placeholder ? kDangerColor
                                     : (node->enabled ? geo::kTextColor : kOffColor));
        c.drawString(c.clipToWidth(node->name, notesRight - nameRect.x).c_str(), nameRect.x,
                     row.rect.centerY() + 4.0f);

        if (node->placeholder || !node->enabled) {
            c.setFontSize(kSmallSize);
            c.setColor(node->placeholder ? kDangerColor : kOffColor);
            const char *why = node->placeholder ? "not installed" : "out of circuit";
            c.drawString(why, notesRight - c.stringWidth(why), row.rect.centerY() + 3.5f);
        }

        // The per-node cost, in the width the name gave up. There is no latency readout beside it:
        // a node's latency is compensated automatically and summed in the footer, so nothing was
        // ever done with the number.
        if (mModel->diagArmed())
            drawDiagCost(c, row.rect, Rect(notesRight, row.rect.y, kDiagTextW, row.rect.h), *node,
                         mModel->diagPeriodMicros());

        // Wet/dry. Drawn as a track with a filled portion rather than a knob: it is a value the
        // user needs to read at a glance across a whole rack, and a row of knobs is not readable.
        // The ends are named, because the filled portion alone does not say which way round it is.
        drawMixSlider(c, rowMixTrackRect(row.rect), node->mix, node->enabled, true,
                      row.rect.centerY() + 3.5f);

        // Controls, right to left: remove, editor, down, up.
        const bool onThisRow = hover.nodeId == node->id;
        const Rect removeCell = rowCtlRect(row.rect, 0);
        const Rect editorCell = rowCtlRect(row.rect, 1);
        const Rect downCell = rowCtlRect(row.rect, 2);
        const Rect upCell = rowCtlRect(row.rect, 3);

        drawChevron(c, upCell, true,
                    onThisRow && hover.part == HitTarget::Part::RowUp ? kIconHot : kIconColor);
        drawChevron(c, downCell, false,
                    onThisRow && hover.part == HitTarget::Part::RowDown ? kIconHot : kIconColor);
        if (node->hasEditor)
            drawGear(c, editorCell,
                     node->editorOpen
                         ? geo::kAccent
                         : (onThisRow && hover.part == HitTarget::Part::RowEditor ? kIconHot
                                                                                  : kIconColor),
                     node->editorOpen);
        drawCross(c, removeCell,
                  onThisRow && hover.part == HitTarget::Part::RowRemove ? kDangerColor
                                                                        : kIconColor);
    }

    // Scroll indicator, same idiom as the file browser.
    const int visible = visibleListRows();
    if (totalRows > visible) {
        const float trackX = kListX + kListW + 4.0f;
        const float frac = static_cast<float>(visible) / static_cast<float>(totalRows);
        const float thumbH = std::max(kListH * frac, 18.0f);
        const float maxScroll = static_cast<float>(totalRows - visible);
        const float pos =
            maxScroll > 0
                ? std::min(static_cast<float>(mModel->listScroll()), maxScroll) / maxScroll
                : 0.0f;
        c.setColor(kPanelBorder);
        c.fillRect(Rect(trackX, kListY, 3.0f, kListH));
        c.setColor(geo::kAccent);
        c.fillRect(Rect(trackX, kListY + pos * (kListH - thumbH), 3.0f, thumbH));
    }
}

//------------------------------------------------------------------------
void RackView::drawNodes(Canvas &c)
{
    NodeLayout layout;
    layoutNodes(*mModel, layout);

    const HitTarget &hover = mModel->hover();
    const DragState &drag = mModel->drag();

    // Terminals.
    auto drawTerminal = [&c](const Rect &r, const char *label) {
        c.setColor(kPanelBg);
        c.fillRoundRect(r, r.h * 0.5f);
        c.setColor(kPanelBorder);
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, r.h * 0.5f);
        c.setFontSize(kSmallSize);
        c.setColor(kIconColor);
        c.drawString(label, r.centerX() - c.stringWidth(label) * 0.5f, r.centerY() + 3.5f);
    };
    drawTerminal(layout.inTerm, "IN");
    drawTerminal(layout.outTerm, "OUT");

    // Cables under the cards, so a card always sits on top of its own connections.
    for (const Cable &cable : layout.cables) {
        const bool target = drag.active && drag.dropValid && drag.dropSlot == cable.slot &&
                            drag.section == cable.section;
        const bool hot = hover.part == HitTarget::Part::Cable && hover.slot == cable.slot &&
                         hover.section == cable.section;
        c.setPenSize(target ? 3.0f : 2.0f);
        c.setColor(target ? geo::kAccentBright : (hot ? kCableLive : kCableColor));
        c.strokeConnector(cable.x0, cable.y0, cable.x1, cable.y1, kCableSlack);

        // Ports at each end, so the cable visibly plugs into something.
        c.setColor(target ? geo::kAccentBright : kCableColor);
        c.fillEllipse(cable.x0, cable.y0, kPortR, kPortR);
        c.fillEllipse(cable.x1, cable.y1, kPortR, kPortR);
    }

    auto drawCard = [&](const CardBox &box, bool ghost) {
        const RackNode *node = box.node;
        const bool dragging = !ghost && drag.active && node && node->id == drag.nodeId;
        if (dragging)
            return; // drawn last, under the pointer

        Rect r = box.rect;
        if (box.anchor) {
            c.setColor(kAnchorBg);
            c.fillRoundRect(r, kCardRadius);
            c.setColor(geo::kAccent, 130);
            c.setPenSize(1.5f);
            c.strokeRoundRect(r, kCardRadius);
            c.setFont(Font::Title);
            c.setFontSize(kLabelSize + 1.0f);
            c.setColor(geo::kAccent);
            c.drawString("NAMp", r.centerX() - c.stringWidth("NAMp") * 0.5f, r.centerY() + 1.0f);
            c.setFont(Font::Body);
            c.setFontSize(kSmallSize);
            c.setColor(kOffColor);
            const char *sub = "amp";
            c.drawString(sub, r.centerX() - c.stringWidth(sub) * 0.5f, r.centerY() + 15.0f);
            return;
        }

        const bool hot = node && hover.nodeId == node->id;
        c.setColor(ghost ? kPanelBg : (hot ? kRowBgHover : kPanelBg), ghost ? 160 : 255);
        c.fillRoundRect(r, kCardRadius);
        c.setColor(node && !node->enabled ? kPanelBorder : (hot ? kIconColor : kPanelBorder));
        c.setPenSize(1.0f);
        c.strokeRoundRect(r, kCardRadius);

        if (!node)
            return;

        // The name is centred in what the remove cross leaves rather than in the whole card, so a
        // long name is clipped shorter instead of running under the cross. Centring it in the card
        // and reserving the cross's width on BOTH sides would cost twice as much of it.
        const Rect removeCell = cardRemoveRect(r);
        const float nameLeft = r.x + 6.0f;
        const float nameW = removeCell.x - 4.0f - nameLeft;
        c.setFontSize(kLabelSize);
        c.setColor(node->placeholder ? kDangerColor
                                     : (node->enabled ? geo::kTextColor : kOffColor));
        const std::string name = c.clipToWidth(node->name, nameW);
        c.drawString(name.c_str(), nameLeft + (nameW - c.stringWidth(name.c_str())) * 0.5f,
                     r.y + 22.0f);

        // Same glyph as the list's remove cross — drawCross sizes from the cell's centre, not its
        // extent, so the two are the same mark at two sizes of cell.
        drawCross(c, removeCell,
                  hot && hover.part == HitTarget::Part::RowRemove ? kDangerColor : kIconColor);

        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        const char *side = node->section == host::ChainSection::Pre ? "before" : "after";
        c.drawString(side, r.centerX() - c.stringWidth(side) * 0.5f, r.y + 36.0f);

        if (cardHasControls(r)) {
            // The same wet/dry slider the list has. Without it the node view could show a mix and
            // not set one, which meant leaving the view to turn a knob that is drawn in it.
            drawMixSlider(c, cardMixTrackRect(r), node->mix, node->enabled, cardMixLabelled(r),
                          r.y + kCardMixDY + 3.5f);

            drawEnableDot(c, Rect(r.x + 6.0f, r.bottom() - 20.0f, 16.0f, 16.0f), node->enabled,
                          hot && hover.part == HitTarget::Part::RowEnable);
            if (node->hasEditor)
                drawGear(c, Rect(r.right() - 22.0f, r.bottom() - 20.0f, 16.0f, 16.0f),
                         node->editorOpen ? geo::kAccent : kIconColor, node->editorOpen);
        }
    };

    for (const CardBox &box : layout.path)
        drawCard(box, false);

    // The shelf, and its label, only when something is on it.
    if (!layout.shelf.empty()) {
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        c.drawString("OUT OF CIRCUIT", kMargin, kShelfY - 8.0f);
        for (const CardBox &box : layout.shelf)
            drawCard(box, true);
    }

    // The dragged card last, so it floats over everything.
    if (drag.active) {
        if (const RackNode *node = mModel->nodeById(drag.nodeId)) {
            CardBox floating;
            floating.node = node;
            floating.rect = Rect(drag.x - drag.grabDx, drag.y - drag.grabDy, layout.cardW, kCardH);
            c.setColor(0x000000, 90);
            c.fillRoundRect(Rect(floating.rect.x + 2.0f, floating.rect.y + 3.0f, floating.rect.w,
                                 floating.rect.h),
                            kCardRadius);
            drawCard(floating, false);
        }
    }
}

//------------------------------------------------------------------------
void RackView::drawFooter(Canvas &c)
{
    c.setFontSize(kSmallSize);
    c.setColor(kOffColor);

    char text[128];
    const uint32_t latency = mModel->totalLatency();
    const int count = static_cast<int>(mModel->nodes().size());
    snprintf(text, sizeof(text), "%d pedal%s  ·  rack latency %u samples", count,
             count == 1 ? "" : "s", latency);
    c.drawString(text, kMargin, kRackH - 9.0f);
    const float leftEnd = kMargin + c.stringWidth(text);

    // When diagnostics are armed the right-hand end of the footer is the summary rather than the
    // interaction hint: the whole reason to arm them is that something is costing more than it
    // should, and a hint about dragging cards is not what is wanted at that moment.
    if (mModel->diagArmed()) {
        uint64_t allocs = 0;
        for (const RackNode &node : mModel->nodes())
            allocs += node.diagAllocs;

        const int64_t worst = mModel->diagWorstMicros();
        const double period = mModel->diagPeriodMicros();
        char diag[192];
        if (period > 0.0)
            snprintf(diag, sizeof(diag),
                     "NAMP_DIAG  ·  worst pedal %lld us of %.0f us per callback (%.1f%%)  ·  "
                     "RT allocations %llu",
                     static_cast<long long>(worst), period,
                     100.0 * static_cast<double>(worst) / period,
                     static_cast<unsigned long long>(allocs));
        else
            snprintf(diag, sizeof(diag),
                     "NAMP_DIAG  ·  worst pedal %lld us  ·  RT allocations %llu",
                     static_cast<long long>(worst), static_cast<unsigned long long>(allocs));

        c.setColor(allocs > 0 ? kDangerColor : kOffColor);
        const float w = c.stringWidth(diag);
        c.drawString(diag, kRackW - kMargin - w, kRackH - 9.0f);
        return;
    }

    // The interaction hint belongs to the mode that needs it: in List mode a row's controls are
    // visible where they act, while a card view has to say that a cable is a drop target.
    float rightStart = kRackW - kMargin;
    if (mModel->viewMode() == ViewMode::Nodes) {
        const char *hint = kFooterNodesHint;
        const float w = c.stringWidth(hint);
        rightStart = kRackW - kMargin - w;
        c.drawString(hint, rightStart, kRackH - 9.0f);
    }

    // THE DEVICE, IN WHATEVER IS LEFT BETWEEN THE TWO, and clipped to it rather than trusted to
    // fit. An ASIO driver's name is whatever the vendor registered and a WASAPI endpoint's is
    // whatever the user renamed it to, so its length is not this project's to know — and the two
    // things either side of it are the pedal count and an interaction hint, both of which matter
    // more than the last few characters of a device name.
    const std::string &device = mModel->audioStatus();
    if (device.empty())
        return;

    std::string status = device;
    const uint32_t dropouts = mModel->audioDropouts();
    if (dropouts > 0) {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "  ·  %u dropout%s", dropouts, dropouts == 1 ? "" : "s");
        status += suffix;
    }

    const float gapX = leftEnd + kMargin;
    const float gapW = rightStart - kMargin - gapX;
    if (gapW <= 0.0f)
        return;
    // Red once there have been any: a dropout is the number the live gate is written against, so it
    // is not something to mention in passing.
    c.setColor(dropouts > 0 ? kDangerColor : kOffColor);
    c.drawString(c.clipToWidth(status, gapW).c_str(), gapX, kRackH - 9.0f);
}

//------------------------------------------------------------------------
void RackView::drawPicker(Canvas &c)
{
    const PickerState &picker = mModel->picker();

    c.setColor(0x000000, 175);
    c.fillRect(Rect(0, 0, kRackW, kRackH));

    const Rect panel(kPickerX, kPickerY, kPickerW, kPickerH);
    c.setColor(kPanelBg);
    c.fillRoundRect(panel, 8.0f);
    c.setColor(kPanelBorder);
    c.setPenSize(1.0f);
    c.strokeRoundRect(panel, 8.0f);

    const bool presets = picker.mode == PickerState::Mode::Presets;
    const bool paths = picker.mode == PickerState::Mode::Paths;
    const bool devices = picker.mode == PickerState::Mode::Devices;

    c.setFont(Font::Title);
    c.setFontSize(kLabelSize + 1.0f);
    c.setColor(geo::kTextColor);
    if (presets)
        c.drawString("Racks", kPickerX + 14.0f, kPickerY + 22.0f);
    else if (paths)
        c.drawString("Plug-ins", kPickerX + 14.0f, kPickerY + 22.0f);
    else if (devices)
        c.drawString("Audio", kPickerX + 14.0f, kPickerY + 22.0f);
    else
        c.drawString(picker.section == host::ChainSection::Pre ? "Add a pedal before the amp"
                                                               : "Add a pedal after the amp",
                     kPickerX + 14.0f, kPickerY + 22.0f);
    c.setFont(Font::Body);

    if (paths) {
        // What is installed, counted from the catalogue itself so this line cannot disagree with
        // the picker's list. The "new" tail is why the baseline is persisted at all.
        char counts[128];
        const int fresh = mModel->freshCount();
        char freshTail[32] = "";
        if (fresh > 0)
            snprintf(freshTail, sizeof(freshTail), "  ·  %d new", fresh);
        snprintf(counts, sizeof(counts), "%d VST3  ·  %d LV2%s",
                 mModel->catalogCount(host::PluginFormat::Vst3),
                 mModel->catalogCount(host::PluginFormat::Lv2), freshTail);
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        const float w = c.stringWidth(counts);
        c.drawString(counts, kPickerX + kPickerW - 40.0f - w, kPickerY + 22.0f);
    }

    const Rect close = pickerCloseBox();
    drawCross(c, close,
              mModel->hover().part == HitTarget::Part::PickerClose ? kIconHot : kIconColor);

    if (devices) {
        // The device that is open, in the header, so the list below reads as a choice against
        // something rather than as a list of names with no present tense.
        const std::string &open = mModel->audioStatus();
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        const std::string line = open.empty() ? std::string("no device open") : open;
        const std::string shown = c.clipToWidth(line, kPickerW * 0.6f);
        c.drawString(shown.c_str(), kPickerX + kPickerW - 40.0f - c.stringWidth(shown.c_str()),
                     kPickerY + 22.0f);
    }

    const std::vector<AudioDeviceRow> *deviceRows = mModel->audioDevices();
    if (devices && (!deviceRows || deviceRows->empty())) {
        c.setFontSize(kLabelSize);
        c.setColor(kOffColor);
        c.drawString("No audio devices were found. The editor still runs; there is just nothing "
                     "to play through.",
                     kPickerX + 14.0f, kPickerY + kPickerHeaderH + 22.0f);
        return;
    }

    const std::vector<host::PluginDesc> *catalog = mModel->catalog();
    if (!presets && !paths && !devices && (!catalog || catalog->empty())) {
        c.setFontSize(kLabelSize);
        c.setColor(kOffColor);
        c.drawString("No plug-ins have been scanned yet. Use Scan to look again, or to add a "
                     "folder to search.",
                     kPickerX + 14.0f, kPickerY + kPickerHeaderH + 22.0f);
        return;
    }

    const int total = pickerRowCount();
    const int rows = pickerVisibleRows();
    const int maxScroll = std::max(0, total - rows);
    const int scroll = std::min(picker.scroll, maxScroll);
    const std::vector<std::string> *saved = mModel->presets();
    const std::vector<SearchPathRow> *searchPaths = mModel->searchPaths();

    c.pushClip(Rect(kPickerX, kPickerY + kPickerHeaderH, kPickerW, kPickerH - kPickerHeaderH));
    for (int i = 0; i < rows; ++i) {
        const int index = scroll + i;
        if (index >= total)
            break;
        const Rect row = pickerRowRect(i);
        const bool hot =
            mModel->hover().part == HitTarget::Part::PickerRow && mModel->hover().slot == index;

        c.setColor(hot ? kRowBgHover : (index % 2 ? kRowBgOdd : kRowBgEven));
        c.fillRect(row);

        if (devices) {
            if (!deviceRows || static_cast<size_t>(index) >= deviceRows->size())
                continue;
            const AudioDeviceRow &dev = (*deviceRows)[static_cast<size_t>(index)];

            // The group on the left, in the same column the search-path list puts its format tag:
            // the two lists are the same shape and reading them should be the same motion.
            c.setFontSize(kSmallSize);
            c.setColor(kOffColor);
            c.drawString(c.clipToWidth(dev.group, 44.0f).c_str(), row.x + 12.0f,
                         row.centerY() + 3.5f);

            // The one that is open is in the accent, like the rack that is loaded in the preset
            // list. A row that cannot be chosen is dim whether or not the pointer is over it.
            c.setFontSize(kLabelSize - 1.0f);
            if (dev.current)
                c.setColor(geo::kAccent);
            else if (!dev.selectable)
                c.setColor(kOffColor);
            else
                c.setColor(hot ? geo::kTextColor : 0xD8D4D0);
            c.drawString(c.clipToWidth(dev.name, kPickerW - 230.0f).c_str(), row.x + 62.0f,
                         row.centerY() + 3.5f);

            if (!dev.detail.empty()) {
                c.setFontSize(kSmallSize);
                c.setColor(kOffColor);
                const std::string detail = c.clipToWidth(dev.detail, 150.0f);
                c.drawString(detail.c_str(), row.right() - c.stringWidth(detail.c_str()) - 12.0f,
                             row.centerY() + 3.5f);
            }
            continue;
        }

        if (paths) {
            if (index == kPathScanRow) {
                c.setFontSize(kLabelSize - 1.0f);
                c.setColor(hot ? geo::kAccentBright : geo::kAccent);
                c.drawString("Scan now", row.x + 12.0f, row.centerY() + 3.5f);
                c.setFontSize(kSmallSize);
                c.setColor(kOffColor);
                const char *why = "looks again in every folder below; unchanged plug-ins are not "
                                  "opened";
                c.drawString(c.clipToWidth(why, kPickerW * 0.6f).c_str(), row.x + 110.0f,
                             row.centerY() + 3.5f);
                continue;
            }
            if (index == kPathAddRow) {
                c.setFontSize(kLabelSize - 1.0f);
                c.setColor(hot ? geo::kAccentBright : geo::kAccent);
                c.drawString("Add a folder...", row.x + 12.0f, row.centerY() + 3.5f);
                continue;
            }

            const size_t which = static_cast<size_t>(index - kPathFirstRow);
            if (!searchPaths || which >= searchPaths->size())
                continue;
            const SearchPathRow &pathRow = (*searchPaths)[which];

            c.setFontSize(kSmallSize);
            c.setColor(kOffColor);
            c.drawString(pathRow.tag.empty() ? "ALL" : pathRow.tag.c_str(), row.x + 12.0f,
                         row.centerY() + 3.5f);

            // An automatic row is dimmed and carries no cross: it is where this host looks anyway,
            // and offering to remove something the user never added would be offering a lie.
            c.setFontSize(kLabelSize - 1.0f);
            c.setColor(pathRow.automatic ? kOffColor : (hot ? geo::kTextColor : 0xD8D4D0));
            c.drawString(c.clipToWidth(pathRow.path, kPickerW - 190.0f).c_str(), row.x + 52.0f,
                         row.centerY() + 3.5f);

            if (pathRow.automatic) {
                c.setFontSize(kSmallSize);
                c.setColor(kOffColor);
                const char *tag = "found on its own";
                c.drawString(tag, row.right() - c.stringWidth(tag) - 12.0f, row.centerY() + 3.5f);
            } else {
                const bool overCross = mModel->hover().part == HitTarget::Part::PickerRowRemove &&
                                       mModel->hover().slot == index;
                drawCross(c, pathRowRemoveBox(row), overCross ? kDangerColor : kIconColor);
            }
            continue;
        }

        if (presets) {
            // Row zero saves; the rest load. Save is a row rather than a separate button because it
            // belongs to the same list — it is what the current rack would become — and because the
            // overlay already scrolls, so a floating button would have to be laid out around it.
            if (index == kPresetSaveRow) {
                c.setFontSize(kLabelSize - 1.0f);
                c.setColor(hot ? geo::kAccentBright : geo::kAccent);
                const std::string label =
                    "Save changes to \"" +
                    (mModel->presetName().empty() ? std::string("default") : mModel->presetName()) +
                    "\"";
                c.drawString(c.clipToWidth(label, kPickerW - 40.0f).c_str(), row.x + 12.0f,
                             row.centerY() + 3.5f);
                continue;
            }

            if (index == kPresetNewRow) {
                const TextEntry &entry = mModel->presetEntry();
                if (entry.active) {
                    // A field, drawn like the amp's rename field: the text, and a caret that is a
                    // rule at the width of what precedes it. No selection and no scrolling — a
                    // preset name that outruns the row is longer than anything worth naming, and
                    // clipToWidth keeps it inside the box either way.
                    c.setFontSize(kSmallSize);
                    c.setColor(kOffColor);
                    c.drawString("NEW", row.x + 12.0f, row.centerY() + 3.5f);
                    c.setFontSize(kLabelSize - 1.0f);

                    const float textX = row.x + 52.0f;
                    const float textW = kPickerW - 80.0f;
                    c.setColor(geo::kTextColor);
                    const std::string shown = c.clipToWidth(entry.text, textW);
                    c.drawString(shown.c_str(), textX, row.centerY() + 3.5f);

                    const std::string upToCaret = c.clipToWidth(
                        entry.text.substr(0, std::min(entry.caret, entry.text.size())), textW);
                    const float caretX = textX + c.stringWidth(upToCaret.c_str());
                    c.setColor(geo::kAccent);
                    c.fillRect(Rect(caretX, row.centerY() - 7.0f, 1.0f, 14.0f));

                    c.setFontSize(kSmallSize);
                    c.setColor(kOffColor);
                    const char *hint = "Return saves - Esc cancels";
                    c.drawString(hint, row.right() - c.stringWidth(hint) - 12.0f,
                                 row.centerY() + 3.5f);
                    continue;
                }
                c.setFontSize(kLabelSize - 1.0f);
                c.setColor(hot ? geo::kAccentBright : geo::kAccent);
                c.drawString("+  New preset...", row.x + 12.0f, row.centerY() + 3.5f);

                c.setFontSize(kSmallSize);
                c.setColor(kOffColor);
                const char *hint = "click, then type a name";
                c.drawString(hint, row.right() - c.stringWidth(hint) - 12.0f, row.centerY() + 3.5f);
                continue;
            }

            const size_t which = static_cast<size_t>(index - kPresetFirstRow);
            if (!saved || index < kPresetFirstRow || which >= saved->size())
                continue;
            const std::string &name = (*saved)[which];
            const bool current = (name == mModel->presetName());

            const bool armed = mModel->presetDeleteArmed() == index;

            c.setFontSize(kSmallSize);
            c.setColor(current ? geo::kAccent : kPanelBorder);
            c.drawString(current ? "NOW" : "-", row.x + 12.0f, row.centerY() + 3.5f);

            c.setFontSize(kLabelSize - 1.0f);
            c.setColor(hot ? geo::kTextColor : 0xD8D4D0);
            c.drawString(c.clipToWidth(name, kPickerW - 140.0f).c_str(), row.x + 52.0f,
                         row.centerY() + 3.5f);

            const bool overDelete = mModel->hover().part == HitTarget::Part::PickerRowRemove &&
                                    mModel->hover().slot == index;
            const Rect deleteBox = presetRowRemoveBox(row);

            // An armed row says so in words. A control that had only changed colour would not be a
            // confirmation, and this is the one click in the rack that destroys something on disk.
            c.setFontSize(kSmallSize);
            c.setColor((armed || overDelete) ? kDangerColor : kIconColor);
            c.drawString(armed ? "Delete?" : "Delete", deleteBox.x, row.centerY() + 3.5f);
            if (armed) {
                const char *ask = "click again";
                c.drawString(ask, deleteBox.x - c.stringWidth(ask) - 10.0f, row.centerY() + 3.5f);
            }
            continue;
        }

        const host::PluginDesc &desc = (*catalog)[static_cast<size_t>(index)];
        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        c.drawString(host::formatTag(desc.ref.format), row.x + 10.0f, row.centerY() + 3.5f);

        // Installed since the last scan. Marked in the list rather than only counted in the header,
        // because "3 new" in a list of two hundred is a number, not an answer.
        if (desc.freshlyFound) {
            c.setColor(geo::kAccent);
            c.drawString("NEW", row.right() - 40.0f, row.centerY() - 3.0f);
        }

        c.setFontSize(kLabelSize - 1.0f);
        c.setColor(hot ? geo::kTextColor : 0xD8D4D0);
        c.drawString(c.clipToWidth(desc.name, kPickerW * 0.52f).c_str(), row.x + 52.0f,
                     row.centerY() + 3.5f);

        c.setFontSize(kSmallSize);
        c.setColor(kOffColor);
        const std::string category = c.clipToWidth(desc.category, kPickerW * 0.3f);
        c.drawString(category.c_str(), row.right() - c.stringWidth(category.c_str()) - 12.0f,
                     row.centerY() + 3.5f);
    }
    c.popClip();

    if (presets && total == kPresetFirstRow) {
        c.setFontSize(kLabelSize);
        c.setColor(kOffColor);
        c.drawString("No racks saved yet.", kPickerX + 14.0f,
                     kPickerY + kPickerHeaderH + kPresetFirstRow * kPickerRowH + 22.0f);
    }

    if (total > rows) {
        const float trackX = kPickerX + kPickerW - 6.0f;
        const float listTop = kPickerY + kPickerHeaderH;
        const float listH = static_cast<float>(rows) * kPickerRowH;
        const float frac = static_cast<float>(rows) / static_cast<float>(total);
        const float thumbH = std::max(listH * frac, 16.0f);
        const float pos = maxScroll > 0 ? static_cast<float>(scroll) / maxScroll : 0.0f;
        c.setColor(kPanelBorder);
        c.fillRect(Rect(trackX, listTop, 3.0f, listH));
        c.setColor(geo::kAccent);
        c.fillRect(Rect(trackX, listTop + pos * (listH - thumbH), 3.0f, thumbH));
    }
}

//------------------------------------------------------------------------
// The scan banner. A scan is synchronous — the run loop is inside it, one bundle at a time — so
// this is drawn from the progress callback and blitted immediately, and it is the only thing that
// tells the user the application is working rather than wedged. It deliberately covers the whole
// strip: nothing behind it can be interacted with while it is up, and pretending otherwise by
// leaving the controls visible would invite clicks that go nowhere.
void RackView::drawScanProgress(Canvas &c)
{
    const ScanState &scan = mModel->scan();

    c.setColor(0x000000, 210);
    c.fillRect(Rect(0, 0, kRackW, kRackH));

    const float w = kPickerW * 0.7f;
    const float h = 86.0f;
    const Rect box((kRackW - w) * 0.5f, (kRackH - h) * 0.5f, w, h);
    c.setColor(kPanelBg);
    c.fillRoundRect(box, 8.0f);
    c.setColor(kPanelBorder);
    c.setPenSize(1.0f);
    c.strokeRoundRect(box, 8.0f);

    c.setFont(Font::Title);
    c.setFontSize(kLabelSize + 1.0f);
    c.setColor(geo::kTextColor);
    c.drawString("Scanning for plug-ins", box.x + 16.0f, box.y + 26.0f);
    c.setFont(Font::Body);

    // A determinate bar, because the total is known before the first bundle is opened. The parent
    // project pulses an indeterminate one; there is no reason to hide a number we have.
    const Rect track(box.x + 16.0f, box.y + 42.0f, box.w - 32.0f, kScanBarH);
    c.setColor(kPanelBorder);
    c.fillRoundRect(track, kScanBarH * 0.5f);
    const float frac =
        scan.total > 0
            ? std::min(1.0f, static_cast<float>(scan.index) / static_cast<float>(scan.total))
            : 0.0f;
    c.setColor(geo::kAccent);
    c.fillRoundRect(Rect(track.x, track.y, track.w * frac, track.h), kScanBarH * 0.5f);

    c.setFontSize(kSmallSize);
    c.setColor(kOffColor);
    char line[512];
    // The bundle's own name, not its whole path: the path is long, the name is what identifies the
    // plug-in that is taking its time.
    std::string current = scan.current;
    const size_t slash = current.find_last_of('/');
    if (slash != std::string::npos)
        current = current.substr(slash + 1);
    snprintf(line, sizeof(line), "%d of %d   %s", scan.index, scan.total, current.c_str());
    c.drawString(c.clipToWidth(line, box.w - 32.0f).c_str(), box.x + 16.0f, box.bottom() - 18.0f);
}

//------------------------------------------------------------------------
void RackView::openFolderBrowser(const std::string &startPath)
{
    mBrowser.setBounds(browserBounds());
    // Counting bundles rather than files, not requiring any: a .vst3 and a .lv2 are DIRECTORIES,
    // and a folder the user is about to add may be empty today and full next week.
    mBrowser.setDirectoryChoice("plug-in", true, false);
    mBrowser.open(startPath, "vst3,lv2", "Choose a folder to search for plug-ins",
                  FileBrowser::Mode::Directory);
}

//------------------------------------------------------------------------
// Rows in the overlay's list, whichever list it is showing. The preset list carries one extra row
// at the top — "save" — so that a rack can be written without leaving the rack.
int RackView::pickerRowCount() const
{
    if (!mModel)
        return 0;
    if (mModel->picker().mode == PickerState::Mode::Presets) {
        const std::vector<std::string> *presets = mModel->presets();
        return kPresetFirstRow + (presets ? static_cast<int>(presets->size()) : 0);
    }
    if (mModel->picker().mode == PickerState::Mode::Paths) {
        const std::vector<SearchPathRow> *paths = mModel->searchPaths();
        return kPathFirstRow + (paths ? static_cast<int>(paths->size()) : 0);
    }
    if (mModel->picker().mode == PickerState::Mode::Devices) {
        const std::vector<AudioDeviceRow> *devices = mModel->audioDevices();
        return devices ? static_cast<int>(devices->size()) : 0;
    }
    const std::vector<host::PluginDesc> *catalog = mModel->catalog();
    return catalog ? static_cast<int>(catalog->size()) : 0;
}

//------------------------------------------------------------------------
HitTarget RackView::hitTest(float x, float y) const
{
    HitTarget hit;
    if (!mModel)
        return hit;

    //--- the picker swallows everything while it is up --------------------
    if (mModel->picker().open) {
        if (pickerCloseBox().inset(-6.0f).contains(x, y)) {
            hit.part = HitTarget::Part::PickerClose;
            return hit;
        }
        const int rows = pickerVisibleRows();
        const int total = pickerRowCount();
        const int maxScroll = std::max(0, total - rows);
        const int scroll = std::min(mModel->picker().scroll, maxScroll);
        const bool paths = mModel->picker().mode == PickerState::Mode::Paths;
        const bool presets = mModel->picker().mode == PickerState::Mode::Presets;
        const std::vector<SearchPathRow> *searchPaths = mModel->searchPaths();
        for (int i = 0; i < rows; ++i) {
            const int index = scroll + i;
            if (index >= total)
                break;
            const Rect row = pickerRowRect(i);
            if (!row.contains(x, y))
                continue;

            // The cross first: it sits inside the row it belongs to, so testing the row first would
            // swallow it. Only a row the user added has one.
            if (paths && index >= kPathFirstRow && searchPaths) {
                const size_t which = static_cast<size_t>(index - kPathFirstRow);
                if (which < searchPaths->size() && !(*searchPaths)[which].automatic &&
                    pathRowRemoveBox(row).inset(-6.0f).contains(x, y)) {
                    hit.part = HitTarget::Part::PickerRowRemove;
                    hit.slot = index;
                    return hit;
                }
            }

            // Same for a saved preset, and every one of them has a cross: unlike a search path,
            // there is no such thing as a preset the program put there itself.
            if (presets && index >= kPresetFirstRow) {
                const std::vector<std::string> *saved = mModel->presets();
                const size_t which = static_cast<size_t>(index - kPresetFirstRow);
                if (saved && which < saved->size() &&
                    presetRowRemoveBox(row).inset(-6.0f).contains(x, y)) {
                    hit.part = HitTarget::Part::PickerRowRemove;
                    hit.slot = index;
                    return hit;
                }
            }

            hit.part = HitTarget::Part::PickerRow;
            hit.slot = index;
            return hit;
        }
        return hit; // inside the overlay but on nothing
    }

    //--- header ----------------------------------------------------------
    if (Rect(kToggleX, kToggleY, kToggleSegW, kToggleH).contains(x, y)) {
        hit.part = HitTarget::Part::ViewToggleList;
        return hit;
    }
    if (Rect(kToggleNodesX, kToggleY, kToggleSegW, kToggleH).contains(x, y)) {
        hit.part = HitTarget::Part::ViewToggleNodes;
        return hit;
    }
    if (Rect(kAddBeforeX, kAddY, kAddW, kAddH).contains(x, y)) {
        hit.part = HitTarget::Part::AddBefore;
        return hit;
    }
    if (Rect(kAddAfterX, kAddY, kAddW, kAddH).contains(x, y)) {
        hit.part = HitTarget::Part::AddAfter;
        return hit;
    }
    if (Rect(kPresetsX, kPresetsY, kPresetsW, kToggleH).contains(x, y)) {
        hit.part = HitTarget::Part::Presets;
        return hit;
    }
    if (Rect(kScanX, kScanY, kScanW, kToggleH).contains(x, y)) {
        hit.part = HitTarget::Part::Scan;
        return hit;
    }
    if (Rect(kAudioX, kAudioY, kAudioW, kToggleH).contains(x, y)) {
        hit.part = HitTarget::Part::Audio;
        return hit;
    }

    //--- list view -------------------------------------------------------
    if (mModel->viewMode() == ViewMode::List) {
        std::vector<ListRow> rows;
        int totalRows = 0;
        layoutList(*mModel, rows, totalRows);

        for (const ListRow &row : rows) {
            if (row.anchor || !row.node || !row.rect.contains(x, y))
                continue;
            hit.nodeId = row.node->id;
            hit.section = row.node->section;

            if (rowEnableRect(row.rect).contains(x, y))
                hit.part = HitTarget::Part::RowEnable;
            else if (rowCtlRect(row.rect, 0).contains(x, y))
                hit.part = HitTarget::Part::RowRemove;
            else if (rowCtlRect(row.rect, 1).contains(x, y))
                hit.part =
                    row.node->hasEditor ? HitTarget::Part::RowEditor : HitTarget::Part::NoPart;
            else if (rowCtlRect(row.rect, 2).contains(x, y))
                hit.part = HitTarget::Part::RowDown;
            else if (rowCtlRect(row.rect, 3).contains(x, y))
                hit.part = HitTarget::Part::RowUp;
            else if (rowMixRect(row.rect).contains(x, y)) {
                // The labels are part of the target on purpose: the clamp turns a click on "dry"
                // into 0 and one on "wet" into 1, which is what someone reaching for either end
                // wants and what a track alone makes them aim for.
                const Rect track = rowMixTrackRect(row.rect);
                hit.part = HitTarget::Part::RowMix;
                hit.local = std::min(std::max((x - track.x) / track.w, 0.0f), 1.0f);
            }
            return hit;
        }
        return hit;
    }

    //--- node view -------------------------------------------------------
    NodeLayout layout;
    layoutNodes(*mModel, layout);

    for (const CardBox &box : layout.path) {
        if (box.anchor || !box.node || !box.rect.contains(x, y))
            continue;
        hit.nodeId = box.node->id;
        hit.section = box.node->section;
        hit.part = HitTarget::Part::Card;
        if (cardRemoveRect(box.rect).inset(-kCardRemoveHitPad).contains(x, y))
            hit.part = HitTarget::Part::RowRemove;
        else if (Rect(box.rect.x + 6.0f, box.rect.bottom() - 20.0f, 16.0f, 16.0f).contains(x, y))
            hit.part = HitTarget::Part::RowEnable;
        else if (box.node->hasEditor &&
                 Rect(box.rect.right() - 22.0f, box.rect.bottom() - 20.0f, 16.0f, 16.0f)
                     .contains(x, y))
            hit.part = HitTarget::Part::RowEditor;
        else if (cardHasControls(box.rect) && cardMixBandRect(box.rect).contains(x, y)) {
            // The slider wins over the card drag, which is why it has a band of its own rather than
            // just the 4 px track: a press that means "set the mix" and lands one pixel high would
            // otherwise pick the card up instead.
            const Rect track = cardMixTrackRect(box.rect);
            hit.part = HitTarget::Part::RowMix;
            hit.local = std::min(std::max((x - track.x) / track.w, 0.0f), 1.0f);
        }
        return hit;
    }
    for (const CardBox &box : layout.shelf) {
        if (!box.node || !box.rect.contains(x, y))
            continue;
        hit.nodeId = box.node->id;
        hit.section = box.node->section;
        // The cross before the card: a shelved pedal is the one most likely to be on its way out,
        // and pressing the card body picks it up to drag instead.
        hit.part = cardRemoveRect(box.rect).inset(-kCardRemoveHitPad).contains(x, y)
                       ? HitTarget::Part::RowRemove
                       : HitTarget::Part::Card;
        return hit;
    }

    // Cables are hit by proximity to their midpoint rather than by a rect: the curve is what the
    // user aims at, and a bounding box around an S-curve covers a lot of empty canvas.
    for (const Cable &cable : layout.cables) {
        const float dx = x - cable.midX();
        const float dy = y - cable.midY();
        if (dx * dx + dy * dy <= kCableHitR * kCableHitR) {
            hit.part = HitTarget::Part::Cable;
            hit.section = cable.section;
            hit.slot = cable.slot;
            return hit;
        }
    }

    return hit;
}

//------------------------------------------------------------------------
bool RackView::mixTrackFor(uint64_t nodeId, Rect &track) const
{
    if (mModel->viewMode() == ViewMode::List) {
        std::vector<ListRow> rows;
        int totalRows = 0;
        layoutList(*mModel, rows, totalRows);
        for (const ListRow &row : rows) {
            if (!row.anchor && row.node && row.node->id == nodeId) {
                track = rowMixTrackRect(row.rect);
                return true;
            }
        }
        return false;
    }

    NodeLayout layout;
    layoutNodes(*mModel, layout);
    for (const CardBox &box : layout.path) {
        if (!box.anchor && box.node && box.node->id == nodeId && cardHasControls(box.rect)) {
            track = cardMixTrackRect(box.rect);
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
RackAction RackView::mouseMove(float x, float y)
{
    if (!mModel)
        return RackAction();
    // Every entry point refreshes, not just draw(). Hit-testing against a model that has not been
    // read since the chain changed answers about the rack as it WAS — and before the first paint it
    // answers about an empty rack, so the very first click of a session would land on nothing.
    mModel->refresh();

    if (mBrowser.isOpen())
        return RackAction(); // the browser has no hover state to keep

    // A press that landed on a mix track keeps the pointer until the button comes up, and tracks it
    // whatever the y is. Re-hit-testing every motion instead would drop the slider the moment the
    // pointer left the row, which is not how a slider behaves anywhere — and with a 4 px track it
    // would be most of the time. The rect is looked up rather than remembered because the chain can
    // be republished under a drag: the node keeps its id, so the row it is on is what moves.
    if (mPressed.part == HitTarget::Part::RowMix) {
        Rect track;
        const RackNode *node = mModel->nodeById(mPressed.nodeId);
        if (!node || !mixTrackFor(mPressed.nodeId, track))
            return RackAction();
        const float value = std::min(std::max((x - track.x) / track.w, 0.0f), 1.0f);
        if (std::fabs(value - node->mix) < 1e-4f)
            return RackAction();
        RackAction action;
        action.kind = RackAction::Kind::SetMix;
        action.section = node->section;
        action.index = node->index;
        action.value = value;
        return action;
    }

    DragState &drag = mModel->drag();
    if (drag.active) {
        drag.x = x;
        drag.y = y;

        // Where would it land? A card can only be spliced into its OWN section: the section fixes
        // the channel count at instantiation, so moving across the amp is a different plug-in
        // instance, not a move.
        NodeLayout layout;
        layoutNodes(*mModel, layout);
        drag.dropValid = false;
        drag.dropSlot = -1;

        float best = kCableHitR * 3.0f;
        for (const Cable &cable : layout.cables) {
            if (cable.section != drag.section)
                continue;
            const float dx = x - cable.midX();
            const float dy = y - cable.midY();
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < best) {
                best = dist;
                drag.dropSlot = cable.slot;
                drag.dropValid = true;
            }
        }
        return RackAction::redraw();
    }

    const HitTarget hit = hitTest(x, y);
    if (hit == mModel->hover())
        return RackAction();
    mModel->setHover(hit);
    return RackAction::redraw();
}

//------------------------------------------------------------------------
RackAction RackView::mouseDown(float x, float y, int button)
{
    RackAction action;
    if (!mModel)
        return action;
    mModel->refresh();

    // The folder browser is modal over the picker, which is modal over the strip. Handled here
    // rather than inside pickerMouseDown so the ordering is stated once, where the click arrives.
    if (mBrowser.isOpen()) {
        const FileBrowser::Result result = mBrowser.handleClick(x, y);
        if (result == FileBrowser::Result::Chosen) {
            action.kind = RackAction::Kind::AddSearchPath;
            action.text = mBrowser.chosenPath();
            return action;
        }
        return RackAction::redraw();
    }

    const HitTarget hit = hitTest(x, y);
    mModel->setHover(hit);

    if (mModel->picker().open)
        return pickerMouseDown(x, y, button);

    //--- right button: the disconnect gesture ----------------------------
    if (button == 3) {
        mPressed = HitTarget();
        // Right-clicking a cable takes the pedal AFTER it out of the path. That is the plan's
        // "right-click a connection to disconnect": the node stays on the canvas, on the shelf,
        // with its state intact, and audio simply stops going through it.
        if (hit.part == HitTarget::Part::Cable) {
            const std::vector<const RackNode *> routed = mModel->section(hit.section, true);
            if (hit.slot >= 0 && hit.slot < static_cast<int>(routed.size())) {
                action.kind = RackAction::Kind::Route;
                action.section = hit.section;
                action.index = routed[static_cast<size_t>(hit.slot)]->index;
                action.toIndex = -1;
                action.flag = false;
                return action;
            }
        }
        if (hit.part == HitTarget::Part::Card && hit.nodeId != 0) {
            if (const RackNode *node = mModel->nodeById(hit.nodeId)) {
                action.kind = RackAction::Kind::Route;
                action.section = node->section;
                action.index = node->index;
                action.toIndex = -1;
                action.flag = !node->enabled;
                return action;
            }
        }
        return action;
    }

    if (button != 1)
        return action;

    mPressed = hit;

    // The mix slider tracks from the press, so a click anywhere on the track jumps to that value.
    if (hit.part == HitTarget::Part::RowMix) {
        if (const RackNode *node = mModel->nodeById(hit.nodeId)) {
            action.kind = RackAction::Kind::SetMix;
            action.section = node->section;
            action.index = node->index;
            action.value = hit.local;
        }
        return action;
    }

    // Pressing a card in the node view starts a drag rather than acting immediately; the action, if
    // any, is decided on release.
    if (hit.part == HitTarget::Part::Card && mModel->viewMode() == ViewMode::Nodes) {
        if (const RackNode *node = mModel->nodeById(hit.nodeId)) {
            NodeLayout layout;
            layoutNodes(*mModel, layout);
            Rect cardRect;
            for (const CardBox &box : layout.path)
                if (box.node == node)
                    cardRect = box.rect;
            for (const CardBox &box : layout.shelf)
                if (box.node == node)
                    cardRect = box.rect;

            DragState &drag = mModel->drag();
            drag.active = true;
            drag.nodeId = node->id;
            drag.section = node->section;
            drag.x = x;
            drag.y = y;
            drag.grabDx = x - cardRect.x;
            drag.grabDy = y - cardRect.y;
            drag.dropSlot = -1;
            drag.dropValid = false;
            return RackAction::redraw();
        }
    }

    return RackAction::redraw();
}

//------------------------------------------------------------------------
RackAction RackView::mouseUp(float x, float y, int button)
{
    RackAction action;
    if (!mModel || button != 1)
        return action;
    mModel->refresh();

    DragState &drag = mModel->drag();
    if (drag.active) {
        const RackNode *node = mModel->nodeById(drag.nodeId);
        const bool valid = drag.dropValid;
        const int slot = drag.dropSlot;
        const host::ChainSection section = drag.section;
        drag = DragState();

        if (!node)
            return RackAction::redraw();

        if (valid) {
            // Spliced into the path at `slot`: in circuit, and at that position.
            //
            // ChainBuilder::move() takes the index the node ends up at in the FINAL list, and the
            // node is lifted out before it is put back. So when it is travelling forwards, every
            // position at or beyond the target has already shifted down by one and the target has
            // to come with it. Getting this wrong lands the card one slot to the right of the cable
            // it was dropped on — which looks like the drop was ignored, not like an off-by-one.
            int target = mModel->fullIndexForRoutedSlot(section, slot);
            if (node->index < target)
                --target;

            action.kind = RackAction::Kind::Route;
            action.section = section;
            action.index = node->index;
            action.toIndex = target;
            action.flag = true;
            return action;
        }
        if (node->enabled) {
            // Dropped away from every cable: out of the path, on to the shelf.
            action.kind = RackAction::Kind::Route;
            action.section = section;
            action.index = node->index;
            action.toIndex = -1;
            action.flag = false;
            return action;
        }
        return RackAction::redraw();
    }

    const HitTarget hit = hitTest(x, y);
    // A press that slid off its control does nothing, which is what every other button in this
    // project does.
    if (hit != mPressed || hit.part == HitTarget::Part::NoPart) {
        mPressed = HitTarget();
        return RackAction();
    }
    mPressed = HitTarget();

    switch (hit.part) {
        case HitTarget::Part::ViewToggleList:
        case HitTarget::Part::ViewToggleNodes: {
            const ViewMode mode =
                hit.part == HitTarget::Part::ViewToggleList ? ViewMode::List : ViewMode::Nodes;
            if (mode == mModel->viewMode())
                return RackAction();
            action.kind = RackAction::Kind::SetViewMode;
            action.mode = mode;
            return action;
        }
        case HitTarget::Part::AddBefore:
        case HitTarget::Part::AddAfter: {
            PickerState &picker = mModel->picker();
            picker.open = true;
            picker.scroll = 0;
            picker.mode = PickerState::Mode::Plugins;
            picker.section = hit.part == HitTarget::Part::AddBefore ? host::ChainSection::Pre
                                                                    : host::ChainSection::Post;
            return RackAction::redraw();
        }
        case HitTarget::Part::Presets: {
            PickerState &picker = mModel->picker();
            // Opened fresh every time: a field left over from a previous visit would be holding the
            // keyboard for a box the user is not looking at, and an arm left over would be a
            // confirmation the user never started.
            mModel->presetEntry().clear();
            mModel->setPresetDeleteArmed(-1);
            picker.open = true;
            picker.scroll = 0;
            picker.mode = PickerState::Mode::Presets;
            return RackAction::redraw();
        }
        case HitTarget::Part::Scan: {
            PickerState &picker = mModel->picker();
            picker.open = true;
            picker.scroll = 0;
            picker.mode = PickerState::Mode::Paths;
            return RackAction::redraw();
        }
        case HitTarget::Part::Audio: {
            PickerState &picker = mModel->picker();
            picker.open = true;
            picker.scroll = 0;
            picker.mode = PickerState::Mode::Devices;
            return RackAction::redraw();
        }
        default:
            break;
    }

    const RackNode *node = mModel->nodeById(hit.nodeId);
    if (!node)
        return RackAction();

    action.section = node->section;
    action.index = node->index;

    switch (hit.part) {
        case HitTarget::Part::RowEnable:
            action.kind = RackAction::Kind::Route;
            action.toIndex = -1;
            action.flag = !node->enabled;
            return action;
        case HitTarget::Part::RowUp:
            if (node->index <= 0)
                return RackAction();
            action.kind = RackAction::Kind::Route;
            action.toIndex = node->index - 1;
            action.flag = node->enabled;
            return action;
        case HitTarget::Part::RowDown:
            action.kind = RackAction::Kind::Route;
            action.toIndex = node->index + 1;
            action.flag = node->enabled;
            return action;
        case HitTarget::Part::RowEditor:
            action.kind = RackAction::Kind::ToggleEditor;
            return action;
        case HitTarget::Part::RowRemove:
            action.kind = RackAction::Kind::Remove;
            return action;
        default:
            return RackAction();
    }
}

//------------------------------------------------------------------------
// The longest name this field will take. The same limit the amp's rename uses, and for the same
// reason: it is a name, not a note, and the row it is drawn in is one row wide.
constexpr size_t kPresetNameMaxChars = 64;

//------------------------------------------------------------------------
// Shut the overlay, and with it the name field.
//
// The field is CANCELLED rather than committed, which is where this parts company with the amp's
// channel rename that it is otherwise a copy of. There, a click elsewhere keeps what was typed,
// because renaming a channel costs nothing and is undone by typing again. Here the commit WRITES A
// FILE, and a preset appearing on disk because someone clicked past a box they had started typing
// in is a surprise with a lasting effect. Return is the save, and it is the only save.
void RackView::closePicker()
{
    if (!mModel)
        return;
    mModel->presetEntry().clear();
    mModel->setPresetDeleteArmed(-1);
    mModel->picker().open = false;
}

//------------------------------------------------------------------------
bool RackView::wantsKeyboard() const
{
    return mModel && mModel->presetEntry().active;
}

//------------------------------------------------------------------------
RackAction RackView::commitPresetName()
{
    if (!mModel || !mModel->presetEntry().active)
        return RackAction::redraw();

    std::string name = mModel->presetEntry().text;
    mModel->presetEntry().clear();

    // Trailing spaces are a typo, not a name, and a file called "lead " beside one called "lead" is
    // two presets the user cannot tell apart in a list.
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.empty())
        return RackAction::redraw();

    RackAction action;
    action.kind = RackAction::Kind::SavePreset;
    action.text = name;
    mModel->picker().open = false;
    return action;
}

//------------------------------------------------------------------------
// One key. Ported from the amp's channel-rename handler, which is the field this one is modelled
// on, and it keeps that handler's two hard-won rules.
//
// FIRST: claiming a key that was not handled is worse than missing one. The rack is a child window
// in a standalone rather than a view in a DAW, so there is no transport to swallow here today — but
// the rule is the editor's and the two fields are meant to behave identically, and a NoAction
// return is what lets a key travel on.
//
// SECOND: shift is part of typing, not a command. kShiftKey is set for every capital and for most
// punctuation, so refusing any modifier at all makes "Lead" untypeable while "lead" is fine. Only
// the three command modifiers are refused.
RackAction RackView::key(Steinberg::char16 ch, Steinberg::int16 keyCode, Steinberg::int16 modifiers)
{
    if (!mModel || !mModel->presetEntry().active)
        return RackAction();

    TextEntry &entry = mModel->presetEntry();

    switch (keyCode) {
        case Steinberg::KEY_RETURN:
        case Steinberg::KEY_ENTER:
            return commitPresetName();
        case Steinberg::KEY_ESCAPE:
            entry.clear();
            return RackAction::redraw();
        case Steinberg::KEY_BACK:
            if (entry.caret > 0)
                entry.text.erase(--entry.caret, 1);
            return RackAction::redraw();
        case Steinberg::KEY_DELETE:
            if (entry.caret < entry.text.size())
                entry.text.erase(entry.caret, 1);
            return RackAction::redraw();
        case Steinberg::KEY_LEFT:
            if (entry.caret > 0)
                --entry.caret;
            return RackAction::redraw();
        case Steinberg::KEY_RIGHT:
            if (entry.caret < entry.text.size())
                ++entry.caret;
            return RackAction::redraw();
        case Steinberg::KEY_HOME:
            entry.caret = 0;
            return RackAction::redraw();
        case Steinberg::KEY_END:
            entry.caret = entry.text.size();
            return RackAction::redraw();
        default:
            break;
    }

    const Steinberg::int16 kCommandMods = static_cast<Steinberg::int16>(
        Steinberg::kAlternateKey | Steinberg::kCommandKey | Steinberg::kControlKey);
    if ((modifiers & kCommandMods) != 0)
        return RackAction();

    // A preset name becomes a FILE name, so the characters a path cannot carry are refused here
    // rather than mangled later: a name with a slash in it would be written to a directory that
    // does not exist and reported as a save that failed for no visible reason.
    if (ch < 0x20 || ch > 0x7E)
        return RackAction();
    if (ch == '/' || ch == '\\' || ch == ':')
        return RackAction::redraw(); // consumed, deliberately does nothing
    if (entry.text.size() >= kPresetNameMaxChars)
        return RackAction::redraw(); // consumed, but the field is full

    entry.text.insert(entry.caret, 1, static_cast<char>(ch));
    ++entry.caret;
    return RackAction::redraw();
}

//------------------------------------------------------------------------
RackAction RackView::pickerMouseDown(float x, float y, int button)
{
    RackAction action;
    PickerState &picker = mModel->picker();

    if (button != 1) {
        closePicker();
        return RackAction::redraw();
    }

    // Anywhere outside the card dismisses, as the file browser does.
    if (!Rect(kPickerX, kPickerY, kPickerW, kPickerH).contains(x, y)) {
        closePicker();
        return RackAction::redraw();
    }

    const HitTarget hit = hitTest(x, y);
    if (hit.part == HitTarget::Part::PickerClose) {
        closePicker();
        return RackAction::redraw();
    }
    if (hit.part == HitTarget::Part::PickerRowRemove) {
        if (picker.mode == PickerState::Mode::Presets) {
            const std::vector<std::string> *saved = mModel->presets();
            const size_t which = static_cast<size_t>(hit.slot - kPresetFirstRow);
            if (!saved || hit.slot < kPresetFirstRow || which >= saved->size())
                return RackAction::redraw();

            // First click arms this row; a second on the SAME row is the confirmation. Arming a
            // different row moves the arm rather than deleting two things.
            if (mModel->presetDeleteArmed() != hit.slot) {
                mModel->setPresetDeleteArmed(hit.slot);
                mModel->presetEntry().clear();
                return RackAction::redraw();
            }
            mModel->setPresetDeleteArmed(-1);
            action.kind = RackAction::Kind::DeletePreset;
            action.text = (*saved)[which];
            // The overlay stays up, for the same reason the search-path one does: tidying up a
            // preset list is usually more than one deletion.
            return action;
        }

        const std::vector<SearchPathRow> *searchPaths = mModel->searchPaths();
        const size_t which = static_cast<size_t>(hit.slot - kPathFirstRow);
        if (searchPaths && hit.slot >= kPathFirstRow && which < searchPaths->size()) {
            action.kind = RackAction::Kind::RemoveSearchPath;
            action.text = (*searchPaths)[which].path;
            // The overlay stays up: removing a folder is usually one of several edits, and an
            // overlay that closed itself after each one would have to be reopened for the next.
            return action;
        }
        return RackAction::redraw();
    }

    // Anything below this line is not the armed cross, so the arm goes. A confirmation that
    // survived the user looking elsewhere would eventually fire on a click they had forgotten
    // they were halfway through.
    mModel->setPresetDeleteArmed(-1);

    if (hit.part == HitTarget::Part::PickerRow) {
        if (picker.mode == PickerState::Mode::Paths) {
            if (hit.slot == kPathScanRow) {
                action.kind = RackAction::Kind::ScanPlugins;
                return action;
            }
            if (hit.slot == kPathAddRow) {
                // The browser opens over the overlay and hands back a directory; the overlay is
                // still behind it when it closes.
                const std::vector<SearchPathRow> *searchPaths = mModel->searchPaths();
                std::string start;
                if (searchPaths && !searchPaths->empty())
                    start = searchPaths->back().path;
                openFolderBrowser(start);
                return RackAction::redraw();
            }
            return RackAction::redraw();
        }

        if (picker.mode == PickerState::Mode::Devices) {
            const std::vector<AudioDeviceRow> *devices = mModel->audioDevices();
            const size_t which = static_cast<size_t>(hit.slot);
            if (!devices || hit.slot < 0 || which >= devices->size())
                return RackAction::redraw();
            const AudioDeviceRow &dev = (*devices)[which];
            // A row that cannot be chosen, or the one already open, is not a click that does
            // anything. Reopening the device that is already open would be a silent gap in the
            // audio for no change at all.
            if (!dev.selectable || dev.current)
                return RackAction::redraw();

            action.kind = RackAction::Kind::SelectAudioDevice;
            action.text = dev.id;
            // WHICH ROW, not only which id. An id on its own could be an ASIO driver or either half
            // of a WASAPI pair, and the standalone does three different things with those.
            action.index = hit.slot;
            // THE OVERLAY STAYS OPEN, unlike every other picker here. Choosing a device closes and
            // reopens the audio device, which can fail or land somewhere other than asked, and the
            // list is where that is reported — closing it would hide the answer to the question the
            // click just asked. It also makes choosing an input and then an output one visit.
            return action;
        }

        if (picker.mode == PickerState::Mode::Presets) {
            if (hit.slot == kPresetSaveRow) {
                // Overwrite the rack that is loaded. This is what the row has always done and it
                // stays one click.
                mModel->presetEntry().clear();
                action.kind = RackAction::Kind::SavePreset;
                action.text = mModel->presetName();
                closePicker();
                return action;
            }
            if (hit.slot == kPresetNewRow) {
                // Opens the field EMPTY. Seeding it with the loaded rack's name would make the
                // obvious keystroke — click, press Return — overwrite the preset the user is on,
                // from a row that says New.
                mModel->presetEntry().begin(std::string());
                return RackAction::redraw();
            }
            // Clicking a saved rack while the field is open LOADS it and abandons what was
            // typed. The alternative — saving first — would make one click do two filesystem
            // things, only one of which was asked for.
            mModel->presetEntry().clear();

            const std::vector<std::string> *saved = mModel->presets();
            const size_t which = static_cast<size_t>(hit.slot - kPresetFirstRow);
            if (saved && hit.slot >= kPresetFirstRow && which < saved->size()) {
                action.kind = RackAction::Kind::LoadPreset;
                action.text = (*saved)[which];
                closePicker();
                return action;
            }
            return RackAction::redraw();
        }

        const std::vector<host::PluginDesc> *catalog = mModel->catalog();
        if (catalog && hit.slot >= 0 && hit.slot < static_cast<int>(catalog->size())) {
            action.kind = RackAction::Kind::Add;
            action.section = picker.section;
            action.ref = (*catalog)[static_cast<size_t>(hit.slot)].ref;
            closePicker();
            return action;
        }
    }
    return RackAction::redraw();
}

//------------------------------------------------------------------------
RackAction RackView::wheel(float x, float y, int delta)
{
    if (!mModel)
        return RackAction();
    mModel->refresh();

    if (mBrowser.isOpen())
        return mBrowser.handleWheel(delta) ? RackAction::redraw() : RackAction();

    if (mModel->picker().open) {
        const int maxScroll = std::max(0, pickerRowCount() - pickerVisibleRows());
        PickerState &picker = mModel->picker();
        const int next = std::min(std::max(picker.scroll - delta * 3, 0), maxScroll);
        if (next == picker.scroll)
            return RackAction();
        picker.scroll = next;
        return RackAction::redraw();
    }

    // The mix slider under the pointer takes the wheel: it is the one continuous value in the row,
    // and reaching for it with a drag across an 82 px track is worse than nudging it.
    const HitTarget hit = hitTest(x, y);
    if (hit.part == HitTarget::Part::RowMix) {
        if (const RackNode *node = mModel->nodeById(hit.nodeId)) {
            RackAction action;
            action.kind = RackAction::Kind::SetMix;
            action.section = node->section;
            action.index = node->index;
            action.value =
                std::min(std::max(node->mix + static_cast<float>(delta) * 0.05f, 0.0f), 1.0f);
            return action;
        }
    }

    if (mModel->viewMode() != ViewMode::List)
        return RackAction();

    std::vector<ListRow> rows;
    int totalRows = 0;
    layoutList(*mModel, rows, totalRows);
    const int maxScroll = std::max(0, totalRows - visibleListRows());
    const int next = std::min(std::max(mModel->listScroll() - delta, 0), maxScroll);
    if (next == mModel->listScroll())
        return RackAction();
    mModel->setListScroll(next);
    return RackAction::redraw();
}

} // namespace NAMp::rack
