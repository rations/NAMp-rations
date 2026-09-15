// rackrender — render the rack strip offline to PNGs, and exercise its interaction rules.
//
// The counterpart to panelrender, and the phase gate for the rack UI. Three jobs:
//
//   1. RENDER. Both views and the picker overlay are drawn through the real RackView, the real
//      RackModel and the real Canvas, with no X server, no JACK, no plug-in installed and no audio
//      thread. What lands in the PNG is what the standalone puts on screen.
//
//   2. LAYOUT AUDIT. Every rect the view lays out is checked against the strip it has to fit in.
//      A control that has drifted off the edge is a silent defect on screen — the pixels are simply
//      not drawn — so it is made loud here instead.
//
//   3. INTERACTION. The hit-test and the action mapping are driven directly: click the reorder
//      arrow and assert the chain moved, right-click a cable and assert the pedal left the routed
//      path, drop a card on a cable and assert it came back. These are the rules the gate names,
//      and they are checked against ChainBuilder's actual state rather than by looking at a
//      picture.
//
// The chain is built from PLACEHOLDER nodes — real ChainBuilder entries with no instance behind
// them — so the tool needs no plug-in on the machine and produces the same output everywhere. That
// is the same mechanism a saved chain naming an uninstalled plug-in will use.
//
// Usage: rackrender [output-prefix] [resource-dir] [scale]

#include "rack/rackview.h"
#include "filebrowser.h"
#include "rack/genericpanel.h"
#include "rack/rackgeometry.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "host/chainbuilder.h"
#include "platform/respath.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace NAMp;
using namespace NAMp::rack;
// Canvas, FontStack and the file browser are the amp's; the rack draws with them.
using namespace Rations;

namespace
{

namespace fs = std::filesystem;

int gFailures = 0;

void check(bool ok, const char *what)
{
    printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++gFailures;
}

//------------------------------------------------------------------------
// A stand-in catalogue, so the picker renders the same on every machine. A real scan would produce
// this shape; using the real one would make the gate depend on what happens to be installed.
std::vector<host::PluginDesc> demoCatalog()
{
    const struct {
        const char *name;
        const char *category;
        host::PluginFormat format;
    } rows[] = {
        {"Clon Minotaur", "Fx|Distortion", host::PluginFormat::Vst3},
        {"Analog Rack Delay", "Fx|Delay", host::PluginFormat::Vst3},
        {"Analog Rack Chorus", "Fx|Modulation", host::PluginFormat::Vst3},
        {"Big Bubble Muff", "Fx|Distortion", host::PluginFormat::Vst3},
        {"Calf Vintage Delay", "Fx|Delay", host::PluginFormat::Lv2},
        {"GxTubeScreamer", "Fx|Distortion", host::PluginFormat::Lv2},
        {"TAP Reverberator", "Fx|Reverb", host::PluginFormat::Lv2},
        {"Dragonfly Hall", "Fx|Reverb", host::PluginFormat::Lv2},
        {"Wah Wah", "Fx|Filter", host::PluginFormat::Vst3},
        {"Noise Gate", "Fx|Dynamics", host::PluginFormat::Vst3},
        {"Superior Drummer", "Fx", host::PluginFormat::Vst2},
    };

    std::vector<host::PluginDesc> out;
    for (const auto &row : rows) {
        host::PluginDesc desc;
        desc.ref.format = row.format;
        desc.ref.key = std::string("/demo/") + row.name;
        desc.name = row.name;
        desc.category = row.category;
        out.push_back(std::move(desc));
    }
    return out;
}

//------------------------------------------------------------------------
// A plug-in that is not one: enough of PluginBackend to sit in a chain and be drawn. It processes
// nothing, because nothing here ever calls process() — the rack is a user interface and this tool
// runs no audio. Its only interesting answers are the latency and the editor kind, which are what
// the rack actually renders per node.
//
// It exists so the pictures show a REAL rack. Built only from placeholders, every row rendered as
// "not installed" in the danger colour, which is a truthful drawing of an untruthful chain: it hid
// the ordinary appearance of the strip and it never once exercised the gear column.
class StubBackend final : public host::PluginBackend
{
public:
    StubBackend(std::string name, uint32_t latency, bool editor)
        : mName(std::move(name)), mLatency(latency), mEditor(editor)
    {
    }

    host::PluginFormat format() const override
    {
        return host::PluginFormat::Vst3;
    }
    const char *key() const override
    {
        return mName.c_str();
    }
    const char *displayName() const override
    {
        return mName.c_str();
    }
    const char *category() const override
    {
        return "Fx";
    }

    bool prepare(const host::ProcessConfig &) override
    {
        return true;
    }
    void activate() override
    {
    }
    void deactivate() override
    {
    }
    void reset() override
    {
    }

    uint32_t latencySamples() const override
    {
        return mLatency;
    }
    bool prefersInPlace() const override
    {
        return false;
    }
    int32_t audioInCount() const override
    {
        return 2;
    }
    int32_t audioOutCount() const override
    {
        return 2;
    }

    void process(const host::AudioBlock &) noexcept override
    {
    }

    // A synthetic parameter list, so the generic panel has something real to lay out: a continuous
    // control, a stepped one, a switch and a read-only meter are the four shapes it draws
    // differently, and a stub with none of them would render an empty box that proves nothing.
    //
    // The MIDI destinations after them are the fifth shape, and the one that has to be drawn as
    // NOTHING. A plug-in that accepts MIDI declares its controller destinations as parameters
    // because the format has no other vocabulary for it, and one measured on a real machine has 128
    // of them behind six real controls. They are appended rather than interleaved, which is where a
    // plug-in actually puts them, and it leaves the row arithmetic of the checks below untouched.
    static constexpr uint32_t kStubRealParams = 14;
    static constexpr uint32_t kStubMidiParams = 8;
    static constexpr uint32_t kStubParams = kStubRealParams + kStubMidiParams;

    uint32_t paramCount() const override
    {
        return kStubParams;
    }
    bool paramInfo(uint32_t index, host::ParamInfo &out) const override
    {
        if (index >= kStubParams)
            return false;
        out = host::ParamInfo{};
        if (index >= kStubRealParams) {
            std::snprintf(out.name, sizeof(out.name), "MIDI CC %u", index - kStubRealParams);
            out.isMidiMapped = true;
            out.defaultNormalized = 0.0;
            return true;
        }
        std::snprintf(out.name, sizeof(out.name), "Parameter %u", index + 1);
        out.stepCount = (index % 4 == 1) ? 5 : ((index % 4 == 2) ? 2 : 0);
        out.isReadOnly = (index % 7 == 6);
        out.defaultNormalized = 0.5;
        return true;
    }
    double paramGet(uint32_t index) const override
    {
        return index < kStubParams ? mParams[index] : 0.0;
    }
    void paramSetFromUi(uint32_t index, double normalized) override
    {
        if (index < kStubParams)
            mParams[index] = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
    }
    bool paramDisplay(uint32_t index, double normalized, char *buf, int32_t bufLen) const override
    {
        if (index >= kStubParams || !buf || bufLen <= 0)
            return false;
        std::snprintf(buf, static_cast<size_t>(bufLen), "%.2f", normalized);
        return true;
    }
    bool paramPollFromRt(uint32_t &, double &) override
    {
        return false;
    }
    // Nothing is queued here: paramSetFromUi writes the value straight into the array above,
    // because a stub with no audio thread has nothing to marshal across.
    void paramFlushToPlugin() override
    {
    }

    bool stateSave(std::vector<uint8_t> &) const override
    {
        return false;
    }
    bool stateLoad(const uint8_t *, size_t) override
    {
        return false;
    }

    host::EditorKind editorKind() const override
    {
        return mEditor ? host::EditorKind::Vst3PlugView : host::EditorKind::NoEditor;
    }
    bool editorOpen(const host::EditorOpenRequest &, host::EditorSurface &) override
    {
        return false;
    }
    void editorIdle() override
    {
    }
    bool editorTakeResizeRequest(int32_t &, int32_t &) override
    {
        return false;
    }
    bool editorCheckSize(int32_t &, int32_t &) const override
    {
        return false;
    }
    void editorSetSize(int32_t, int32_t) override
    {
    }
    void editorShow() override
    {
    }
    void editorHide() override
    {
    }
    void editorClose() override
    {
    }

    bool hasFileLoader() const override
    {
        return false;
    }
    bool fileGet(int32_t, char *, int32_t) const override
    {
        return false;
    }
    bool fileSet(int32_t, const char *) override
    {
        return false;
    }

