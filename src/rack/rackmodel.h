// RackModel — what the rack UI knows that the chain does not.
//
// ChainBuilder is the authority on the chain: which plug-ins exist, in what order, on which side of
// the amp, enabled or not. This does NOT duplicate any of that. It holds a read-only snapshot of it
// for drawing, plus the four things that are purely the user interface's own: which view is
// showing, what the pointer is over, what is being dragged, and whether the plug-in picker is open.
//
// NO CARD POSITIONS. A first draft of the node view stored an x/y per node so cards could be
// dragged anywhere on a canvas. They are derived instead — the routed path is laid out left to
// right in signal order and everything unrouted sits on a shelf below it — because in a SERIAL
// chain a card's position carries no information the order does not already carry, and a stored
// position is then just a thing that can disagree with the truth. When the parallel split arrives
// (the plan's last phase) a branch gets its own row, which is still derived. Only the card actually
// under the pointer has a position, and it lives in DragState for the duration of the drag.
//
// THE RACK NEVER EDITS THE CHAIN. Hit-testing returns a RackAction describing what the user asked
// for; the owner applies it to ChainBuilder and republishes. That is what keeps every chain edit on
// one thread, in one place, with one publish at the end of it — and it is what lets the offline
// render tool drive the whole UI with no plug-in host at all.

#pragma once

