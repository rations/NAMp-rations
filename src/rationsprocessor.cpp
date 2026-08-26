// Rations processor implementation. See the header for what this phase does and does not cover.

#include "rationsprocessor.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Rations
{

//------------------------------------------------------------------------
Channel channelFromNorm(double norm)
{
    const double steps = static_cast<double>(kChannelCount - 1);
    int i = static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * steps));
    i = std::clamp(i, 0, kChannelCount - 1);
    return static_cast<Channel>(i);
}

//------------------------------------------------------------------------
double normFromChannel(Channel ch)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    return static_cast<double>(i) / static_cast<double>(kChannelCount - 1);
}

//------------------------------------------------------------------------
RationsProcessor::RationsProcessor()
{
    setControllerClass(RationsControllerUID);
}

//------------------------------------------------------------------------
RationsProcessor::~RationsProcessor() = default;

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::initialize(FUnknown *context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    addAudioInput(STR16("Input"), SpeakerArr::kMono);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::terminate()
{
    return AudioEffect::terminate();
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::setBusArrangements(SpeakerArrangement *inputs, int32 numIns,
                                                        SpeakerArrangement *outputs, int32 numOuts)
{
    // Accepted layouts: mono or stereo in (channel 0 is used), mono or stereo out (the mono
    // result is copied to every output channel).
    if (numIns != 1 || numOuts != 1)
        return kResultFalse;
    if (inputs[0] != SpeakerArr::kMono && inputs[0] != SpeakerArr::kStereo)
        return kResultFalse;
    if (outputs[0] != SpeakerArr::kMono && outputs[0] != SpeakerArr::kStereo)
        return kResultFalse;
    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::setupProcessing(ProcessSetup &setup)
{
    tresult result = AudioEffect::setupProcessing(setup);
    if (result != kResultOk)
        return result;

    mSampleRate = setup.sampleRate;
    mMaxBlockSize = setup.maxSamplesPerBlock;
    return kResultOk;
}

//------------------------------------------------------------------------
void RationsProcessor::handleParameterChanges(IParameterChanges *changes)
{
    if (!changes)
        return;

    const int32 count = changes->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        IParamValueQueue *queue = changes->getParameterData(i);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points <= 0)
            continue;

        // Sample-accurate automation is not modelled: every parameter here is a control, not a
        // signal, so the value at the end of the block is the one that matters.
        int32 offset = 0;
        ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;

        switch (queue->getParameterId()) {
            case kBypassId:
                mBypass.store(value, std::memory_order_relaxed);
                break;
            case kInputGainId:
                mInputGainNorm.store(value, std::memory_order_relaxed);
                break;
            case kOutputGainId:
                mOutputGainNorm.store(value, std::memory_order_relaxed);
                break;
            case kNoiseGateThresholdId:
                mNgThresholdNorm.store(value, std::memory_order_relaxed);
                break;
            case kBassId:
                mBassNorm.store(value, std::memory_order_relaxed);
                break;
            case kMiddleId:
                mMiddleNorm.store(value, std::memory_order_relaxed);
                break;
            case kTrebleId:
                mTrebleNorm.store(value, std::memory_order_relaxed);
                break;
            case kNoiseGateOnId:
                mNoiseGateOn.store(value, std::memory_order_relaxed);
                break;
            case kChannelId:
                mChannelNorm.store(value, std::memory_order_relaxed);
                break;
            case kCleanGainId:
            case kCrunchGainId:
            case kOd1GainId:
            case kOd2GainId: {
                const ParamID id = queue->getParameterId();
                for (int c = 0; c < kChannelCount; ++c)
                    if (kChannelGainId[c] == id)
                        mChannelGainNorm[c].store(value, std::memory_order_relaxed);
                break;
            }
            case kIrBlendId:
                mIrBlendNorm.store(value, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::process(ProcessData &data)
{
    handleParameterChanges(data.inputParameterChanges);

    if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0)
        return kResultOk;

    const int32 numSamples = data.numSamples;
    const float *in = data.inputs[0].channelBuffers32[0];
    AudioBusBuffers &outBus = data.outputs[0];

    if (!in) {
        for (int32 ch = 0; ch < outBus.numChannels; ++ch)
            if (float *out = outBus.channelBuffers32[ch])
                std::memset(out, 0, static_cast<size_t>(numSamples) * sizeof(float));
        outBus.silenceFlags = (static_cast<uint64>(1) << outBus.numChannels) - 1;
        return kResultOk;
    }

    // Phase 0: pass the input through to every output channel. No allocation, no locks, no
    // destructors — the contract this file lives under from here on.
    for (int32 ch = 0; ch < outBus.numChannels; ++ch)
        if (float *out = outBus.channelBuffers32[ch])
            std::memcpy(out, in, static_cast<size_t>(numSamples) * sizeof(float));
    outBus.silenceFlags = 0;

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::setState(IBStream *state)
{
    // Untrusted input: a project file can hold anything. Every read is checked and a malformed
    // blob returns kResultFalse rather than leaving the processor half-loaded.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 1)
        return kResultFalse;

    double values[8] = {0};
    for (double &v : values)
        if (!streamer.readDouble(v))
            return kResultFalse;

    mBypass.store(values[0], std::memory_order_relaxed);
    mInputGainNorm.store(values[1], std::memory_order_relaxed);
    mOutputGainNorm.store(values[2], std::memory_order_relaxed);
    mNgThresholdNorm.store(values[3], std::memory_order_relaxed);
    mBassNorm.store(values[4], std::memory_order_relaxed);
    mMiddleNorm.store(values[5], std::memory_order_relaxed);
    mTrebleNorm.store(values[6], std::memory_order_relaxed);
    mNoiseGateOn.store(values[7], std::memory_order_relaxed);

    double channel = 0.0;
    if (!streamer.readDouble(channel))
        return kResultFalse;
    // Snapped through the decoder rather than stored raw: a blob written by a future version with
    // more channels, or simply a corrupt one, must not leave an out-of-range channel selected.
    mChannelNorm.store(normFromChannel(channelFromNorm(channel)), std::memory_order_relaxed);

    for (int c = 0; c < kChannelCount; ++c) {
        double g = 0.0;
        if (!streamer.readDouble(g))
            return kResultFalse;
        mChannelGainNorm[c].store(std::clamp(g, 0.0, 1.0), std::memory_order_relaxed);
    }

    double blend = 0.0;
    if (!streamer.readDouble(blend))
        return kResultFalse;
    mIrBlendNorm.store(std::clamp(blend, 0.0, 1.0), std::memory_order_relaxed);

    // IR paths (written with writeStr8: int32 length + bytes). A missing entry leaves the slot
    // empty rather than failing the load.
    mIrPathA.clear();
    mIrPathB.clear();
    if (char8 *p = streamer.readStr8()) {
        mIrPathA = p;
        delete[] p;
    }
    if (char8 *p = streamer.readStr8()) {
        mIrPathB = p;
        delete[] p;
    }

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::getState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    // State version 1.
    streamer.writeInt32(1);

    streamer.writeDouble(mBypass.load(std::memory_order_relaxed));
    streamer.writeDouble(mInputGainNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mOutputGainNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mNgThresholdNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mBassNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mMiddleNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mTrebleNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mNoiseGateOn.load(std::memory_order_relaxed));

    streamer.writeDouble(mChannelNorm.load(std::memory_order_relaxed));
    for (int c = 0; c < kChannelCount; ++c)
        streamer.writeDouble(mChannelGainNorm[c].load(std::memory_order_relaxed));
    streamer.writeDouble(mIrBlendNorm.load(std::memory_order_relaxed));

    streamer.writeStr8(mIrPathA.c_str());
    streamer.writeStr8(mIrPathB.c_str());
    return kResultOk;
}

} // namespace Rations