    // Read-AND-RESET in every real backend, and modelled that way here on purpose: the second
    // refresh() within one frame is what used to strobe the number to zero, and a stub that
    // returned a constant would never have exercised the latch that fixes it.
    int64_t diagTakeMaxMicros() override
    {
        const int64_t micros = mMicros;
        mMicros = 0;
        return micros;
    }
    uint64_t diagRtAllocCount() const override
    {
        return mAllocs;
    }
    void setDiag(int64_t micros, uint64_t allocs)
    {
        mMicros = micros;
        mAllocs = allocs;
    }

private:
    int64_t mMicros = 0;
    uint64_t mAllocs = 0;
    std::string mName;
    double mParams[kStubParams] = {0.10, 0.25, 1.00, 0.75, 0.40, 0.60, 0.33,
                                   0.90, 0.05, 0.50, 0.80, 0.20, 0.65, 0.45};
    uint32_t mLatency = 0;
    bool mEditor = true;
};

//------------------------------------------------------------------------
void addDemoNode(host::ChainBuilder &builder, host::ChainSection section, const char *name,
                 bool enabled, float mix, uint32_t latency, bool editor, bool missing = false)
{
    host::PluginRef ref;
    ref.format = host::PluginFormat::Vst3;
    ref.key = std::string("/demo/") + name;

    int index = -1;
    if (missing) {
        index = builder.addPlaceholder(section, ref, name);
    } else {
        std::string error;
        index = builder.adopt(section, std::make_unique<StubBackend>(name, latency, editor), ref,
                              error);
    }
    if (index < 0)
        return;
    builder.setEnabled(section, index, enabled);
    builder.setMix(section, index, mix);
}

//------------------------------------------------------------------------
// The rack the pictures show. Every state the strip can be in appears exactly once, so a render
// that has regressed shows it: a pedal out of circuit, a pedal that is not installed, a pedal with
// no editor of its own, a latency that is not zero, and a spread of mix values so the slider is not
// a row of identical bars.
void buildDemoChain(host::ChainBuilder &builder)
{
    builder.configure(48000.0, 256);
    addDemoNode(builder, host::ChainSection::Pre, "Noise Gate", true, 1.0f, 0, false);
    addDemoNode(builder, host::ChainSection::Pre, "Clon Minotaur", true, 1.0f, 0, true);
    addDemoNode(builder, host::ChainSection::Pre, "Wah Wah", false, 1.0f, 0, true);
    addDemoNode(builder, host::ChainSection::Post, "Analog Rack Delay", true, 0.34f, 64, true);
    addDemoNode(builder, host::ChainSection::Post, "Dragonfly Hall", true, 0.22f, 1024, true);
    addDemoNode(builder, host::ChainSection::Post, "Vintage Phaser", true, 0.5f, 0, true, true);
}

//------------------------------------------------------------------------
// Apply an action the way the standalone does, so the interaction checks below exercise the real
// mapping from a click to a chain edit rather than a second one written for the test.
bool applyAction(host::ChainBuilder &builder, RackModel &model, const RackAction &action)
{
    switch (action.kind) {
        case RackAction::Kind::NoAction:
        case RackAction::Kind::Redraw:
            return false;
        case RackAction::Kind::SetViewMode:
            model.setViewMode(action.mode);
            return false;
        case RackAction::Kind::Route: {
            builder.setEnabled(action.section, action.index, action.flag);
            if (action.toIndex >= 0)
                builder.move(action.section, action.index, action.toIndex);
            return true;
        }
        case RackAction::Kind::Remove:
            builder.remove(action.section, action.index);
            return true;
        case RackAction::Kind::Add:
            return true; // the standalone loads the plug-in here; nothing to do offline
        case RackAction::Kind::ToggleEditor:
        case RackAction::Kind::LoadPreset:
        case RackAction::Kind::SavePreset:
        case RackAction::Kind::DeletePreset:
        // Discovery: the standalone scans, edits the path list and writes it here. None of it
        // touches the chain, which is exactly why the overlay can be exercised offline.
        case RackAction::Kind::ScanPlugins:
        case RackAction::Kind::AddSearchPath:
        case RackAction::Kind::RemoveSearchPath:
        // The device, for the same reason: the standalone closes and reopens the audio device here,
        // and there is none offline. The chain is untouched either way.
        case RackAction::Kind::SelectAudioDevice:
            return false; // the standalone touches the filesystem here; nothing to do offline
        case RackAction::Kind::SetMix:
            builder.setMix(action.section, action.index, action.value);
            return true;
    }
    return false;
}

//------------------------------------------------------------------------
// A full click: press then release at the same point, which is what the view requires before it
// will act.
RackAction click(RackView &view, float x, float y, int button = 1)
{
    view.mouseMove(x, y);
    view.mouseDown(x, y, button);
    return view.mouseUp(x, y, button);
}

//------------------------------------------------------------------------
bool renderTo(const std::string &path, RackView &view, RackModel &model, const FontStack &fonts,
              double scale)
{
    const int pxW = static_cast<int>(std::lround(rackgeo::kRackW * scale));
    const int pxH = static_cast<int>(std::lround(rackgeo::kRackH * scale));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxW, pxH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "rackrender: cannot create a %dx%d surface\n", pxW, pxH);
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    // The one scale the whole design rests on, exactly as the window applies it.
    cairo_scale(cr, scale, scale);
    Canvas c(cr, &fonts, rackgeo::kRackW, rackgeo::kRackH);
    view.draw(c);
    (void)model;
    cairo_destroy(cr);

    cairo_surface_flush(surface);
    const cairo_status_t st = cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "rackrender: cannot write %s: %s\n", path.c_str(),
                cairo_status_to_string(st));
        return false;
    }
    printf("wrote      %s (%dx%d, scale %.2f)\n", path.c_str(), pxW, pxH, scale);
    return true;
}

//------------------------------------------------------------------------
// Nothing the view lays out may fall outside the strip. Checked through the public hit-test rather
// than by reaching into the layout: if a control is unreachable by a click at the place it is
// drawn, it does not matter where the rect nominally is.
void auditLayout(RackView &view, RackModel &model, const FontStack &fonts)
{
    printf("\nlayout\n");

    // The two strings set in the title face. Michroma is much wider per character than the body
    // face, so the gutters reserved for them in rackgeometry.h cannot be judged by eye or by letter
    // count — they are measured against the face that will actually draw them. Getting this wrong
    // clips the wordmark under the button beside it, which is what it did the first time.
    {
        cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t *cr = cairo_create(probe);
        Canvas measure(cr, &fonts, rackgeo::kRackW, rackgeo::kRackH);
        measure.setFont(Font::Title);

        measure.setFontSize(rackgeo::kTitleSize);
        const float titleW = measure.stringWidth("RACK");
        check(titleW + 10.0f <= rackgeo::kTitleGutter, "the RACK wordmark clears the add buttons");

        measure.setFontSize(rackgeo::kLabelSize + 1.0f);
        const float anchorW = measure.stringWidth("NAMp");
        check(anchorW + 10.0f <= rackgeo::kAnchorSubtitleDX,
              "the NAMp wordmark clears the anchor row's subtitle");

        // The preset row's delete control is a WORD in a fixed box, and the painter and the hit
        // test share that box — so a label wider than it would be a control wider than its target,
        // which is the failure mode a glyph cannot have.
        measure.setFont(Font::Body);
        measure.setFontSize(rackgeo::kSmallSize);
        check(measure.stringWidth("Delete") <= rackgeo::kPresetDeleteW &&
                  measure.stringWidth("Delete?") <= rackgeo::kPresetDeleteW,
              "both Delete labels fit the box they are hit-tested in");
        measure.setFont(Font::Title);

        // Same reasoning for the slider's end labels: kMixLabelW is the space reserved for them on
        // either side of the track, and a label wider than its reserve runs into the name column on
        // the left and the latency cell on the right.
        measure.setFont(Font::Body);
        measure.setFontSize(rackgeo::kSmallSize);
        check(measure.stringWidth("dry") <= rackgeo::kMixLabelW, "the dry label fits its reserve");
        check(measure.stringWidth("wet") <= rackgeo::kMixLabelW, "the wet label fits its reserve");

        cairo_destroy(cr);
        cairo_surface_destroy(probe);
    }

    // Every header control answers where it is drawn.
    model.setViewMode(ViewMode::List);
    view.mouseMove(rackgeo::kToggleX + 10.0f, rackgeo::kToggleY + rackgeo::kToggleH * 0.5f);
    check(model.hover().part == HitTarget::Part::ViewToggleList, "the List segment hit-tests");
    view.mouseMove(rackgeo::kToggleX + rackgeo::kToggleW - 10.0f,
                   rackgeo::kToggleY + rackgeo::kToggleH * 0.5f);
    check(model.hover().part == HitTarget::Part::ViewToggleNodes, "the Nodes segment hit-tests");
    view.mouseMove(rackgeo::kAddBeforeX + 10.0f, rackgeo::kAddY + rackgeo::kAddH * 0.5f);
    check(model.hover().part == HitTarget::Part::AddBefore, "+ Before hit-tests");
    view.mouseMove(rackgeo::kAddAfterX + 10.0f, rackgeo::kAddY + rackgeo::kAddH * 0.5f);
    check(model.hover().part == HitTarget::Part::AddAfter, "+ After hit-tests");

    check(rackgeo::kToggleX + rackgeo::kToggleW <= rackgeo::kRackW - rackgeo::kMargin,
          "the view toggle stays inside the right margin");
    check(rackgeo::kAddAfterX + rackgeo::kAddW < rackgeo::kToggleX,
          "the add buttons never reach the view toggle");

    // Every adjacent pair in the header is separated by the SAME gap. The view toggle used to be
    // one pill with a seam down it, which read as a single control rather than two buttons; now it
    // is two, and this is what keeps the four pairs from drifting apart again one literal at a
    // time.
    struct {
        float left;
        float right;
        const char *what;
    } gaps[] = {
        {rackgeo::kAddBeforeX + rackgeo::kAddW, rackgeo::kAddAfterX, "+ Before / + After"},
        {rackgeo::kScanX + rackgeo::kScanW, rackgeo::kPresetsX, "Scan / Presets"},
        {rackgeo::kPresetsX + rackgeo::kPresetsW, rackgeo::kToggleX, "Presets / List"},
        {rackgeo::kToggleX + rackgeo::kToggleSegW, rackgeo::kToggleNodesX, "List / Nodes"},
    };
    for (const auto &gap : gaps) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s are one button gap apart", gap.what);
        check(std::fabs((gap.right - gap.left) - rackgeo::kBtnGap) < 0.01f, msg);
    }
    // ...and the gap is really a gap: neither segment answers for the space between them.
    view.mouseMove(rackgeo::kToggleX + rackgeo::kToggleSegW + rackgeo::kBtnGap * 0.5f,
                   rackgeo::kToggleY + rackgeo::kToggleH * 0.5f);
    check(model.hover().part != HitTarget::Part::ViewToggleList &&
              model.hover().part != HitTarget::Part::ViewToggleNodes,
          "the space between List and Nodes belongs to neither");
    check(rackgeo::kListY + rackgeo::kListH < rackgeo::kRackH, "the list clears the footer");
    check(rackgeo::kShelfY + rackgeo::kCardH * 0.62f < rackgeo::kRackH - 14.0f,
          "the node shelf clears the footer");

    // THE FOOTER IS THREE PIECES ON ONE LINE and the device is the one in the middle, clipped to
    // whatever the other two leave it. Clipping is what makes an overlap impossible; it is also
    // what would hide the gap closing entirely, so the room left over is measured here at the size
    // the strip is really drawn — against the CARD view's hint, which is the longer of the two
    // cases.
    //
    // The left-hand figure is a worst case rather than the one on screen now: the pedal count and
    // the rack latency both grow, and a footer that fitted an empty rack and not a full one would
    // only fail once somebody had built a real chain.
    {
        cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cairo_t *cr = cairo_create(probe);
        Canvas measure(cr, &fonts, rackgeo::kRackW, rackgeo::kRackH);
        measure.setFontSize(rackgeo::kSmallSize);

        const float leftW = measure.stringWidth("64 pedals  ·  rack latency 262144 samples");
        const float hintW = measure.stringWidth(rackgeo::kFooterNodesHint);
        const float gap = (rackgeo::kRackW - rackgeo::kMargin - hintW) - rackgeo::kMargin -
                          (rackgeo::kMargin + leftW);
        check(gap >= rackgeo::kFooterDeviceMinW,
              "the audio device still has room between the pedal count and the card-view hint");

        cairo_destroy(cr);
        cairo_surface_destroy(probe);
    }

    // Every control of the first row answers, and each answers differently — a row whose cells
    // overlap would still hit-test, just always to the same thing.
    const float rowY = rackgeo::kListY + rackgeo::kRowH * 0.5f;
    const float right = rackgeo::kListX + rackgeo::kListW;
    struct {
        float x;
        HitTarget::Part part;
        const char *what;
    } cells[] = {
        {rackgeo::kListX + rackgeo::kOrderW + 13.0f, HitTarget::Part::RowEnable, "the enable dot"},
        {right - 4.0f * rackgeo::kCtlW + 13.0f, HitTarget::Part::RowUp, "the up arrow"},
        {right - 3.0f * rackgeo::kCtlW + 13.0f, HitTarget::Part::RowDown, "the down arrow"},
        {right - 1.0f * rackgeo::kCtlW + 13.0f, HitTarget::Part::RowRemove, "the remove cross"},
    };
    for (const auto &cell : cells) {
        view.mouseMove(cell.x, rowY);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s hit-tests where it is drawn", cell.what);
        check(model.hover().part == cell.part, msg);
    }

    // The mix slider covers its own track and nothing else's.
    view.mouseMove(right - 4.0f * rackgeo::kCtlW - rackgeo::kMixW + 8.0f, rowY);
    check(model.hover().part == HitTarget::Part::RowMix, "the mix track hit-tests");
}

