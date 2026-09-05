// Rations edit controller — the plug-in's parameters, and the native editor it hands the host
// through createView().
//
// The two impulse-response paths, the four capture sources and the four channel names live here
// rather than being parameters, because a path and a name are not values a host can automate or
// interpolate. They are held as plain members with plain accessors — deliberately NOT a published
// COM interface. The parent plug-in exposes one so that a GUI-less host can load captures; that is
// a permanent commitment to a UID, and nothing has asked for it here yet.
//
// The authority for all ten strings is the PROCESSOR, which is the half that writes the state blob.
// What lives here is a mirror, refreshed from setComponentState, exactly as the IR paths already
// were — without it a reopened project draws every row empty while the audio plays correctly.

#pragma once

#include "engineconfig.h"
#include "midilearn.h"
#include "rationsids.h"

#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <string>
#include <vector>

namespace Rations
{

class RationsEditorView;

//------------------------------------------------------------------------
// EditControllerEx1 rather than EditController, and IMidiMapping alongside it, both for the same
// reason: a footswitch. IMidiMapping is how Control Change reaches a VST3 plug-in at all, and
// IUnitInfo (which EditControllerEx1 supplies) is how Program Change does - see midilearn.h for
// the SDK sites. Nothing about the plug-in's own parameters needed either.
class RationsController : public Steinberg::Vst::EditControllerEx1,
                          public Steinberg::Vst::IMidiMapping
{
public:
    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IEditController *)new RationsController();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    // Overridden so the editor is told about every route into a parameter — automation, a generic
    // UI, a state load and the processor's own meter and progress feedback all pass through here.
    Steinberg::tresult PLUGIN_API
    setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;
    // Receives the processor's report of what the four banks hold.
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *message) SMTG_OVERRIDE;

    // The native editor (IPlugView). The live view is tracked through the EditorView attach hooks;
    // setParamNormalized and the capability message push updates to it. All run on the host's
    // UI/run-loop thread (host contract), so mView needs no locking.
    Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;
    void editorAttached(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;
    void editorRemoved(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;

    // Asks the processor to re-send its capability message. The editor calls this while the banks
    // are still being built on worker threads, because the counts and names do not exist yet at
    // the moment the plug-in is created.
    void requestCaps();

    // The cabinet page's two impulse-response slots. `slot` is 0 (A) or 1 (B); an empty path
    // clears the slot. Out of range is a clean kInvalidArgument, never an out-of-bounds write.
    Steinberg::tresult setIrFile(int slot, const Steinberg::char8 *path);
    Steinberg::tresult getIrFile(int slot, Steinberg::char8 *buffer,
                                 Steinberg::int32 bufferSize) const;

    // The settings page's four capture rows. An empty path clears that channel. `isDirectory` is
    // the browser's own answer about what was picked, not a fresh look at the disk: a path can stop
    // being a directory between the click and the question, and what the user chose is fixed at
    // the click.
    Steinberg::tresult setCaptureSource(int channel, const Steinberg::char8 *path,
                                        bool isDirectory);

    // Publish the Slim setting to the processor, which is what makes it take effect: the parameter
    // itself only records it. Called from the editor when the Slim knob is RELEASED, never while it
    // moves — see kMsgSetSlim for why, and ChannelRack::setSlim for what it costs.
    Steinberg::tresult applySlim();
    const std::string &capturePath(int channel) const;
    bool captureIsDirectory(int channel) const;

    // What a channel is called, and the three-deep rule behind it: the user's typed override, else
    // the basename of whatever is loaded, else the channel's default name. channelName() resolves
    // all three, which is why the head panel, the level rows and the MIDI rows can each just ask.
    Steinberg::tresult setChannelName(int channel, const Steinberg::char8 *name);
    const std::string &channelNameOverride(int channel) const;
    std::string channelName(int channel) const;

    // What one MIDI learn row is called. The first four are channels, so they follow whatever the
    // user renamed that channel to; the rest are the pedalboard's footswitches, whose names are
    // the pedals' own and are not the user's to change. One function so the settings page can draw
    // nine rows without knowing which half it is on.
    std::string midiRowLabel(int row) const;

    // How many captures that channel holds, and whether they came from a folder. Both from the
    // processor's capability report, so both are zero/false until the workers have caught up.
    int entryCount(int channel) const;
    bool bankIsDirectory(int channel) const;
    // What the loaded captures state about their own levels, for greying an output mode they
    // cannot honour.
    bool bankHasLoudness(int channel) const;
    bool bankHasInputLevel(int channel) const;
    bool bankHasOutputLevel(int channel) const;

    // Whether ANY loaded channel's captures are slimmable, which is what decides whether the Slim
    // icon is drawn at all. Any and not all: Slim is one setting that rebuilds every bank, so if a
    // single loaded channel can use it the control is worth reaching - hiding it because one of
    // four banks is an older capture would put the setting out of reach of the three that can.
    // Unlike the output section's gating this does NOT follow the sounding channel, for the same
    // reason: what it enables is not per-channel.
    bool anyBankSlimmable() const;

    //---from IMidiMapping------------
    // One ParamID per controller number, on every channel and every event bus. The channel is
    // deliberately ignored: this call returns a single parameter, so sixteen channels necessarily
    // collapse onto it, and pretending otherwise here would be a lie the table could not keep.
    // See midilearn.h.
    Steinberg::tresult PLUGIN_API getMidiControllerAssignment(
        Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::CtrlNumber midiControllerNumber, Steinberg::Vst::ParamID &id) SMTG_OVERRIDE;

    //---from IUnitInfo---------------
    // Every MIDI channel of the event input maps to the MIDI unit, which is the unit carrying the
    // program list. This is the hook the host-side Program Change converter goes through; without
    // it a Program Change reaches nothing at all.
    Steinberg::tresult PLUGIN_API getUnitByBus(Steinberg::Vst::MediaType type,
                                               Steinberg::Vst::BusDirection dir,
                                               Steinberg::int32 busIndex, Steinberg::int32 channel,
                                               Steinberg::Vst::UnitID &unitId) SMTG_OVERRIDE;

    //--- MIDI learn, driven by the settings page -------------------------
    // Arm a row for learning, or pass -1 to disarm. The next matching message the PROCESSOR sees
    // is captured into that row; the editor finds out by polling, because the moment it happens is
    // on the audio thread.
    void armMidiLearn(int row);
    void clearMidiLearn(int row);
    // Ask the processor for the table. Answered with kMsgMidiTable, which lands in notify().
    void requestMidiTable();
    const MidiBinding &midiBinding(int row) const;
    int armedMidiRow() const
    {
        return mArmedRow;
    }

    //-----------------------------
    DELEGATE_REFCOUNT(Steinberg::Vst::EditControllerEx1)
    Steinberg::tresult PLUGIN_API queryInterface(const char *iid, void **obj) SMTG_OVERRIDE;
    //-----------------------------

private:
    void sendMidiRow(const char *messageID, int row);
    Steinberg::tresult receiveMidiTable(Steinberg::Vst::IMessage *message);
    Steinberg::tresult sendPath(const char *messageID, const Steinberg::char8 *path);
    static Steinberg::tresult copyPath(const std::string &src, Steinberg::char8 *buffer,
                                       Steinberg::int32 bufferSize);

    // Retitle one parameter in place. What a generic (host-drawn) parameter UI shows in place of
    // the editor's greyed-out controls: an option the loaded captures cannot honour reads
    // "Output Mode (n/a)" there rather than looking available and doing nothing.
    bool retitleParam(Steinberg::Vst::ParamID tag, const char *title);
    // Re-derive every title that depends on what is loaded, and tell the host to re-read them.
    // UI thread only — restartComponent is documented that way.
    void refreshParamTitles();

    // The three things the controller ever tells the editor. Named rather than written out as
    // `if (mView) mView->...` at each of the nine call sites, because that idiom put a reference
    // to the view's class into nine places, and macOS has no view yet (see the guarded block at
    // the foot of rationscontroller.cpp). All three are no-ops while no editor is attached, which
    // is the ordinary state for a plug-in a host has loaded but not opened.
    void notifyViewFiles();
    void notifyViewCaps();
    void notifyViewParam(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value);

    std::string mIrPath[kIrSlotCount];
    std::string mCapturePath[kChannelCount];
    bool mCaptureIsDir[kChannelCount] = {false, false, false, false};
    std::string mChannelNameOverride[kChannelCount];
    RationsEditorView *mView = nullptr; // live editor, host UI/run-loop thread only

    // Last capability report from the processor, cached so an editor created later in the session
    // can be brought up to date the moment it attaches instead of waiting for the next report.
    int mEntryCount[kChannelCount] = {0, 0, 0, 0};
    bool mBankIsDir[kChannelCount] = {false, false, false, false};
    CaptureLevels mBankLevels[kChannelCount];
    std::vector<std::string> mCaptureNames[kChannelCount];

    // The editor's copy of the learn table. NOT the authority - the processor's is, because that
    // is the one a closed editor still needs - so this is only ever written from the processor's
    // reply and from component state, never edited in place and pushed.
    MidiBinding mMidiTable[kMidiLearnRowCount];
    int mArmedRow = -1;
};

} // namespace Rations
