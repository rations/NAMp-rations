// Rations edit controller implementation.

#include "rationscontroller.h"
#include "rationsids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"

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

} // namespace Rations