//------------------------------------------------------------------------
void auditInteraction(host::ChainBuilder &builder, RackView &view, RackModel &model)
{
    printf("\ninteraction\n");

    auto nameAt = [&builder](host::ChainSection s, int index) {
        host::ChainNodeInfo info;
        return builder.nodeInfo(s, index, info) ? info.name : std::string("<none>");
    };
    auto enabledAt = [&builder](host::ChainSection s, int index) {
        host::ChainNodeInfo info;
        return builder.nodeInfo(s, index, info) && info.enabled;
    };

    //--- list view: reorder ------------------------------------------------
    model.setViewMode(ViewMode::List);
    model.refresh();
    check(nameAt(host::ChainSection::Pre, 0) == "Noise Gate", "the gate starts first");

    // Click the DOWN arrow on row 1. Rows run pre-first, so row 1 is the gate.
    const float right = rackgeo::kListX + rackgeo::kListW;
    const float row0Y = rackgeo::kListY + rackgeo::kRowH * 0.5f;
    RackAction action = click(view, right - 3.0f * rackgeo::kCtlW + 13.0f, row0Y);
    check(action.kind == RackAction::Kind::Route && action.toIndex == 1,
          "the down arrow asks for a move to index 1");
    applyAction(builder, model, action);
    model.refresh();
    check(nameAt(host::ChainSection::Pre, 0) == "Clon Minotaur" &&
              nameAt(host::ChainSection::Pre, 1) == "Noise Gate",
          "the chain reordered");

    // ...and back, so the rendered picture below is the documented one.
    action = click(view, right - 4.0f * rackgeo::kCtlW + 13.0f,
                   rackgeo::kListY + (rackgeo::kRowH + rackgeo::kRowGap) + rackgeo::kRowH * 0.5f);
    check(action.kind == RackAction::Kind::Route && action.toIndex == 0,
          "the up arrow asks for a move back");
    applyAction(builder, model, action);
    model.refresh();
    check(nameAt(host::ChainSection::Pre, 0) == "Noise Gate", "the reorder round-tripped");

    //--- list view: the enable dot ----------------------------------------
    check(enabledAt(host::ChainSection::Pre, 0), "the gate is in circuit to begin with");
    action = click(view, rackgeo::kListX + rackgeo::kOrderW + 13.0f, row0Y);
    check(action.kind == RackAction::Kind::Route && !action.flag,
          "clicking a lit dot asks to take the pedal out");
    applyAction(builder, model, action);
    model.refresh();
    check(!enabledAt(host::ChainSection::Pre, 0), "the gate left the routed path");
    action = click(view, rackgeo::kListX + rackgeo::kOrderW + 13.0f, row0Y);
    applyAction(builder, model, action);
    model.refresh();
    check(enabledAt(host::ChainSection::Pre, 0), "and came back");

    //--- list view: the wet/dry ends ---------------------------------------
    // The labels are inside the slider's target, so clicking a name is how you get to that end
    // exactly. Driven through mouseDown rather than click(), because the mix tracks from the press
    // — that is what makes a click anywhere on the track jump straight to the value under it.
    {
        const Rect mixCell(right - rackgeo::kRowCtlCount * rackgeo::kCtlW - rackgeo::kMixW,
                           rackgeo::kListY, rackgeo::kMixW, rackgeo::kRowH);
        const float dryX = mixCell.x + rackgeo::kMixLabelW * 0.5f;
        const float wetX = mixCell.x + rackgeo::kMixLabelW + rackgeo::kMixLabelGap +
                           rackgeo::kMixTrackW + rackgeo::kMixLabelGap + rackgeo::kMixLabelW * 0.5f;

        view.mouseMove(wetX, row0Y);
        check(model.hover().part == HitTarget::Part::RowMix, "the wet label is part of the slider");
        action = view.mouseDown(wetX, row0Y, 1);
        view.mouseUp(wetX, row0Y, 1);
        check(action.kind == RackAction::Kind::SetMix && action.value >= 1.0f,
              "clicking wet asks for fully wet");
        applyAction(builder, model, action);

        view.mouseMove(dryX, row0Y);
        check(model.hover().part == HitTarget::Part::RowMix, "the dry label is part of the slider");
        action = view.mouseDown(dryX, row0Y, 1);
        view.mouseUp(dryX, row0Y, 1);
        check(action.kind == RackAction::Kind::SetMix && action.value <= 0.0f,
              "clicking dry asks for fully dry");
        applyAction(builder, model, action);
        model.refresh();

        // And the track between them still maps linearly, which is what says the labels were added
        // beside the track rather than carved out of it.
        const float midX =
            mixCell.x + rackgeo::kMixLabelW + rackgeo::kMixLabelGap + rackgeo::kMixTrackW * 0.5f;
        action = view.mouseDown(midX, row0Y, 1);
        view.mouseUp(midX, row0Y, 1);
        check(action.kind == RackAction::Kind::SetMix && action.value > 0.4f && action.value < 0.6f,
              "the middle of the track is half wet");
        applyAction(builder, model, action);

        // ...and it DRAGS. A press keeps the pointer until the button comes up, including well off
        // the row: a 4 px track that let go the moment the pointer strayed vertically would be a
        // slider nobody can use, which is what it was.
        const float trackX = mixCell.x + rackgeo::kMixLabelW + rackgeo::kMixLabelGap;
        view.mouseMove(midX, row0Y);
        view.mouseDown(midX, row0Y, 1);
        action = view.mouseMove(trackX + rackgeo::kMixTrackW * 0.25f, row0Y);
        check(action.kind == RackAction::Kind::SetMix && action.value > 0.2f && action.value < 0.3f,
              "dragging the slider follows the pointer");
        applyAction(builder, model, action);
        model.refresh();

        // Off the strip entirely, and still tracking horizontally.
        action = view.mouseMove(trackX + rackgeo::kMixTrackW + 200.0f, row0Y + 400.0f);
        check(action.kind == RackAction::Kind::SetMix && action.value >= 1.0f,
              "...and keeps it after the pointer has left the row");
        applyAction(builder, model, action);
        model.refresh();
        view.mouseUp(trackX + rackgeo::kMixTrackW + 200.0f, row0Y + 400.0f, 1);

        // Released, the same motion asks for nothing: the drag is the button, not the pointer.
        action = view.mouseMove(trackX, row0Y + 400.0f);
        check(action.kind != RackAction::Kind::SetMix, "and lets go when the button comes up");
        action = RackAction();
    }

    //--- list view: a press that slides off does nothing --------------------
    view.mouseMove(rackgeo::kListX + rackgeo::kOrderW + 13.0f, row0Y);
    view.mouseDown(rackgeo::kListX + rackgeo::kOrderW + 13.0f, row0Y, 1);
    action = view.mouseUp(right - rackgeo::kCtlW + 13.0f, row0Y, 1);
    check(action.kind == RackAction::Kind::NoAction, "a press released elsewhere is discarded");
    check(enabledAt(host::ChainSection::Pre, 0), "...and changed nothing");

    //--- node view: right-click a cable disconnects -------------------------
    model.setViewMode(ViewMode::Nodes);
    model.refresh();
    const int routedBefore = static_cast<int>(model.section(host::ChainSection::Pre, true).size());
    check(routedBefore == 2, "two pedals are routed before the amp");

    // The cable into the first pre card. Its midpoint is between the IN terminal and that card.
    const float portY = rackgeo::kPathY + rackgeo::kCardH * 0.5f;
    const float cable0X = (rackgeo::kMargin + rackgeo::kTerminalW +
                           (rackgeo::kMargin + rackgeo::kTerminalW + rackgeo::kCardGap)) *
                          0.5f;
    view.mouseMove(cable0X, portY);
    check(model.hover().part == HitTarget::Part::Cable && model.hover().slot == 0,
          "the first cable hit-tests");
    action = view.mouseDown(cable0X, portY, 3);
    check(action.kind == RackAction::Kind::Route && !action.flag,
          "right-clicking a cable asks to take the pedal after it out");
    applyAction(builder, model, action);
    model.refresh();
    check(static_cast<int>(model.section(host::ChainSection::Pre, true).size()) == 1,
          "the pedal left the routed path");
    check(builder.count(host::ChainSection::Pre) == 3, "...but is still in the rack, not deleted");

    //--- node view: drag it back onto a cable -------------------------------
    // It is now on the shelf. Grab it there and drop it on the first cable. Which card that is
    // comes from the hit-test rather than from a second guess at the shelf's ordering — two pedals
    // are unrouted at this point, and asserting about the one the pointer did NOT pick up is how
    // this check quietly passed for the wrong reason the first time it was written.
    const float shelfX = rackgeo::kMargin + rackgeo::kTerminalW + rackgeo::kCardGap + 20.0f;
    const float shelfY = rackgeo::kShelfY + 12.0f;
    view.mouseMove(shelfX, shelfY);
    check(model.hover().part == HitTarget::Part::Card, "a shelved card hit-tests");

    const RackNode *shelved = model.nodeById(model.hover().nodeId);
    check(shelved != nullptr && !shelved->enabled, "the card under the pointer is an unrouted one");

    if (shelved) {
        const std::string draggedName = shelved->name;
        view.mouseDown(shelfX, shelfY, 1);
        check(model.drag().active, "pressing a card starts a drag");
        view.mouseMove(cable0X, portY);
        check(model.drag().dropValid && model.drag().dropSlot == 0,
              "dragging over the first cable offers slot 0");
        action = view.mouseUp(cable0X, portY, 1);
        check(action.kind == RackAction::Kind::Route && action.flag && action.toIndex == 0,
              "dropping asks to splice it in at slot 0");
        applyAction(builder, model, action);
        model.refresh();
        check(static_cast<int>(model.section(host::ChainSection::Pre, true).size()) == 2,
              "the pedal is back in the routed path");
        check(nameAt(host::ChainSection::Pre, 0) == draggedName,
              "...at the position it was dropped, not one slot past it");
    }

    //--- node view: the card carries the same slider ------------------------
    // Found by sweeping the line the slider is drawn on rather than by recomputing where a card
    // lands: layoutNodes shrinks the cards to fit the chain, so a second copy of that arithmetic
    // here would be a check that agrees with itself instead of with the view.
    {
        const float bandY = rackgeo::kPathY + rackgeo::kCardMixDY;
        float mixX = -1.0f;
        uint64_t mixNode = 0;
        for (float x = rackgeo::kMargin; x < rackgeo::kRackW - rackgeo::kMargin; x += 2.0f) {
            view.mouseMove(x, bandY);
            if (model.hover().part == HitTarget::Part::RowMix) {
                mixX = x;
                mixNode = model.hover().nodeId;
                break;
            }
        }
        check(mixX > 0.0f, "a card in the node view carries a wet/dry slider");

        const RackNode *card = model.nodeById(mixNode);
        check(card != nullptr, "...belonging to a real node");

        // The card body above it still picks the card up, so the slider took the band it needs and
        // nothing more — a card you can no longer drag would be a worse bug than no slider.
        view.mouseMove(mixX, rackgeo::kPathY + 20.0f);
        check(model.hover().part == HitTarget::Part::Card,
              "the card above the slider still starts a drag");

        if (card) {
            const uint64_t before = card->id;
            view.mouseMove(mixX, bandY);
            view.mouseDown(mixX, bandY, 1);
            check(!model.drag().active, "pressing the slider does not pick the card up");
            // Left first: a pedal starts fully wet, so dragging right would ask for the value it
            // already has and correctly produce nothing at all.
            action = view.mouseMove(0.0f, bandY);
            check(action.kind == RackAction::Kind::SetMix && action.value <= 0.0f,
                  "dragging it left asks for fully dry");
            applyAction(builder, model, action);
            model.refresh();
            action = view.mouseMove(rackgeo::kRackW, bandY + 300.0f);
            check(action.kind == RackAction::Kind::SetMix && action.value >= 1.0f,
                  "...and right, from below the strip, for fully wet");
            applyAction(builder, model, action);
            model.refresh();
            action = view.mouseMove(0.0f, bandY);
            applyAction(builder, model, action);
            view.mouseUp(0.0f, bandY, 1);
            model.refresh();
            const RackNode *after = model.nodeById(before);
            check(after && after->mix <= 0.0f, "the node the pointer was on is the one that moved");
        }
        action = RackAction();
    }

    //--- the preset overlay --------------------------------------------------
    // The one part of the strip that reaches the filesystem in the standalone, and therefore the
    // one most likely to be exercised only by hand. It is driven here with a list pushed in, no
    // disk involved, which is exactly why the rack takes its preset list rather than reading it.
    {
        const std::vector<std::string> saved = {"clean", "crunch", "default"};
        model.setPresets(&saved);
        model.setPresetName("crunch");

        action =
            click(view, rackgeo::kPresetsX + 20.0f, rackgeo::kPresetsY + rackgeo::kToggleH * 0.5f);
        check(model.picker().open && model.picker().mode == PickerState::Mode::Presets,
              "the Presets pill opens the overlay in preset mode");

        // Row 0 saves. It is deliberately not a preset, so a stray click at the top of the list
        // cannot load one.
        const float row0 =
            rackgeo::kPickerY + rackgeo::kPickerHeaderH + rackgeo::kPickerRowH * 0.5f;
        view.mouseMove(rackgeo::kPickerX + 40.0f, row0);
        check(model.hover().part == HitTarget::Part::PickerRow && model.hover().slot == 0,
              "the save row hit-tests");
        action = view.mouseDown(rackgeo::kPickerX + 40.0f, row0, 1);
        check(action.kind == RackAction::Kind::SavePreset && action.text == "crunch",
              "the save row overwrites the rack that is loaded, in one click");
        check(!model.picker().open, "and closes the overlay");
        check(!model.presetEntry().active, "with no field involved");

        // Row 1 is the one that makes a SECOND preset. It is its own row rather than a hint on the
        // row above, because a row labelled with an existing preset's name cannot also advertise
        // making a new one — the first attempt said "click to rename" there and read, correctly, as
        // renaming that preset.
        click(view, rackgeo::kPresetsX + 20.0f, rackgeo::kPresetsY + rackgeo::kToggleH * 0.5f);
        const float rowNew = row0 + rackgeo::kPickerRowH;
        view.mouseMove(rackgeo::kPickerX + 40.0f, rowNew);
        check(model.hover().part == HitTarget::Part::PickerRow &&
                  model.hover().slot == rackgeo::kPresetNewRow,
              "the new-preset row hit-tests");
        action = view.mouseDown(rackgeo::kPickerX + 40.0f, rowNew, 1);
        check(action.kind == RackAction::Kind::Redraw, "clicking it saves nothing yet");
        check(model.presetEntry().active, "it opens the name field");
        check(model.presetEntry().text.empty(),
              "EMPTY, so click-then-Return cannot overwrite the preset the user is on");
        check(model.picker().open, "and the overlay stays up to be typed into");
        check(view.wantsKeyboard(), "the window is told to take the keyboard");

        // Typing. Two ordinary characters and a backspace, which is the whole of what the field
        // has to do; the SDK's key values are what a real key press arrives as.
        auto type = [&view](char c) { return view.key(static_cast<Steinberg::char16>(c), 0, 0); };
        for (const char c : std::string("XY"))
            type(c);
        check(model.presetEntry().text == "XY", "typed characters land at the caret");
        view.key(0, Steinberg::KEY_BACK, 0);
        check(model.presetEntry().text == "X", "backspace removes one");

        // A path separator would be a save into a directory that does not exist, reported as a
        // failure with no visible cause, so the field refuses it rather than passing it on.
        type('/');
        check(model.presetEntry().text == "X", "a slash is refused, not inserted");

        // Escape abandons it, and abandoning must not write anything.
        action = view.key(0, Steinberg::KEY_ESCAPE, 0);
        check(action.kind == RackAction::Kind::Redraw && !model.presetEntry().active,
              "escape closes the field");
        check(!view.wantsKeyboard(), "and gives the keyboard back");

        // Now the real thing: open, type a NAME THAT IS NOT THE LOADED ONE, and press Return.
        // This is the whole feature — before it there was no way to produce a second preset.
        view.mouseDown(rackgeo::kPickerX + 40.0f, rowNew, 1);
        for (const char c : std::string(" lead tone "))
            type(c);
        action = view.key(0, Steinberg::KEY_RETURN, 0);
        check(action.kind == RackAction::Kind::SavePreset, "return saves");
        check(action.text == "lead tone", "under the typed name, trimmed at both ends");
        check(!model.picker().open, "and closes the overlay");
        check(!model.presetEntry().active && !view.wantsKeyboard(),
              "leaving no field open and no keyboard held");

        // A field that is open and then dismissed by clicking away must NOT write a file: the
        // commit here has a lasting effect, unlike the channel rename this field is modelled on.
        click(view, rackgeo::kPresetsX + 20.0f, rackgeo::kPresetsY + rackgeo::kToggleH * 0.5f);
        view.mouseDown(rackgeo::kPickerX + 40.0f, rowNew, 1);
        check(model.presetEntry().active, "the field is open again");
        action = view.mouseDown(4.0f, static_cast<float>(rackgeo::kRackH) - 4.0f, 1);
        check(action.kind != RackAction::Kind::SavePreset,
              "clicking outside the overlay does not save what was typed");
        check(!model.presetEntry().active && !view.wantsKeyboard(), "it just closes the field");

        // A name that is only spaces is not a file name.
        click(view, rackgeo::kPresetsX + 20.0f, rackgeo::kPresetsY + rackgeo::kToggleH * 0.5f);
        view.mouseDown(rackgeo::kPickerX + 40.0f, rowNew, 1);
        model.presetEntry().text = "   ";
        model.presetEntry().caret = 3;
        action = view.key(0, Steinberg::KEY_RETURN, 0);
        check(action.kind != RackAction::Kind::SavePreset, "an all-space name saves nothing");

        model.picker().open = true;
        model.picker().mode = PickerState::Mode::Presets;
        model.picker().scroll = 0;

        // Row 1 is the first saved rack.
        model.picker().open = true;
        model.picker().mode = PickerState::Mode::Presets;
        model.picker().scroll = 0;
        const float row1 = row0 + rackgeo::kPresetFirstRow * rackgeo::kPickerRowH;
        action = view.mouseDown(rackgeo::kPickerX + 40.0f, row1, 1);
        check(action.kind == RackAction::Kind::LoadPreset && action.text == "clean",
              "the first row under the two action rows loads the first saved rack, not the second");

        // Deleting. Two clicks, and the first one must not delete anything.
        model.picker().open = true;
        model.picker().mode = PickerState::Mode::Presets;
        model.picker().scroll = 0;
        const float deleteX = rackgeo::kPickerX + rackgeo::kPickerW - rackgeo::kPresetDeleteDX -
                              rackgeo::kPresetDeleteW * 0.5f;
        view.mouseMove(deleteX, row1);
        check(model.hover().part == HitTarget::Part::PickerRowRemove &&
                  model.hover().slot == rackgeo::kPresetFirstRow,
              "the Delete label hit-tests as its row's remove control");

        action = view.mouseDown(deleteX, row1, 1);
        check(action.kind != RackAction::Kind::DeletePreset, "the first click deletes nothing");
        check(model.presetDeleteArmed() == rackgeo::kPresetFirstRow, "it arms that row");
        check(model.picker().open, "and leaves the overlay up to be confirmed in");

        action = view.mouseDown(deleteX, row1, 1);
        check(action.kind == RackAction::Kind::DeletePreset && action.text == "clean",
              "the second click on the same row deletes it, by name");
        check(model.presetDeleteArmed() < 0, "and disarms");
        check(model.picker().open, "the overlay stays up for the next one");

        // An arm must not survive a click elsewhere, or it fires on a click the user has
        // forgotten they were halfway through.
        view.mouseDown(deleteX, row1, 1);
        check(model.presetDeleteArmed() == rackgeo::kPresetFirstRow, "armed again");
        view.mouseDown(rackgeo::kPickerX + 40.0f, row0, 1);
        check(model.presetDeleteArmed() < 0, "a click on another row disarms it");

        // Arming a DIFFERENT row moves the arm rather than deleting two presets.
        model.picker().open = true;
        model.picker().mode = PickerState::Mode::Presets;
        const float row2 = row1 + rackgeo::kPickerRowH;
        view.mouseDown(deleteX, row1, 1);
        action = view.mouseDown(deleteX, row2, 1);
        check(action.kind != RackAction::Kind::DeletePreset,
              "arming one row then clicking another deletes nothing");
        check(model.presetDeleteArmed() == rackgeo::kPresetFirstRow + 1, "the arm moved");

        model.picker().open = false;
        model.setPresetDeleteArmed(-1);
        model.picker().open = true;
        model.picker().mode = PickerState::Mode::Presets;
        model.picker().open = false;

        model.setPresets(nullptr);
        model.setPresetName(std::string());
    }

    //--- the search-path overlay --------------------------------------------
    // Same box, same rows, a third list. Driven here with a pushed-in path list and no filesystem,
    // for the same reason the presets are: the rack draws discovery, it does not perform it.
    {
        const std::vector<SearchPathRow> paths = {
            {"/usr/lib/vst3", "VST3", true},
            {"/usr/lib/lv2", "LV2", true},
            {"~/pedals", std::string(), false},
        };
        model.setSearchPaths(&paths);

        view.mouseMove(rackgeo::kScanX + 10.0f, rackgeo::kScanY + rackgeo::kToggleH * 0.5f);
        check(model.hover().part == HitTarget::Part::Scan, "the Scan pill hit-tests");
        check(rackgeo::kScanX + rackgeo::kScanW < rackgeo::kPresetsX,
              "the Scan pill clears the Presets pill");
        check(rackgeo::kAddAfterX + rackgeo::kAddW < rackgeo::kScanX,
              "...and does not collide with + After");

        action = click(view, rackgeo::kScanX + 10.0f, rackgeo::kScanY + rackgeo::kToggleH * 0.5f);
        check(model.picker().open && model.picker().mode == PickerState::Mode::Paths,
              "the Scan pill opens the overlay in search-path mode");

        const float rowY = [](int row) {
            return rackgeo::kPickerY + rackgeo::kPickerHeaderH +
                   (static_cast<float>(row) + 0.5f) * rackgeo::kPickerRowH;
        }(rackgeo::kPathScanRow);
        action = view.mouseDown(rackgeo::kPickerX + 40.0f, rowY, 1);
        check(action.kind == RackAction::Kind::ScanPlugins, "the first row asks for a scan");
        check(model.picker().open, "and leaves the overlay up, because a scan is not a choice");

        // Row 2 onwards are the folders. An automatic one has no remove cross; removing something
        // nobody added is not a thing this host can do, and offering it would be a lie.
        const float autoRowY =
            rackgeo::kPickerY + rackgeo::kPickerHeaderH +
            (static_cast<float>(rackgeo::kPathFirstRow) + 0.5f) * rackgeo::kPickerRowH;
        const float crossX = rackgeo::kPickerX + rackgeo::kPickerW - rackgeo::kPathRemoveDX;
        view.mouseMove(crossX, autoRowY);
        check(model.hover().part == HitTarget::Part::PickerRow,
              "an automatic folder has no remove cross");
        action = view.mouseDown(crossX, autoRowY, 1);
        check(action.kind == RackAction::Kind::Redraw,
              "...and clicking where one would be does nothing");

        const float userRowY =
            rackgeo::kPickerY + rackgeo::kPickerHeaderH +
            (static_cast<float>(rackgeo::kPathFirstRow) + 2.5f) * rackgeo::kPickerRowH;
        view.mouseMove(crossX, userRowY);
        check(model.hover().part == HitTarget::Part::PickerRowRemove,
              "the folder the user added does have one");
        action = view.mouseDown(crossX, userRowY, 1);
        check(action.kind == RackAction::Kind::RemoveSearchPath && action.text == "~/pedals",
              "and clicking it asks to remove that folder, by path rather than by row index");
        check(model.picker().open, "the overlay stays up for the next edit");

        // The add row opens the folder browser, which swallows input until it is dismissed.
        const float addRowY =
            rackgeo::kPickerY + rackgeo::kPickerHeaderH +
            (static_cast<float>(rackgeo::kPathAddRow) + 0.5f) * rackgeo::kPickerRowH;
        action = view.mouseDown(rackgeo::kPickerX + 40.0f, addRowY, 1);
        check(view.browserOpen(), "the add row opens the folder browser");
        action = view.mouseDown(4.0f, 4.0f, 1); // outside the card: dismiss
        check(!view.browserOpen(), "clicking outside it dismisses it");
        check(action.kind == RackAction::Kind::Redraw,
              "...without asking for a folder that was never chosen");

        model.picker().open = false;
        model.setSearchPaths(nullptr);
    }

    //--- the audio device ---------------------------------------------------
    {
        static const std::vector<AudioDeviceRow> devices = {
            {"ASIO", "Focusrite USB ASIO", "Focusrite USB ASIO", "", true, true},
            {"ASIO", "Generic Low Latency ASIO Driver", "Generic Low Latency ASIO Driver", "",
             false, true},
            {"Input", "{0.0.1.0}", "Analogue 1 + 2", "system default", false, true},
            {"ASIO", "", "no ASIO driver is installed", "", false, false},
        };
        model.setAudioDevices(&devices);

        check(rackgeo::kAudioX + rackgeo::kAudioW < rackgeo::kScanX,
              "the Audio pill clears the Scan pill");
        check(rackgeo::kAddAfterX + rackgeo::kAddW < rackgeo::kAudioX,
              "...and does not collide with + After");
        check(std::fabs((rackgeo::kScanX - (rackgeo::kAudioX + rackgeo::kAudioW)) -
                        rackgeo::kBtnGap) < 0.01f,
              "Audio / Scan are one button gap apart");

        RackAction act =
            click(view, rackgeo::kAudioX + 10.0f, rackgeo::kAudioY + rackgeo::kToggleH * 0.5f);
        check(model.picker().open && model.picker().mode == PickerState::Mode::Devices,
              "the Audio pill opens the overlay in device mode");

        const auto deviceRowY = [](int row) {
            return rackgeo::kPickerY + rackgeo::kPickerHeaderH +
                   (static_cast<float>(row) + 0.5f) * rackgeo::kPickerRowH;
        };

        // A driver that is not the one open is a choice, and it is reported BY ID rather than by
        // row alone — the standalone matches on the id and uses the row only to know which list it
        // came from, since an ASIO name and a WASAPI endpoint id are not interchangeable.
        act = view.mouseDown(rackgeo::kPickerX + 80.0f, deviceRowY(1), 1);
        check(act.kind == RackAction::Kind::SelectAudioDevice &&
                  act.text == "Generic Low Latency ASIO Driver" && act.index == 1,
              "choosing a driver asks for it by id, and says which row it came from");
        check(model.picker().open,
              "and the overlay stays up, because reopening a device can fail and this is where it "
              "would be said");

        // The one already open is not a click that does anything: reopening it would be a gap in
        // the audio for no change at all.
        act = view.mouseDown(rackgeo::kPickerX + 80.0f, deviceRowY(0), 1);
        check(act.kind == RackAction::Kind::Redraw, "the device already open cannot be re-chosen");

        // Nor is a row that is there to be read rather than chosen.
        act = view.mouseDown(rackgeo::kPickerX + 80.0f, deviceRowY(3), 1);
        check(act.kind == RackAction::Kind::Redraw, "a row that is not a device cannot be chosen");

        // An input is a different kind of choice from a driver, and the row index is what carries
        // that: the id alone would not say whether it names a driver or half of a pair.
        act = view.mouseDown(rackgeo::kPickerX + 80.0f, deviceRowY(2), 1);
        check(act.kind == RackAction::Kind::SelectAudioDevice && act.index == 2,
              "an input endpoint is chosen as its own row");

        model.picker().open = false;
        model.setAudioDevices(nullptr);
    }

    //--- the picker ---------------------------------------------------------
    action = click(view, rackgeo::kAddBeforeX + 10.0f, rackgeo::kAddY + rackgeo::kAddH * 0.5f);
    check(model.picker().open && model.picker().section == host::ChainSection::Pre,
          "+ Before opens the picker for the pre section");
    view.mouseMove(rackgeo::kPickerX + 40.0f, rackgeo::kPickerY + rackgeo::kPickerHeaderH + 8.0f);
    check(model.hover().part == HitTarget::Part::PickerRow && model.hover().slot == 0,
          "the first catalogue row hit-tests");
    action = view.mouseDown(rackgeo::kPickerX + 40.0f,
                            rackgeo::kPickerY + rackgeo::kPickerHeaderH + 8.0f, 1);
    check(action.kind == RackAction::Kind::Add && action.section == host::ChainSection::Pre,
          "clicking a row asks to add that plug-in before the amp");
    check(!model.picker().open, "and closes the picker");

    // The rack must not be editable while the picker is up: a click that lands on a row underneath
    // it would otherwise reorder the chain the user is looking away from.
    model.picker().open = true;
    model.picker().section = host::ChainSection::Post;
    action = click(view, right - 3.0f * rackgeo::kCtlW + 13.0f, row0Y);
    check(action.kind != RackAction::Kind::Route, "the picker swallows clicks meant for a row");
    model.picker().open = false;
}

