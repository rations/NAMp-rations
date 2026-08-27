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

#include "rationsids.h"

#include "public.sdk/source/vst/vsteditcontroller.h"

#include <string>
#include <vector>

namespace Rations
{

class RationsEditorView;

//------------------------------------------------------------------------
class RationsController : public Steinberg::Vst::EditController
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

private:
    Steinberg::tresult sendPath(const char *messageID, const Steinberg::char8 *path);
    static Steinberg::tresult copyPath(const std::string &src, Steinberg::char8 *buffer,
                                       Steinberg::int32 bufferSize);

    std::string mIrPath[kIrSlotCount];
    RationsEditorView *mView = nullptr; // live editor, host UI/run-loop thread only

    // Last capability report from the processor, cached so an editor created later in the session
    // can be brought up to date the moment it attaches instead of waiting for the next report.
    int mEntryCount[kChannelCount] = {0, 0, 0, 0};
    std::vector<std::string> mCaptureNames[kChannelCount];
};

} // namespace Rations
