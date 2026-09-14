// RackWindow — the rack strip's own window, below the amp's editor in the main window.
//
// A SIBLING CHILD WINDOW, not a region of the top-level. The amp's editor creates its own child
// window inside the top-level and handles its own input on its own connection to the display; the
// top-level therefore asks for structure events only and would never see a click meant for the
// rack. Giving the rack a child window of its own means the two input paths never have to be told
// apart, and the editor's window can stay exactly what it was.
//
// PAINTING FOLLOWS THE SAME DISCIPLINE AS THE EDITOR ABOVE IT, and it is the editor's for a reason
// that applies here too: an event never paints. It sets a dirty flag, and the next timer tick
// composes the whole strip offscreen and presents it in one operation. A host that re-enters its
// run loop can therefore never recurse into drawing, and no partially drawn frame is ever on
// screen.
//
// NOTHING HERE KNOWS WHICH WINDOWING SYSTEM IT IS ON. The window, its surfaces, the keyboard focus
// and the translation of whatever the system reported into a WindowEvent all live behind
// nativewindow.h. What is left in this file is the part that is the same everywhere, and it is
// nearly all of it.
//
// ONE LOGICAL CANVAS. The strip is drawn in the same logical units as the editor and scaled by the
// same factor — one cairo_scale at compose time — so the two always agree at every window size.
// Mouse coordinates are divided by that scale before they reach RackView, which is why nothing in
// the rack code below knows what a pixel is.
//
// CHAIN EDITS HAPPEN HERE, ON THE RUN-LOOP THREAD, WITH AUDIO RUNNING. That is safe by
// construction and not by luck: ChainBuilder publishes an immutable snapshot by atomic exchange,
// the audio thread hands the old one back through a lock-free queue, and a removed plug-in is
// destroyed only once a later snapshot is live. Nothing here suspends the audio thread and nothing
// here is heard as a click.

#pragma once

#include "eventloop.h"
#include "nativewindow.h"

#include "gfx/fontstack.h"
#include "host/chainbuilder.h"
#include "rack/rackmodel.h"
#include "rack/rackview.h"

