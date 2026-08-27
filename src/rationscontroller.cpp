// Rations edit controller implementation.

#include "rationscontroller.h"
#include "rationsids.h"
#include "rationsview.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"

#include <cstring>

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::initialize(FUnknown *context)
{
    tresult result = EditController::initialize(context);
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

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsController::setComponentState(IBStream *state)
{
    // Mirror of RationsProcessor::getState — keep the two in sync.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 1)
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

    // The two IR paths follow. They are not parameters, so nothing is set from them here; the
    // editor reads them from the processor when it needs them.
    return kResultOk;
}

//------------------------------------------------------------------------
// The processor reporting what the four banks hold. The message comes from our own processor, but
// it is still parsed defensively: a truncated or absent blob must leave empty lists rather than an
// out-of-range read, because the editor indexes into them.
tresult PLUGIN_API RationsController::notify(Vst::IMessage *message)
{
    const char *id = message ? message->getMessageID() : nullptr;
    if (!id || strcmp(id, kMsgModelCaps) != 0)
        return EditController::notify(message);

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
    mIrPath[slot] = path ? path : "";
    if (mView)
        mView->FilesChanged();
    return sendPath(slot == 0 ? kMsgLoadIrA : kMsgLoadIrB, path);
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

} // namespace Rations
