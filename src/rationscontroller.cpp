// Rations edit controller implementation.

#include "rationscontroller.h"
#include "rationsids.h"
#include "rationsview.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"

#include <cstdio>
#include <cstring>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::initialize(FUnknown *context)
{
    tresult result = EditControllerEx1::initialize(context);
    if (result != kResultOk)
        return result;

    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass,
                            kBypassId);

    auto *inGain =
        new Vst::RangeParameter(STR16("Input"), kInputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    inGain->setPrecision(1);
    parameters.addParameter(inGain);

    auto *outGain =
        new Vst::RangeParameter(STR16("Output"), kOutputGainId, STR16("dB"), ranges::kGainMin,
                                ranges::kGainMax, ranges::kGainDefault);
    outGain->setPrecision(1);
    parameters.addParameter(outGain);

    // The channel. One list parameter, so "exactly one channel is on" needs no invariant kept in
    // four places, and the host gets one automation lane and one MIDI target. The string order is
    // the Channel enum's order and must not be rearranged.
    auto *channel = new Vst::StringListParameter(STR16("Channel"), kChannelId);
    channel->appendString(STR16("Clean"));
    channel->appendString(STR16("Crunch"));
    channel->appendString(STR16("OD1"));
    channel->appendString(STR16("OD2"));
    channel->setNormalized(0.0); // default: Clean
    parameters.addParameter(channel);

    // One dial per channel, sweeping that channel's own bank of captures. Plain 0 .. 1 rather
    // than a capture index: a bank's size must not change a parameter's range under a host that
    // has already written automation against it.
    static const Vst::TChar *kGainTitles[kChannelCount] = {
        STR16("Clean Gain"), STR16("Crunch Gain"), STR16("OD1 Gain"), STR16("OD2 Gain")};
    for (int c = 0; c < kChannelCount; ++c) {
        auto *g =
            new Vst::RangeParameter(kGainTitles[c], kChannelGainId[c], nullptr, 0.0, 1.0, 0.0);
        g->setPrecision(2);
        parameters.addParameter(g);
    }

    // Per-channel output trim, on the settings page. The captures are already loudness-normalized
    // per capture, so what this corrects is the perceptual residual: a high-gain channel is far
    // more compressed than a clean one and reads louder at the same measured loudness. Narrow on
    // purpose - see the range's own note.
    static const Vst::TChar *kLevelTitles[kChannelCount] = {
        STR16("Clean Level"), STR16("Crunch Level"), STR16("OD1 Level"), STR16("OD2 Level")};
    for (int c = 0; c < kChannelCount; ++c) {
        auto *l =
            new Vst::RangeParameter(kLevelTitles[c], kChannelLevelId[c], STR16("dB"),
                                    ranges::kLevelMin, ranges::kLevelMax, ranges::kLevelDefault);
        l->setPrecision(1);
        parameters.addParameter(l);
    }

    auto *ngThresh = new Vst::RangeParameter(STR16("Gate"), kNoiseGateThresholdId, STR16("dB"),
                                             ranges::kNgMin, ranges::kNgMax, ranges::kNgDefault);
    ngThresh->setPrecision(1);
    parameters.addParameter(ngThresh);

    auto *bass = new Vst::RangeParameter(STR16("Bass"), kBassId, nullptr, ranges::kToneMin,
                                         ranges::kToneMax, ranges::kToneDefault);
    bass->setPrecision(1);
    parameters.addParameter(bass);

    auto *middle = new Vst::RangeParameter(STR16("Middle"), kMiddleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    middle->setPrecision(1);
    parameters.addParameter(middle);

    auto *treble = new Vst::RangeParameter(STR16("Treble"), kTrebleId, nullptr, ranges::kToneMin,
                                           ranges::kToneMax, ranges::kToneDefault);
    treble->setPrecision(1);
    parameters.addParameter(treble);

    // The gate's own toggle. Deliberately NOT on the MIDI path: it stays on for as long as the
    // user has it on. There is no tone-stack toggle — Bass, Middle and Treble are always on.
    parameters.addParameter(STR16("Gate On"), nullptr, 1, 1.0, Vst::ParameterInfo::kCanAutomate,
                            kNoiseGateOnId);

    // Cabinet blend: 0 = IR A, 1 = IR B. Inert while only one slot is filled, in which case that
    // IR runs at unity and the editor draws this control disabled.
    auto *blend = new Vst::RangeParameter(STR16("Cab Blend"), kIrBlendId, nullptr, 0.0, 1.0, 0.0);
    blend->setPrecision(2);
    parameters.addParameter(blend);

    // Processor -> editor feedback. Hidden and read-only: never automated, never persisted,
    // invisible to generic parameter UIs.
    parameters.addParameter(STR16("Input Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kInputMeterId);
    parameters.addParameter(STR16("Output Meter"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kOutputMeterId);
    parameters.addParameter(STR16("Bank Progress"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kBankProgressId);
    parameters.addParameter(STR16("Active Capture"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kActiveIndexId);
    // The channel that is SOUNDING, as opposed to kChannelId, which is the one being asked for.
    // The panel's LEDs follow this one so a lamp cannot light over a channel whose captures are
    // still being built.
    parameters.addParameter(STR16("Active Channel"), nullptr, 0, 0.0,
                            Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden,
                            kActiveChannelId);

    // --- the MIDI block ------------------------------------------------------------------
    //
    // 129 parameters nobody will ever turn. They are here because a footswitch's messages do not
    // arrive as MIDI: Control Change arrives as a parameter change routed by IMidiMapping, and
    // Program Change as a parameter change on a kIsProgramChange parameter found through
    // IUnitInfo. Both need a real parameter to land on or they do not arrive at all. midilearn.h
    // carries the SDK sites this was verified against.
    //
    // Their own Unit, so a host lists them under "MIDI" rather than beside Bass and Treble.
    addUnit(new Vst::Unit(STR16("MIDI"), kMidiUnitId, Vst::kRootUnitId, kMidiProgramListId));

    // FLAGS 0, and this is not an oversight. kIsHidden would be the obvious choice for a
    // parameter with no UI, and the SDK documents it as implying kIsReadOnly - which would make
    // these unwritable and silently discard every CC the host routed to them. Flags 0 is the
    // SDK's own pattern for MIDI-mapped parameters.
    for (int cc = 0; cc < kMidiCcCount; ++cc) {
        char ascii[32];
        snprintf(ascii, sizeof(ascii), "MIDI CC %d", cc);
        Vst::String128 title = {};
        UString(title, USTRINGSIZE(title)).fromAscii(ascii);
        parameters.addParameter(title, nullptr, 0, 0.0, 0,
                                static_cast<Vst::ParamID>(kMidiCcBaseId + cc), kMidiUnitId);
    }

    // Program Change. A program LIST rather than a plain parameter, because that is the only
    // route a Program Change actually travels: the host looks up the unit for the incoming MIDI
    // channel, finds that unit's program list, and writes the program number onto the list's
    // kIsProgramChange parameter. ProgramList builds that parameter itself, with the list id as
    // the ParamID, which is why kMidiProgramListId and kMidiProgramChangeId are the same number.
    //
    // The programs are named for what they are - the message, not a preset. This plug-in has no
    // presets and the list is not pretending to be one; it is the doorway a PC number comes
    // through, and the learn table decides what any given number does.
    auto *programs = new Vst::ProgramList(STR16("Program Change"), kMidiProgramListId, kMidiUnitId);
    for (int pc = 0; pc < kMidiProgramCount; ++pc) {
        char ascii[32];
        snprintf(ascii, sizeof(ascii), "PC %d", pc);
        Vst::String128 name = {};
        UString(name, USTRINGSIZE(name)).fromAscii(ascii);
        programs->addProgram(name);
    }
    addProgramList(programs);
    if (Vst::Parameter *pcParam = programs->getParameter())
        parameters.addParameter(pcParam);

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::queryInterface(const char *iid, void **obj)
{
    QUERY_INTERFACE(iid, obj, Vst::IMidiMapping::iid, Vst::IMidiMapping)
    return EditControllerEx1::queryInterface(iid, obj);
}

//------------------------------------------------------------------------
// Controller numbers 0 .. 127 only. 128 and 129 are aftertouch and pitch bend, which are not
// switches and have nothing to learn; 130 and up are not controller numbers at all - the SDK's
// kCountCtrlNumber is 130, the same value as kCtrlProgramChange, because everything from there up
// belongs to the legacy MIDI-CC-OUT namespace. Answering for those would be answering a question
// the host is not asking, and the SDK's own validator says so.
tresult PLUGIN_API RationsController::getMidiControllerAssignment(int32 busIndex, int16 /*channel*/,
                                                                  Vst::CtrlNumber ccNumber,
                                                                  Vst::ParamID &id)
{
    if (busIndex != 0 || ccNumber < 0 || ccNumber >= kMidiCcCount)
        return kResultFalse;
    id = static_cast<Vst::ParamID>(kMidiCcBaseId + ccNumber);
    return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::getUnitByBus(Vst::MediaType type, Vst::BusDirection dir,
                                                   int32 busIndex, int32 channel,
                                                   Vst::UnitID &unitId)
{
    // Every channel of the one event input, onto the one unit that has a program list. Rations
    // has no per-channel behaviour to express here: a Program Change means the same thing
    // whichever channel the pedal sends it on, which is the same limitation CC has and for the
    // same reason (midilearn.h).
    if (type == Vst::kEvent && dir == Vst::kInput && busIndex == 0 && channel >= 0 &&
        channel < 16) {
        unitId = kMidiUnitId;
        return kResultTrue;
    }
    return kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::setComponentState(IBStream *state)
{
    // Mirror of RationsProcessor::getState — keep the two in sync.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > kStateVersion)
        return kResultFalse;

    static const Vst::ParamID kOrder[] = {
        kBypassId, kInputGainId, kOutputGainId, kNoiseGateThresholdId,
        kBassId,   kMiddleId,    kTrebleId,     kNoiseGateOnId};
    for (Vst::ParamID id : kOrder) {
        double v = 0.0;
        if (!streamer.readDouble(v))
            return kResultFalse;
        setParamNormalized(id, v);
    }

    double channel = 0.0;
    if (!streamer.readDouble(channel))
        return kResultFalse;
    setParamNormalized(kChannelId, channel);

    for (int c = 0; c < kChannelCount; ++c) {
        double g = 0.0;
        if (!streamer.readDouble(g))
            return kResultFalse;
        setParamNormalized(kChannelGainId[c], g);
    }

    double blend = 0.0;
    if (!streamer.readDouble(blend))
        return kResultFalse;
    setParamNormalized(kIrBlendId, blend);

    // The two IR paths follow. They are not parameters, but they are still read here, because the
    // editor has no other way to learn them: getIrFile() answers out of this controller's own copy
    // and nothing asks the processor. Skip this and reopening a project leaves both cabinet rows
    // reading "no IR loaded" while the IRs are audibly playing — and, worse since the second slot
    // became real, leaves the blend dial drawn disabled over a blend that is actually running.
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        mIrPath[slot].clear();
        if (char8 *p = streamer.readStr8()) {
            mIrPath[slot] = p;
            delete[] p;
        }
    }

    // The MIDI learn table, added in state version 2. A version 1 blob simply has nothing here,
    // which is not a failure: it is a project saved before the pedal could do anything, and it
    // opens with an unlearned table.
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        mMidiTable[row] = MidiBinding();
    if (version >= 2) {
        for (int row = 0; row < kMidiLearnRowCount; ++row) {
            int32 word = 0;
            if (!streamer.readInt32(word))
                return kResultFalse;
            mMidiTable[row] = unpackBinding(static_cast<std::uint32_t>(word));
        }
    }

    // The per-channel trims, added in state version 3. A version 1 or 2 project has nothing here
    // and opens with every trim at its default, which is 0 dB.
    for (int c = 0; c < kChannelCount; ++c) {
        double level = 0.5;
        if (version >= 3 && !streamer.readDouble(level))
            return kResultFalse;
        setParamNormalized(kChannelLevelId[c], level);
    }

    if (mView)
        mView->FilesChanged();
    return kResultOk;
}

//------------------------------------------------------------------------
// The processor reporting what the four banks hold. The message comes from our own processor, but
// it is still parsed defensively: a truncated or absent blob must leave empty lists rather than an
// out-of-range read, because the editor indexes into them.
tresult PLUGIN_API RationsController::notify(Vst::IMessage *message)
{
    const char *id = message ? message->getMessageID() : nullptr;
    if (id && strcmp(id, kMsgMidiTable) == 0)
        return receiveMidiTable(message);
    if (!id || strcmp(id, kMsgModelCaps) != 0)
        return EditControllerEx1::notify(message);

    Vst::IAttributeList *attrs = message->getAttributes();
    if (!attrs)
        return kResultFalse;

    for (int c = 0; c < kChannelCount; ++c) {
        int64 count = 0;
        std::string attr(kCapsEntryCountAttr);
        attr += kChannelDirName[c];
        if (attrs->getInt(attr.c_str(), count) != kResultOk || count < 0)
            count = 0;
        mEntryCount[c] = static_cast<int>(count);
        mCaptureNames[c].clear();
    }

    // Names arrive as UTF-8, '\n' between captures and '\f' between channels — one blob rather
    // than four, because setBinary is per-attribute and four attributes would be four chances for
    // the channels to disagree about how many there are.
    const void *data = nullptr;
    uint32 size = 0;
    if (attrs->getBinary(kCapsNamesAttr, data, size) == kResultOk && data && size > 0) {
        const char *begin = static_cast<const char *>(data);
        const char *end = begin + size;
        int channel = 0;
        const char *p = begin;
        while (p < end && channel < kChannelCount) {
            const char *ff =
                static_cast<const char *>(memchr(p, '\f', static_cast<size_t>(end - p)));
            const char *stop = ff ? ff : end;
            for (const char *q = p; q < stop;) {
                const char *nl =
                    static_cast<const char *>(memchr(q, '\n', static_cast<size_t>(stop - q)));
                const char *lineEnd = nl ? nl : stop;
                if (lineEnd > q)
                    mCaptureNames[channel].emplace_back(q, static_cast<size_t>(lineEnd - q));
                q = nl ? nl + 1 : stop;
            }
            ++channel;
            p = ff ? ff + 1 : end;
        }
    }

    if (mView)
        mView->ModelCapsChanged(mEntryCount, mCaptureNames);
    return kResultOk;
}

//------------------------------------------------------------------------
IPlugView *PLUGIN_API RationsController::createView(FIDString name)
{
    if (name && strcmp(name, Vst::ViewType::kEditor) == 0)
        return new RationsEditorView(this);
    return nullptr;
}

// The view registers itself here rather than in createView, because a host may create a view and
// never attach it (the SDK validator does exactly that). Both hooks run on the host's UI thread,
// the same thread setParamNormalized and notify() arrive on, so mView needs no locking.
void RationsController::editorAttached(Vst::EditorView *editor)
{
    mView = static_cast<RationsEditorView *>(editor);
    if (!mView)
        return;
    // Push the current values in immediately: the editor has just built its window and knows
    // nothing yet, and without this it would paint every control at its default until the next
    // parameter change happened to arrive.
    for (int32 i = 0; i < parameters.getParameterCount(); ++i) {
        if (Vst::Parameter *p = parameters.getParameterByIndex(i))
            mView->ParamChanged(p->getInfo().id, p->getNormalized());
    }
    mView->ModelCapsChanged(mEntryCount, mCaptureNames);
}

void RationsController::editorRemoved(Vst::EditorView *editor)
{
    if (mView == static_cast<RationsEditorView *>(editor))
        mView = nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::setParamNormalized(Vst::ParamID tag, Vst::ParamValue value)
{
    const tresult result = EditController::setParamNormalized(tag, value);
    if (mView && result == kResultOk)
        mView->ParamChanged(tag, value);
    return result;
}

//------------------------------------------------------------------------
void RationsController::requestCaps()
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgRequestCaps);
    sendMessage(message);
}

//------------------------------------------------------------------------
// Forward a path to the processor over the connection. When no peer is connected (a host
// inspecting the controller alone), the local copy is still updated and kResultFalse is returned.
tresult RationsController::sendPath(const char *messageID, const char8 *path)
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(messageID);
    const char *p = path ? path : "";
    message->getAttributes()->setBinary(kMsgPathAttr, p, static_cast<uint32>(strlen(p)));
    return sendMessage(message);
}

tresult RationsController::setIrFile(int slot, const char8 *path)
{
    if (slot < 0 || slot >= kIrSlotCount)
        return kInvalidArgument;
    // Sent BEFORE the row is updated, and the row is normally updated only if the processor
    // accepted it: a malformed or unreadable WAV is refused, and a row naming a file the audio
    // path does not have would be telling the user something untrue about what they hear.
    //
    // The exception is a host that has not connected the two halves. sendMessage answers
    // kResultFalse both for "the processor said no" and for "there was nobody to ask", and only
    // the first is a reason to leave the row empty; treating the second the same way would make
    // the browser look inert in a host whose connection is merely late. So the peer is checked
    // first, and with no peer the row is updated optimistically - which is the behaviour this
    // plug-in has always had, kept for exactly the case where nothing better is knowable.
    const tresult result = sendPath(kMsgLoadIr[slot], path);
    if (result != kResultOk && getPeer())
        return result;
    mIrPath[slot] = path ? path : "";
    if (mView)
        mView->FilesChanged();
    return result;
}

// Truncation is a failure rather than a silent short path: a caller that acted on half a path
// would open the wrong file.
tresult RationsController::copyPath(const std::string &src, char8 *buffer, int32 bufferSize)
{
    if (!buffer || bufferSize <= 0)
        return kInvalidArgument;
    if (src.size() + 1 > static_cast<size_t>(bufferSize)) {
        buffer[0] = 0;
        return kResultFalse;
    }
    memcpy(buffer, src.c_str(), src.size() + 1);
    return kResultOk;
}

tresult RationsController::getIrFile(int slot, char8 *buffer, int32 bufferSize) const
{
    if (slot < 0 || slot >= kIrSlotCount)
        return kInvalidArgument;
    return copyPath(mIrPath[slot], buffer, bufferSize);
}

//------------------------------------------------------------------------
// MIDI learn. Every one of these is a request, not a change: the table this controller holds is a
// copy for the editor to draw, and the processor's is the one a footswitch talks to. Editing this
// copy directly and hoping the two stay in step is exactly the class of bug the single-parameter
// channel decision exists to avoid, so it is not done here either.
void RationsController::sendMidiRow(const char *messageID, int row)
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(messageID);
    message->getAttributes()->setInt(kMidiRowAttr, row);
    sendMessage(message);
}

void RationsController::armMidiLearn(int row)
{
    if (row < -1 || row >= kMidiLearnRowCount)
        return;
    // Shown as armed straight away rather than waiting for the round trip, because the user is
    // about to stamp on a pedal and a button that takes a message round trip to light up reads as
    // one that did not register the click. The processor's reply corrects it either way.
    mArmedRow = row;
    sendMidiRow(kMsgMidiLearn, row);
    if (mView)
        mView->FilesChanged();
}

void RationsController::clearMidiLearn(int row)
{
    if (row < 0 || row >= kMidiLearnRowCount)
        return;
    sendMidiRow(kMsgMidiClear, row);
}

void RationsController::requestMidiTable()
{
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgRequestMidi);
    sendMessage(message);
}

const MidiBinding &RationsController::midiBinding(int row) const
{
    static const MidiBinding kUnlearned;
    if (row < 0 || row >= kMidiLearnRowCount)
        return kUnlearned;
    return mMidiTable[row];
}

// The processor's answer. Parsed defensively for the same reason the capability blob is: it is
// our own processor, but a short or absent attribute must leave a readable table rather than an
// out-of-range read, because the editor indexes into it every repaint.
tresult RationsController::receiveMidiTable(Vst::IMessage *message)
{
    Vst::IAttributeList *attrs = message ? message->getAttributes() : nullptr;
    if (!attrs)
        return kResultFalse;

    const void *data = nullptr;
    uint32 size = 0;
    if (attrs->getBinary(kMidiTableAttr, data, size) == kResultOk && data &&
        size == kMidiLearnRowCount * sizeof(std::uint32_t)) {
        std::uint32_t words[kMidiLearnRowCount] = {};
        memcpy(words, data, sizeof(words));
        for (int row = 0; row < kMidiLearnRowCount; ++row)
            mMidiTable[row] = unpackBinding(words[row]);
    }

    int64 armed = -1;
    if (attrs->getInt(kMidiArmedAttr, armed) != kResultOk)
        armed = -1;
    mArmedRow = (armed >= 0 && armed < kMidiLearnRowCount) ? static_cast<int>(armed) : -1;

    if (mView)
        mView->FilesChanged();
    return kResultOk;
}

} // namespace Rations