#include "host/chainbuilder.h"
#include "host/pluginref.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NAMp::rack
{

//------------------------------------------------------------------------
enum class ViewMode {
    List,  // the whole rack, top to bottom, one row per node
    Nodes, // the routed path left to right as cards and cables, unrouted on a shelf
};

//------------------------------------------------------------------------
// One node, flattened for drawing. A copy rather than a pointer: the view must not be able to
// reach the live instance, and a snapshot cannot go stale mid-frame.
struct RackNode {
    uint64_t id = 0;
    host::ChainSection section = host::ChainSection::Pre;
    int index = 0; // position in its section's FULL list, including disabled nodes
    std::string name;
    bool enabled = true;
    float mix = 1.0f;
    uint32_t latency = 0;
    bool placeholder = false; // the plug-in is missing; audio passes through
    bool hasEditor = false;
    bool editorOpen = false;

    // $NAMP_DIAG only, and zero when it is not set. `diagMicros` is the worst single call to this
    // node's process() the audio thread has made recently; `diagAllocs` is how many times it has
    // asked the host to build an object while audio was running, which is an allocation on the
    // real-time thread that no host can prevent and every host should name.
    int64_t diagMicros = 0;
    uint64_t diagAllocs = 0;
};

//------------------------------------------------------------------------
// What the user asked for. Applied by the owner, never here.
struct RackAction {
    // NoAction, not None. <X11/Xlib.h> defines None as a macro and the standalone includes both
    // this header and Xlib in the same translation unit, so a scoped enumerator called None does
    // not survive the preprocessor. Same reason EditorKind spells its empty case NoEditor.
    enum class Kind {
        NoAction,
        Redraw,      // visual state only; nothing to apply
        SetViewMode, // `mode`
        Route,       // `section` `index` -> `toIndex` (< 0 = leave the order alone), enabled=`flag`
        Remove,      // `section` `index`
        Add,         // `ref` into `section`
        ToggleEditor,
        SetMix,       // `value`
        LoadPreset,   // `text` names a saved rack
        SavePreset,   // `text` names where to write the current one
        DeletePreset, // `text` names a saved rack to remove from disk
        // Discovery. The rack asks; the owner owns the catalogue and the path list, exactly as it
        // owns the filesystem for presets.
        ScanPlugins,      // rescan everything, honouring the cache
        AddSearchPath,    // `text` is an absolute directory
        RemoveSearchPath, // `text` is one of the user's own rows
        // The audio device. `text` is the row's id and `index` the row it came from, because the
        // standalone has to know WHICH LIST was clicked — an id alone could be an ASIO driver or
        // either half of a WASAPI pair, and those are three different things to do with it.
        SelectAudioDevice,
    };

    Kind kind = Kind::NoAction;
    ViewMode mode = ViewMode::List;
    host::ChainSection section = host::ChainSection::Pre;
    int index = -1;
    int toIndex = -1;
    bool flag = false;
    float value = 0.0f;
    host::PluginRef ref;
    std::string text; // preset name, or a directory for the search-path actions

    static RackAction redraw()
    {
        RackAction a;
        a.kind = Kind::Redraw;
        return a;
    }
};

//------------------------------------------------------------------------
// What the pointer is currently on. Kept as a coarse target rather than a rect so hover painting
// and click dispatch cannot disagree about what was hit.
struct HitTarget {
    // NoPart rather than None, for the Xlib reason given above RackAction::Kind.
    enum class Part {
        NoPart,
        ViewToggleList,
        ViewToggleNodes,
        AddBefore,
        AddAfter,
        RowEnable,
        RowUp,
        RowDown,
        RowEditor,
        RowRemove,
        RowMix,
        Card,
        Cable,     // `slot` = the gap index within the section's routed path
        PickerRow, // `slot` = the catalogue row, or the preset row when the picker is in preset
                   // mode; -1 there is the save row
        PickerClose,
        Presets,
        Scan,
        // The remove cross on one of the user's own search-path rows. Separate from PickerRow
        // because the row and the cross on it do two different things, and a coarse hit target that
        // could not tell them apart would remove a folder the user meant to look at.
        PickerRowRemove,
        // The header button that opens the device list.
        Audio,
    };

    Part part = Part::NoPart;
    uint64_t nodeId = 0;
    host::ChainSection section = host::ChainSection::Pre;
    int slot = -1;
    float local = 0.0f; // 0..1 along the part, for the mix slider

    bool operator==(const HitTarget &o) const
    {
        return part == o.part && nodeId == o.nodeId && section == o.section && slot == o.slot;
    }
    bool operator!=(const HitTarget &o) const
    {
        return !(*this == o);
    }
};

//------------------------------------------------------------------------
struct DragState {
    bool active = false;
    uint64_t nodeId = 0;
    host::ChainSection section = host::ChainSection::Pre;
    float x = 0.0f, y = 0.0f;           // pointer, logical units
    float grabDx = 0.0f, grabDy = 0.0f; // pointer offset within the card when it was grabbed
    // Where it would land if released now: >= 0 is a slot in the routed path, -1 is the shelf.
    int dropSlot = -1;
    bool dropValid = false;
};

//------------------------------------------------------------------------
// One row of the "where plug-ins are looked for" list.
//
// `automatic` rows are the ones discovery reaches on its own — measured by the host layer, not
// declared here — and cannot be removed, because removing something nobody added is not a thing
// this host can do. They are listed anyway: without them the list reads as though NAMp only looks
// where the user pointed it, which would have people adding ~/.vst3 by hand.
struct SearchPathRow {
    std::string path;
    std::string tag;       // "VST3", "LV2", or empty for a folder that serves both
    bool automatic = true; // false = the user added it and can take it away
};

//------------------------------------------------------------------------
// One device the user could open, as the picker lists it.
//
// FLAT, AND DELIBERATELY NOT A TREE. The three things being listed are genuinely different — an
// ASIO driver is one object that owns both directions, while WASAPI has a capture endpoint and a
// render endpoint that are chosen separately — so a structure that described all of them would have
// to be the union of the two and would be mostly empty whichever was in use. Instead the standalone
// flattens whatever its platform has into rows with a `group` heading, and the rack draws rows
// under headings without knowing what either means.
//
// `id` is what gets SAVED and `name` is what gets SHOWN, and they are not the same string: a WASAPI
// endpoint id is a GUID nobody should ever see, and an ASIO driver's registered name is both. The
// rack never interprets either.
struct AudioDeviceRow {
    std::string group;      // "ASIO", "Input", "Output" — the heading this row sits under
    std::string id;         // what the standalone saves and matches on
    std::string name;       // what the user reads
    std::string detail;     // rates, channels, or why it cannot be opened; may be empty
    bool current = false;   // the one that is open now
    bool selectable = true; // false for a row that is there to be read, not chosen
};

//------------------------------------------------------------------------
// What a scan is doing, for the progress line. Pushed in by the owner: the rack draws a scan, it
// does not run one.
struct ScanState {
    bool running = false;
    int index = 0;
    int total = 0;
    std::string current; // the bundle being probed, for the line under the bar
};

//------------------------------------------------------------------------
struct PickerState {
    // One overlay, two lists. They are the same box, the same rows, the same scrolling and the same
    // dismissal, and the only differences are what is listed and what clicking a row means — so
    // they are one thing with a mode rather than two overlays that would drift apart.
    enum class Mode {
        Plugins,
        Presets,
        Paths,   // where plug-ins are looked for, and the button that looks
        Devices, // which audio device the host opens
    };

    bool open = false;
    Mode mode = Mode::Plugins;
    host::ChainSection section = host::ChainSection::Pre; // Plugins mode only
    int scroll = 0;
};

//------------------------------------------------------------------------
// A typed name, for the one field in the rack that takes one.
//
// The same shape as the amp's channel rename, and deliberately so: a caret index into an ASCII
// string, no selection, no clipboard. What that buys is that the two fields behave identically to
// the user and that neither had to grow a text engine — and the reason ASCII is enough is the same
// there as here, that a name outside it is set by naming the FILE, which goes through the
// filesystem and carries whatever bytes it likes.
//
// `active` is also what says whether the window should be holding the keyboard. Nothing else in the
// rack takes keys, so a field that is not open means no focus is held, which is the whole of the
// keyboard contract the editor works under.
struct TextEntry {
    bool active = false;
    std::string text;
    size_t caret = 0;

    void begin(std::string seed)
    {
        active = true;
        text = std::move(seed);
        caret = text.size();
    }
    void clear()
    {
        active = false;
        text.clear();
        caret = 0;
    }
};

//------------------------------------------------------------------------
class RackModel
{
public:
    // Neither is owned; both must outlive this. The catalogue may be null, in which case the picker
    // opens empty and says so rather than being unreachable.
    void setBuilder(const host::ChainBuilder *builder)
    {
        mBuilder = builder;
    }
    void setCatalog(const std::vector<host::PluginDesc> *catalog)
    {
        mCatalog = catalog;
    }

    // Re-read the chain. Cheap, and called once per repaint rather than being kept in sync by
    // hand — a UI that mirrors a model incrementally is a UI that eventually disagrees with it.
    void refresh();

    const std::vector<RackNode> &nodes() const
    {
        return mNodes;
    }
    // The nodes of one section in order, enabled ones only when `routedOnly`.
    std::vector<const RackNode *> section(host::ChainSection s, bool routedOnly) const;
    const RackNode *nodeById(uint64_t id) const;

    // The full-list index a drop into routed slot `slot` of `s` corresponds to. Slots run 0..n
    // where n is the number of routed nodes, so `slot == n` means "at the end".
    int fullIndexForRoutedSlot(host::ChainSection s, int slot) const;

    uint32_t totalLatency() const
    {
        return mTotalLatency;
    }

    //--- diagnostics ----------------------------------------------------
    // When armed, every RackNode carries a cost and the strip draws it. Pushed in by the owner
    // rather than read from $NAMP_DIAG here: this is a drawing library, an environment read is a
    // dependency that does not show up in its interface, and a flag latched inside a function-local
    // static could not be exercised by the offline render tool at all. RackWindow arms it from
    // host::diagArmed(), which is the one place the environment is consulted.
    void setDiagArmed(bool armed)
    {
        mDiagArmed = armed;
    }
    bool diagArmed() const
    {
        return mDiagArmed;
    }
    // One audio callback's worth of wall clock, which is what a node's cost has to be measured
    // against to mean anything: 5.3 ms at 256 frames and 48 kHz. Zero until the owner knows the
    // block size, in which case the strip shows microseconds and no percentage.
    void setDiagPeriodMicros(double micros)
    {
        mDiagPeriodMicros = micros > 0.0 ? micros : 0.0;
    }
    double diagPeriodMicros() const
    {
        return mDiagPeriodMicros;
    }
    // Worst case across every node, for the footer.
    int64_t diagWorstMicros() const;

    ViewMode viewMode() const
    {
        return mMode;
    }
    void setViewMode(ViewMode m)
    {
        mMode = m;
    }

    const HitTarget &hover() const
    {
        return mHover;
    }
    void setHover(const HitTarget &t)
    {
        mHover = t;
    }

    int listScroll() const
    {
        return mListScroll;
    }
    void setListScroll(int rows)
    {
        mListScroll = rows < 0 ? 0 : rows;
    }

    DragState &drag()
    {
        return mDrag;
    }
    const DragState &drag() const
    {
        return mDrag;
    }

    PickerState &picker()
    {
        return mPicker;
    }
    const PickerState &picker() const
    {
        return mPicker;
    }
    const std::vector<host::PluginDesc> *catalog() const
    {
        return mCatalog;
    }

    // The saved racks the preset overlay lists, and which one is loaded. Both pushed in: NampRack
    // draws, it does not read the filesystem.
    void setPresets(const std::vector<std::string> *presets)
    {
        mPresets = presets;
    }
    const std::vector<std::string> *presets() const
    {
        return mPresets;
    }
    void setPresetName(std::string name)
    {
        mPresetName = std::move(name);
    }
    const std::string &presetName() const
    {
        return mPresetName;
    }

    // The preset overlay's name field. Open only while the overlay is, and closed by every exit
    // from it — see RackView::commitPresetName.
    TextEntry &presetEntry()
    {
        return mPresetEntry;
    }
    const TextEntry &presetEntry() const
    {
        return mPresetEntry;
    }

    // Which preset row has its delete cross armed, as a picker row index, or -1 for none.
    //
    // ARMED, RATHER THAN DELETING ON THE FIRST CLICK, because this one is not like the search-path
    // cross beside it in the same overlay. Removing a search path forgets somewhere to look and is
    // undone by adding it back; deleting a preset unlinks a file the user built by hand, and there
    // is no undo anywhere in this program. A second click on the same cross is the cheapest
    // confirmation that does not need a dialog, and every other click disarms it.
    int presetDeleteArmed() const
    {
        return mPresetDeleteArmed;
    }
    void setPresetDeleteArmed(int row)
    {
        mPresetDeleteArmed = row;
    }

    // Where plug-ins are looked for, and what a running scan is doing. Both pushed in for the same
    // reason the presets are: the rack draws discovery, it does not perform it — which is what lets
    // the offline render drive this whole overlay with no plug-in installed.
    // The devices the picker lists. Not owned; must outlive this. Null or empty is an ordinary
    // state — a machine with no device still gets a window — and the picker says so rather than
    // being unreachable.
    void setAudioDevices(const std::vector<AudioDeviceRow> *devices)
    {
        mAudioDevices = devices;
    }
    const std::vector<AudioDeviceRow> *audioDevices() const
    {
        return mAudioDevices;
    }

    void setSearchPaths(const std::vector<SearchPathRow> *paths)
    {
        mSearchPaths = paths;
    }
    const std::vector<SearchPathRow> *searchPaths() const
    {
        return mSearchPaths;
    }
    // One line describing the audio device, composed by the standalone because only it knows which
    // backend is open — the rack draws a device, it does not choose one. Empty when there is no
    // device, which is an ordinary state: the editor runs without one.
    // Returns true when the text actually changed, so the caller can repaint only then: this is
    // pushed on every UI tick and the answer is the same on almost all of them.
    bool setAudioStatus(std::string status)
    {
        if (status == mAudioStatus)
            return false;
        mAudioStatus = std::move(status);
        return true;
    }
    const std::string &audioStatus() const
    {
        return mAudioStatus;
    }
    // Dropouts the device has reported since it was opened. Shown beside the device because the two
    // are read together: a figure with no device named beside it says nothing about which device
    // produced it.
    bool setAudioDropouts(uint32_t dropouts)
    {
        if (dropouts == mAudioDropouts)
            return false;
        mAudioDropouts = dropouts;
        return true;
    }
    uint32_t audioDropouts() const
    {
        return mAudioDropouts;
    }

    ScanState &scan()
    {
        return mScan;
    }
    const ScanState &scan() const
    {
        return mScan;
    }

    // How many catalogue entries carry each format's tag, and how many were found for the first
    // time by the last scan. Counted from the catalogue rather than reported alongside it, so the
    // header line cannot disagree with the list under it.
    int catalogCount(host::PluginFormat format) const;
    int freshCount() const;

    // Editor windows are the standalone's, not the rack's, so their open/closed state is pushed in
    // rather than read out. An id that no longer exists is ignored.
    void setEditorOpen(uint64_t id, bool open);

private:
    const host::ChainBuilder *mBuilder = nullptr;
    const std::vector<host::PluginDesc> *mCatalog = nullptr;
    const std::vector<std::string> *mPresets = nullptr;
    const std::vector<SearchPathRow> *mSearchPaths = nullptr;
    std::string mPresetName;
    TextEntry mPresetEntry;
    int mPresetDeleteArmed = -1;
    ScanState mScan;

    std::vector<RackNode> mNodes;
    std::vector<uint64_t> mEditorsOpen;
    uint32_t mTotalLatency = 0;

    // PluginBackend::diagTakeMaxMicros() is read-AND-RESET, and refresh() runs on every hit test as
    // well as on every repaint — so the second call within one frame would read zero and the number
    // would strobe. Latched per node id instead: a zero reading leaves the last real one standing,
    // which is also the right behaviour when the audio thread is idle.
    struct DiagPeak {
        uint64_t id = 0;
        int64_t micros = 0;
    };
    std::vector<DiagPeak> mDiagPeaks;
    int64_t latchDiagPeak(uint64_t id, int64_t micros);
    bool mDiagArmed = false;
    double mDiagPeriodMicros = 0.0;

    const std::vector<AudioDeviceRow> *mAudioDevices = nullptr;
    std::string mAudioStatus;
    uint32_t mAudioDropouts = 0;

    ViewMode mMode = ViewMode::List;
    int mListScroll = 0;
    HitTarget mHover;
    DragState mDrag;
    PickerState mPicker;
};

} // namespace NAMp::rack
