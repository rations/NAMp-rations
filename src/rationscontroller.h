// Rations edit controller — the plug-in's parameters, and the native editor it hands the host
// through createView().
//
// The two impulse-response paths live here rather than being parameters, because a path is not a
// value a host can automate or interpolate. They are held as plain members with plain accessors —
// deliberately NOT a published COM interface. The parent plug-in exposes one so that a GUI-less
// host can load captures, but here the captures ship in the bundle and are not a user choice
// (there is no capture browser at all), which leaves only the cabinet page's two IRs. Publishing
// an interface UID is a permanent commitment, and one is not worth making for a feature nothing
// has asked for yet.

#pragma once

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

    std::string mIrPath[kIrSlotCount];
    RationsEditorView *mView = nullptr; // live editor, host UI/run-loop thread only

    // Last capability report from the processor, cached so an editor created later in the session
    // can be brought up to date the moment it attaches instead of waiting for the next report.
    int mEntryCount[kChannelCount] = {0, 0, 0, 0};
    std::vector<std::string> mCaptureNames[kChannelCount];

    // The editor's copy of the learn table. NOT the authority - the processor's is, because that
    // is the one a closed editor still needs - so this is only ever written from the processor's
    // reply and from component state, never edited in place and pushed.
    MidiBinding mMidiTable[kMidiLearnRowCount];
    int mArmedRow = -1;
};

} // namespace Rations