//------------------------------------------------------------------------
// The node view's remove cross. Its own pass rather than part of auditInteraction, because it
// DELETES nodes and every check after it would then be asserting about a different rack — which is
// how it first broke the diagnostics pass.
void auditNodeRemoval(host::ChainBuilder &builder, RackView &view, RackModel &model)
{
    printf("\nnode removal\n");
    RackAction action;

    // Last, because it deletes nodes. Cards are found by sweeping the line the crosses sit on, for
    // the reason the mix slider is: layoutNodes sizes the cards to the chain, so recomputing where
    // one lands here would check this tool's arithmetic rather than the view's.
    {
        model.setViewMode(ViewMode::Nodes);
        model.refresh();

        const float crossY = rackgeo::kPathY + rackgeo::kCardRemoveDY + rackgeo::kCardRemoveSize;
        std::vector<std::pair<float, uint64_t>> crosses;
        uint64_t last = 0;
        for (float x = rackgeo::kMargin; x < rackgeo::kRackW - rackgeo::kMargin; x += 1.0f) {
            view.mouseMove(x, crossY);
            if (model.hover().part == HitTarget::Part::RowRemove && model.hover().nodeId != last) {
                last = model.hover().nodeId;
                crosses.push_back({x, last});
            }
        }
        check(crosses.size() == model.section(host::ChainSection::Pre, true).size() +
                                    model.section(host::ChainSection::Post, true).size(),
              "every routed card carries a remove cross, and the amp does not");

        if (!crosses.empty()) {
            const int before =
                builder.count(host::ChainSection::Pre) + builder.count(host::ChainSection::Post);
            const RackNode *doomed = model.nodeById(crosses.front().second);
            check(doomed != nullptr, "the first cross belongs to a real node");

            // The card body is still a drag, so the cross took its own target and nothing more.
            view.mouseMove(crosses.front().first, rackgeo::kPathY + 30.0f);
            check(model.hover().part == HitTarget::Part::Card,
                  "the card under the cross still starts a drag");

            action = click(view, crosses.front().first, crossY);
            check(action.kind == RackAction::Kind::Remove, "clicking the cross asks to remove");
            applyAction(builder, model, action);
            model.refresh();
            check(builder.count(host::ChainSection::Pre) +
                          builder.count(host::ChainSection::Post) ==
                      before - 1,
                  "...and the pedal left the rack entirely, not just the routed path");
        }

        // A shelved card has one too — the pedal you are most likely to be finished with was the
        // one with no control on it at all.
        model.refresh();
        const float shelfCrossY =
            rackgeo::kShelfY + rackgeo::kCardRemoveDY + rackgeo::kCardRemoveSize;
        float shelfCrossX = -1.0f;
        for (float x = rackgeo::kMargin; x < rackgeo::kRackW - rackgeo::kMargin; x += 1.0f) {
            view.mouseMove(x, shelfCrossY);
            if (model.hover().part == HitTarget::Part::RowRemove) {
                shelfCrossX = x;
                break;
            }
        }
        check(shelfCrossX > 0.0f, "a shelved card carries a remove cross too");
        if (shelfCrossX > 0.0f) {
            const int before =
                builder.count(host::ChainSection::Pre) + builder.count(host::ChainSection::Post);
            action = click(view, shelfCrossX, shelfCrossY);
            check(action.kind == RackAction::Kind::Remove, "and it removes from the shelf");
            applyAction(builder, model, action);
            model.refresh();
            check(builder.count(host::ChainSection::Pre) +
                          builder.count(host::ChainSection::Post) ==
                      before - 1,
                  "...leaving one pedal fewer in the rack");
        }
        model.setViewMode(ViewMode::List);
    }
}

} // namespace