#include <functional>
#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class RackWindow
{
public:
    // Called when the user asks for a hosted plug-in's own editor. The standalone owns those
    // windows (they are top-level and outlive the rack's knowledge of them), so the rack only
    // reports the request.
    using EditorToggle = std::function<void(NAMp::host::ChainSection, int)>;
    // Called with a node's id immediately BEFORE its instance is removed from the chain. A hosted
    // editor window holds a reference to its backend, so a window left open across a removal would
    // be pointing at an object the builder is about to bury and then free. This is the one ordering
    // constraint the rack imposes on its owner.
    using EditorClose = std::function<void(uint64_t nodeId)>;
    // Called after any edit that changed the published chain, so the host can tell JACK its latency
    // moved.
    using ChainChanged = std::function<void()>;
    // Save and load a named rack. The rack strip knows nothing about the filesystem — it names a
    // preset and the standalone does the rest, which is what keeps NampRack free of file I/O and
    // lets the offline render tool drive the same overlay with no disk in the picture.
    using LoadPreset = std::function<bool(const std::string &name)>;
    using SavePreset = std::function<void(const std::string &name)>;
    // Unlink a saved rack. Separate from SavePreset rather than a flag on it, because the rack is
    // the only thing in this program that deletes a user's file and the call that does it should be
    // impossible to reach by accident.
    using DeletePreset = std::function<void(const std::string &name)>;
    // Discovery, for the same reason: the rack knows what the user asked for, the standalone owns
    // the catalogue, the path list and the file they persist to. A scan runs SYNCHRONOUSLY inside
    // this call — see the note on RackWindow::onTimer — so the callback is expected to repaint this
    // window from its progress callback and nothing else may reach the run loop meanwhile.
    using ScanPlugins = std::function<void()>;
    using EditSearchPath = std::function<void(const std::string &dir)>;
    // The user picked an audio device. `id` is the row's own id and `row` is which row it was,
    // since an id alone cannot say whether it names an ASIO driver or one half of a WASAPI pair.
    // The rack knows nothing about either: it lists what it was given and reports which line was
    // clicked.
    using SelectAudioDevice = std::function<void(const std::string &id, int row)>;

    RackWindow(EventLoop &loop, NAMp::host::ChainBuilder &builder);
    ~RackWindow();

    RackWindow(const RackWindow &) = delete;
    RackWindow &operator=(const RackWindow &) = delete;

    // Loads the fonts the editor above is set in, so the strip and the panel read as one window.
    // A failure is a warning and generic faces, never a refusal.
    void loadFonts(const std::string &resourceDir);

    void setCatalog(const std::vector<NAMp::host::PluginDesc> *catalog)
    {
        mModel.setCatalog(catalog);
    }
    void setEditorToggle(EditorToggle callback)
    {
        mEditorToggle = std::move(callback);
    }
    void setEditorClose(EditorClose callback)
    {
        mEditorClose = std::move(callback);
    }
    void setChainChanged(ChainChanged callback)
    {
        mChainChanged = std::move(callback);
    }
    void setPresetHandlers(LoadPreset load, SavePreset save, DeletePreset remove)
    {
        mLoadPreset = std::move(load);
        mSavePreset = std::move(save);
        mDeletePreset = std::move(remove);
    }
    void setDiscoveryHandlers(ScanPlugins scan, EditSearchPath add, EditSearchPath remove)
    {
        mScanPlugins = std::move(scan);
        mAddSearchPath = std::move(add);
        mRemoveSearchPath = std::move(remove);
    }
    // Where plug-ins are looked for, for the overlay to list. Not owned; must outlive this.
    void setSearchPaths(const std::vector<NAMp::rack::SearchPathRow> *paths)
    {
        mModel.setSearchPaths(paths);
        mDirty = true;
    }
    // The devices the picker offers, and what to do when one is chosen. Not owned; must outlive
    // this. Re-pushed after a change so the `current` row follows the device that actually opened.
    void setAudioDevices(const std::vector<NAMp::rack::AudioDeviceRow> *devices)
    {
        mModel.setAudioDevices(devices);
        mDirty = true;
    }
    void setAudioHandler(SelectAudioDevice select)
    {
        mSelectAudioDevice = std::move(select);
    }
    // Ask for a scan on the NEXT timer tick rather than now. A scan is synchronous and paints its
    // own progress, and the click that asked for it arrived in an X event handler — where nothing
    // in this project is allowed to paint, because a host that re-enters its run loop would then
    // recurse into drawing. Deferring by one tick keeps that intact rather than carving an
    // exception out of it.
    void requestScan()
    {
        mScanPending = true;
        mDirty = true;
    }
    // Called by the scan's progress callback: updates the banner and paints it NOW, because the run
    // loop is inside the scan and will not tick again until it finishes. Only ever reached from
    // onTimer, by way of requestScan().
    void showScanProgress(int index, int total, const std::string &current);
    void endScanProgress();
    // The saved racks the overlay lists, and which one is current. Not owned; must outlive this.
    void setPresets(const std::vector<std::string> *presets)
    {
        mModel.setPresets(presets);
        mDirty = true;
    }
    void setPresetName(std::string name)
    {
        mPresetName = std::move(name);
        mModel.setPresetName(mPresetName);
        mDirty = true;
    }
    const std::string &presetName() const
    {
        return mPresetName;
    }

    bool create(NativeHandle parent, int x, int y, int w, int h);
    void destroy();

    // Move and resize with the top-level. `scale` is the window's logical-to-pixel factor, the same
    // one the editor is using.
    void setGeometry(int x, int y, int w, int h, double scale);

    // Run-loop tick: repaints if anything asked it to.
    void onTimer();
    void invalidate()
    {
        mDirty = true;
    }

    // The audio period the diagnostic cost bars are drawn against. Pushed in rather than read out
    // because the rack knows nothing about JACK, and re-pushed whenever the block size changes —
    // a cost bar measured against a stale period is a wrong number drawn confidently.
    void setAudioPeriod(double sampleRate, int frames)
    {
        mModel.setDiagPeriodMicros(sampleRate > 0.0 && frames > 0
                                       ? 1.0e6 * static_cast<double>(frames) / sampleRate
                                       : 0.0);
    }

    // The device the standalone has open, and what it has dropped since it opened. Pushed in for
    // the same reason the period above is: the rack draws a device, it does not choose one, and it
    // knows nothing about JACK or ASIO or WASAPI.
    //
    // Repainted only when the text actually changes. This is called on every UI tick and the answer
    // is the same on almost all of them, so setting the dirty flag unconditionally would repaint
    // the whole strip thirty times a second for nothing.
    void setAudioStatus(std::string status, uint32_t dropouts)
    {
        bool changed = mModel.setAudioStatus(std::move(status));
        changed = mModel.setAudioDropouts(dropouts) || changed;
        if (changed)
            mDirty = true;
    }

    // The standalone tells the rack which hosted editors are on screen so the gear can light.
    void setEditorOpen(uint64_t nodeId, bool open);
    // The node id at a section/index, for turning an editor request back into something stable.
    uint64_t nodeIdAt(NAMp::host::ChainSection section, int index) const;

    // Applies one RackAction to the chain and republishes; returns true if the published chain
    // changed, which is what makes the host recompute its latency.
    //
    // Public because a RackAction is the project's one description of a chain edit, and more than
    // the pointer produces them: the live stress driver in the standalone drives real edits through
    // here while audio runs, and the preset loader will too. Keeping a second, parallel path from
    // "an edit" to "a published chain" is how the two would drift apart.
    bool applyAction(const NAMp::rack::RackAction &action);

    // Re-read the chain into the drawing snapshot. applyAction() does this itself; this is for a
    // caller that wants to inspect the rack before it has made its first edit.
    void refreshModel()
    {
        mModel.refresh();
        mDirty = true;
    }
    const NAMp::rack::RackModel &model() const
    {
        return mModel;
    }

private:
    void onEvent(const WindowEvent &event);
    void redraw();

    // Take or release the keyboard to match whether a text field is open. Called after anything
    // that could have opened or closed one, so there is no path that leaves the focus held by a
    // shut field.
    //
    // The mechanism is the window's; the POLICY is here, and it is the same contract the editor
    // works under. Asking the window for keys costs nothing on its own — a key is delivered only
    // while the window holds the input focus — so everything rests on the focus being taken around
    // an open field and handed straight back. Outside that the rack claims no keys at all and
    // whatever else is listening is untouched. Never a grab: a focus request the window manager
    // declines simply leaves the field untyped, which is a field that does not work rather than a
    // desktop that does not.
    void syncKeyboardFocus();

    NAMp::host::ChainBuilder &mBuilder;
    FontStack mFonts;
    NAMp::rack::RackModel mModel;
    NAMp::rack::RackView mView;

    EditorToggle mEditorToggle;
    EditorClose mEditorClose;
    ChainChanged mChainChanged;
    LoadPreset mLoadPreset;
    SavePreset mSavePreset;
    DeletePreset mDeletePreset;
    ScanPlugins mScanPlugins;
    EditSearchPath mAddSearchPath;
    EditSearchPath mRemoveSearchPath;
    SelectAudioDevice mSelectAudioDevice;
    std::string mPresetName;

    NativeWindow mWindow;
    int mWidth = 0, mHeight = 0;
    double mScale = 1.0;

    bool mDirty = true;
    bool mScanPending = false;
};

} // namespace Rations