//------------------------------------------------------------------------
// $NAMP_DIAG's cost readout. It is the one part of the strip that only appears when an environment
// variable is set, which is exactly the part most likely to rot unseen — so it is armed explicitly
// here (RackModel::setDiagArmed, which is why that flag is pushed in rather than read from getenv
// inside the drawing library) and its arithmetic is checked rather than looked at.
void auditDiagnostics(host::ChainBuilder &builder, RackModel &model)
{
    printf("\ndiagnostics\n");

    auto stubAt = [&builder](host::ChainSection s, int index) -> StubBackend * {
        // Every backend in the demo chain is a StubBackend by construction.
        return static_cast<StubBackend *>(builder.backend(s, index));
    };

    model.setDiagArmed(false);
    model.refresh();
    check(!model.nodes().empty() && model.nodes()[0].diagMicros == 0,
          "with diagnostics off no cost is read at all");

    model.setDiagArmed(true);
    // 256 frames at 48 kHz is one 5333.33 us callback: the budget the whole serial chain shares.
    model.setDiagPeriodMicros(1.0e6 * 256.0 / 48000.0);
    check(std::llround(model.diagPeriodMicros()) == 5333,
          "256 frames at 48 kHz is a 5333 us budget");

    if (StubBackend *first = stubAt(host::ChainSection::Pre, 0))
        first->setDiag(1600, 0);
    if (StubBackend *second = stubAt(host::ChainSection::Pre, 1))
        second->setDiag(400, 3);

    model.refresh();
    check(model.nodes()[0].diagMicros == 1600, "an armed refresh reads the node's worst call");
    check(model.nodes()[1].diagAllocs == 3, "...and its real-time allocation count");
    check(model.diagWorstMicros() == 1600, "the footer's worst-pedal figure is the maximum");

    // The bug this guards: refresh() runs on every hit test as well as every repaint, and the
    // backend call is read-and-reset, so the second read in a frame returns zero. Latched, the
    // number stands until a new one arrives.
    model.refresh();
    check(model.nodes()[0].diagMicros == 1600,
          "a second refresh in the same frame does not strobe the cost to zero");

    if (StubBackend *first = stubAt(host::ChainSection::Pre, 0))
        first->setDiag(4200, 0);
    model.refresh();
    check(model.nodes()[0].diagMicros == 4200, "...but a new reading replaces it");
}

//------------------------------------------------------------------------
// The generic parameter panel, offline. It is the fallback a plug-in with no showable editor gets,
// and on a machine where every installed plug-in happens to have one it would otherwise never be
// drawn until a user hit the one that does not. So it is rendered here, from a synthetic backend,
// and its two arithmetic claims are checked rather than eyeballed.
bool renderPanel(const std::string &path, const FontStack &fonts, double scale)
{
    StubBackend backend("Demo Pedal", 0, false);

    GenericPanel panel;
    panel.setBackend(&backend);
    panel.setSize(panelgeo::kPanelW, panelgeo::kDefaultPanelH);

    // A click at the far right of a slider is 1.0 and at the far left is 0.0. Getting this wrong by
    // half a thumb width is the classic slider bug and is invisible in a picture.
    const float rowY = panelgeo::kMargin + panelgeo::kHeaderH + panelgeo::kRowH * 0.5f;
    panel.mouseDown(panelgeo::kPanelW, rowY, 1);
    panel.mouseUp(panelgeo::kPanelW, rowY, 1);
    check(backend.paramGet(0) == 1.0, "dragging a slider to its right edge reaches exactly 1.0");
    panel.mouseDown(0.0f, rowY, 1);
    panel.mouseUp(0.0f, rowY, 1);
    check(backend.paramGet(0) == 0.0, "and to its left edge reaches exactly 0.0");

    // A read-only parameter has no grip and must not move when it is dragged.
    const uint32_t readOnly = 6;
    const double before = backend.paramGet(readOnly);
    const float roY = panelgeo::kMargin + panelgeo::kHeaderH +
                      static_cast<float>(readOnly) * (panelgeo::kRowH + panelgeo::kRowGap) +
                      panelgeo::kRowH * 0.5f;
    panel.mouseDown(panelgeo::kPanelW, roY, 1);
    panel.mouseUp(panelgeo::kPanelW, roY, 1);
    check(backend.paramGet(readOnly) == before, "a read-only parameter does not move when dragged");

    // A MIDI destination gets no row at all, so nothing the pointer can do reaches one. Scroll past
    // the end of the real parameters and drag every row that is on screen: with the destinations
    // listed, rows would keep coming and these values would move.
    for (uint32_t i = StubBackend::kStubRealParams; i < StubBackend::kStubParams; ++i)
        backend.paramSetFromUi(i, 0.5);
    for (int click = 0; click < 40; ++click)
        panel.wheel(0.0f, 0.0f, -1);
    for (int visual = 0; visual < 12; ++visual) {
        const float y = panelgeo::kMargin + panelgeo::kHeaderH +
                        static_cast<float>(visual) * (panelgeo::kRowH + panelgeo::kRowGap) +
                        panelgeo::kRowH * 0.5f;
        panel.mouseDown(panelgeo::kPanelW, y, 1);
        panel.mouseUp(panelgeo::kPanelW, y, 1);
    }
    bool midiUntouched = true;
    for (uint32_t i = StubBackend::kStubRealParams; i < StubBackend::kStubParams; ++i)
        midiUntouched = midiUntouched && backend.paramGet(i) == 0.5;
    check(midiUntouched, "a MIDI controller destination gets no row, so nothing can drag it");
    // The last real parameter is 13, and 13 is one of the read-only ones by the stub's own rule, so
    // the reachability check uses 12 — the last one a drag is allowed to move at all.
    check(backend.paramGet(StubBackend::kStubRealParams - 2) == 1.0,
          "...and the last real parameter is still reachable, so the list did not just end early");
    for (int click = 0; click < 40; ++click)
        panel.wheel(0.0f, 0.0f, 1);

    // Restore something worth looking at, then draw.
    for (uint32_t i = 0; i < backend.paramCount(); ++i)
        backend.paramSetFromUi(i, 0.15 + 0.06 * static_cast<double>(i % 12));

    const int pxW = static_cast<int>(std::lround(panelgeo::kPanelW * scale));
    const int pxH = static_cast<int>(std::lround(panelgeo::kDefaultPanelH * scale));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pxW, pxH);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    Canvas c(cr, &fonts, panelgeo::kPanelW, panelgeo::kDefaultPanelH);
    panel.draw(c);
    cairo_destroy(cr);

    cairo_surface_flush(surface);
    const cairo_status_t st = cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "rackrender: cannot write %s: %s\n", path.c_str(),
                cairo_status_to_string(st));
        return false;
    }
    printf("wrote      %s (%dx%d, scale %.2f)\n", path.c_str(), pxW, pxH, scale);
    return true;
}

//------------------------------------------------------------------------
// FileBrowser is a NampGfx component with two owners — the editor's capture and IR loaders, and the
// rack's add-a-folder flow — and this is the only offline harness that links NampGfx and can assert
// about behaviour rather than draw a picture. So the shared component is checked here, next to one
// of its callers, rather than in a third tool built for three checks.
//
// What matters is that the three modes answer differently about the same click. The capture loader
// offers a file OR the folder; the rack's offers only the folder, and a file row there must stay
// inert — the two share one implementation, so a change made for one caller is a change made for
// both.
void auditFileBrowser()
{
    printf("\nfile browser\n");

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "namp-rackrender-browser";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub", ec);
    {
        std::ofstream(dir / "amp_1.nam") << "{}";
    }
    {
        std::ofstream(dir / "amp_2.nam") << "{}";
    }
    if (ec || !fs::exists(dir / "amp_1.nam")) {
        check(false, "a scratch capture folder could be made");
        return;
    }

    const Rect bounds(0.0f, 0.0f, 400.0f, 300.0f);
    // Rows are 24 logical units tall under a 52-unit header, and the order is fixed by the browser
    // itself: "..", then directories, then matching files. So row 1 is "sub/" and row 2 is the
    // first capture. A wrong row here would still hit SOMETHING and pass for the wrong reason,
    // which is why each check below names what it expected to hit and the folder row is asserted
    // separately.
    auto rowY = [](int index) { return 52.0f + static_cast<float>(index) * 24.0f + 12.0f; };
    const float subY = rowY(1);
    const float fileY = rowY(2);

    {
        FileBrowser b;
        b.setBounds(bounds);
        b.open(dir.string(), "nam", "capture", FileBrowser::Mode::FileOrDirectory);
        check(b.handleClick(200.0f, subY) == FileBrowser::Result::Handled,
              "a directory row descends rather than choosing");
        check(b.isOpen() && b.chosenPath().empty(), "...and chooses nothing on the way");
        b.close();

        b.open(dir.string(), "nam", "capture", FileBrowser::Mode::FileOrDirectory);
        check(b.handleClick(200.0f, fileY) == FileBrowser::Result::Chosen,
              "a single capture can be chosen");
        check(!b.chosenIsDirectory(), "...and reports itself as a file");
        check(fs::path(b.chosenPath()).filename() == "amp_1.nam", "...by its own path");

        // The folder is still the other answer, from the same card.
        b.open(dir.string(), "nam", "capture", FileBrowser::Mode::FileOrDirectory);
        check(b.handleClick(bounds.right() - 100.0f, bounds.bottom() - 22.0f) ==
                  FileBrowser::Result::Chosen,
              "the folder button still chooses the folder");
        check(b.chosenIsDirectory(), "...and reports itself as a directory");
        check(b.chosenPath() == dir.string(), "...by its own path");
    }

    {
        // The rack's mode is unchanged: a file row there is context, not a choice.
        FileBrowser b;
        b.setBounds(bounds);
        b.setDirectoryChoice("plug-in", true, false);
        b.open(dir.string(), "nam", "folder", FileBrowser::Mode::Directory);
        check(b.handleClick(200.0f, fileY) == FileBrowser::Result::Handled,
              "a file row in directory mode still chooses nothing");
        check(b.isOpen(), "...and leaves the browser up");
    }

    {
        // And pure file mode has no folder button to press: the footer is a margin there, so the
        // click lands on empty card and is swallowed.
        FileBrowser b;
        b.setBounds(bounds);
        b.open(dir.string(), "nam", "capture", FileBrowser::Mode::File);
        check(b.handleClick(bounds.right() - 100.0f, bounds.bottom() - 22.0f) ==
                  FileBrowser::Result::Handled,
              "file mode offers no folder button");
        check(b.isOpen(), "...and stays open");
    }

    fs::remove_all(dir, ec);
}

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    const std::string prefix = argc > 1 ? argv[1] : "rack";
    const std::string res = argc > 2 ? argv[2] : resourceDir();
    const double scale = argc > 3 ? atof(argv[3]) : 1.0;
    if (scale <= 0.05 || scale > 8.0) {
        fprintf(stderr, "rackrender: implausible scale %.3f\n", scale);
        return 2;
    }

    FontStack fonts;
    if (!fonts.load(res))
        fprintf(stderr,
                "rackrender: falling back to generic faces; text will not match the panel\n");

    host::ChainBuilder builder;
    buildDemoChain(builder);

    const std::vector<host::PluginDesc> catalog = demoCatalog();

    RackModel model;
    model.setBuilder(&builder);
    model.setCatalog(&catalog);

    RackView view;
    view.setModel(&model);

    auditLayout(view, model, fonts);
    auditInteraction(builder, view, model);
    auditFileBrowser();
    auditDiagnostics(builder, model);
    auditNodeRemoval(builder, view, model);
    model.setDiagArmed(false);

    printf("\nrender\n");
    // Rebuild, so the pictures show the documented chain rather than whatever the interaction
    // checks left behind.
    host::ChainBuilder fresh;
    buildDemoChain(fresh);
    model.setBuilder(&fresh);
    model.setHover(HitTarget());

    bool ok = true;
    model.setViewMode(ViewMode::List);
    ok = renderTo(prefix + "-list.png", view, model, fonts, scale) && ok;

    // Scrolled by one, which is the only way to see the last row — and the last row is the one that
    // is not installed. A state the demo chain deliberately contains has to appear in a picture, or
    // it is not being checked by anything a human looks at.
    model.setListScroll(1);
    ok = renderTo(prefix + "-list-scrolled.png", view, model, fonts, scale) && ok;
    model.setListScroll(0);

    model.setViewMode(ViewMode::Nodes);
    ok = renderTo(prefix + "-nodes.png", view, model, fonts, scale) && ok;

    model.setViewMode(ViewMode::List);
    model.picker().open = true;
    model.picker().mode = PickerState::Mode::Plugins;
    model.picker().section = host::ChainSection::Pre;
    ok = renderTo(prefix + "-picker.png", view, model, fonts, scale) && ok;

    // The preset overlay, with a rack loaded so the "NOW" marker and the save row's name are both
    // in the picture rather than in a comment.
    const std::vector<std::string> demoRacks = {"clean", "crunch", "lead", "practice"};
    static const std::vector<SearchPathRow> demoPaths = {
        {"~/.vst3", "VST3", true},
        {"/usr/lib/vst3", "VST3", true},
        {"/usr/lib/lv2", "LV2", true},
        {"~/Audio/plugins", std::string(), false},
    };
    model.setSearchPaths(&demoPaths);
    model.picker().open = true;
    model.picker().mode = PickerState::Mode::Paths;
    model.picker().scroll = 0;
    ok = renderTo(prefix + "-paths.png", view, model, fonts, scale) && ok;
    model.picker().open = false;
    model.setSearchPaths(nullptr);

    // The device overlay. The rows are what the standalone would build on a Windows machine with an
    // interface in it — the shape this list has to read well in is a driver, two inputs and two
    // outputs, where one of each is the one that is open.
    static const std::vector<AudioDeviceRow> demoDevices = {
        {"ASIO", "Focusrite USB ASIO", "Focusrite USB ASIO", "", true, true},
        {"ASIO", "Generic Low Latency ASIO Driver", "Generic Low Latency ASIO Driver", "", false,
         true},
        {"Input", "{0.0.1.0}\\in0", "Analogue 1 + 2 (Focusrite USB)", "system default", false,
         true},
        {"Input", "{0.0.1.0}\\in1", "Microphone (Realtek High Definition Audio)", "", false, true},
        {"Output", "{0.0.0.0}\\out0", "Playback 1 + 2 (Focusrite USB)", "system default", false,
         true},
        {"Output", "{0.0.0.0}\\out1", "Speakers (Realtek High Definition Audio)", "", false, true},
    };
    model.setAudioDevices(&demoDevices);
    model.setAudioStatus("ASIO Focusrite USB ASIO, 48000 Hz, 128 frames");
    model.picker().open = true;
    model.picker().mode = PickerState::Mode::Devices;
    model.picker().scroll = 0;
    ok = renderTo(prefix + "-devices.png", view, model, fonts, scale) && ok;
    model.picker().open = false;
    model.setAudioDevices(nullptr);
    model.setAudioStatus(std::string());

    model.setPresets(&demoRacks);
    model.setPresetName("crunch");
    // open = true was MISSING here, so this picture has been rendering the plain list all along and
    // the overlay it is named after has never been in it. Found by looking at the file while adding
    // the name field, which is the whole reason these renders exist.
    model.picker().open = true;
    model.picker().mode = PickerState::Mode::Presets;
    model.picker().scroll = 0;
    ok = renderTo(prefix + "-presets.png", view, model, fonts, scale) && ok;

    // ...and again with the name field open, because a caret and a half-typed name are the parts
    // that cannot be reviewed from the row's resting state.
    model.presetEntry().begin("lead tone");
    model.presetEntry().caret = 4;
    ok = renderTo(prefix + "-preset-name.png", view, model, fonts, scale) && ok;
    model.presetEntry().clear();

    // ...and with a row armed for deletion, which is the state that has to read as a question
    // rather than as a control that has merely changed colour.
    model.setPresetDeleteArmed(rackgeo::kPresetFirstRow + 1);
    ok = renderTo(prefix + "-preset-delete.png", view, model, fonts, scale) && ok;
    model.setPresetDeleteArmed(-1);

    model.picker().open = false;
    model.setPresets(nullptr);
    model.setPresetName(std::string());

    // The diagnostics overlay, which nobody sees without setting an environment variable and which
    // therefore has to be in a picture or it is not being reviewed at all. Costs chosen to land one
    // node in each of the three bands the strip colours.
    model.setDiagArmed(true);
    model.setDiagPeriodMicros(1.0e6 * 256.0 / 48000.0);
    {
        const double period = model.diagPeriodMicros();
        const int64_t band[] = {static_cast<int64_t>(period * 0.12), // comfortable
                                static_cast<int64_t>(period * 0.48), // getting close
                                static_cast<int64_t>(period * 0.88)};
        int i = 0;
        for (const auto section : {host::ChainSection::Pre, host::ChainSection::Post}) {
            for (int n = 0; n < fresh.count(section); ++n) {
                if (auto *stub = static_cast<StubBackend *>(fresh.backend(section, n)))
                    stub->setDiag(band[i % 3], (i == 1) ? 2u : 0u);
                ++i;
            }
        }
    }
    ok = renderTo(prefix + "-diag.png", view, model, fonts, scale) && ok;
    model.setDiagArmed(false);

    // An empty rack is the first thing every user sees, and the easiest state to leave unfinished.
    host::ChainBuilder empty;
    empty.configure(48000.0, 256);
    model.setBuilder(&empty);
    ok = renderTo(prefix + "-empty.png", view, model, fonts, scale) && ok;

    ok = renderPanel(prefix + "-panel.png", fonts, scale) && ok;

    printf("\n%s\n", gFailures == 0 && ok
                         ? "PASSED - the rack lays out and behaves as its rules say"
                         : "FAILED");
    return (gFailures == 0 && ok) ? 0 : 1;
}
