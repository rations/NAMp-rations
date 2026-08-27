// Rations processor implementation. See the header for what this phase does and does not cover.

#include "rationsprocessor.h"
#include "platform/respath.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

// Flush-to-zero / denormals-are-zero, re-armed on every process() call: a host is not required to
// set FTZ/DAZ on its audio threads, and subnormals in the model and filter paths would stall the
// CPU and blow the real-time deadline.
#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define RATIONS_HAVE_SSE_DENORMAL 1
#endif

static inline void rations_set_denormal_mode(void)
{
#ifdef RATIONS_HAVE_SSE_DENORMAL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Rations
{

namespace
{

inline double denorm(double norm, double min, double max)
{
    return min + norm * (max - min);
}

inline double dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

// Map a linear peak to the meter's normalized 0 .. 1 display range.
inline double peakToMeterNorm(double peak)
{
    const double db = 20.0 * std::log10(std::max(peak, 1e-7));
    const double norm = (db - ranges::kMeterMinDb) / (ranges::kMeterMaxDb - ranges::kMeterMinDb);
    return norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
}

// Write one point to an output parameter queue. RT-safe under the SDK host implementation:
// ParameterChanges is pre-sized by the host and every queue pre-reserves points at construction —
// the point-count guard keeps us inside that reserve so addPoint never grows the vector on the RT
// thread.
inline void writeOutputPoint(IParameterChanges *outChanges, ParamID id, ParamValue value,
                             int32 sampleOffset)
{
    if (!outChanges)
        return;
    int32 queueIndex = 0;
    IParamValueQueue *queue = outChanges->addParameterData(id, queueIndex);
    if (!queue || queue->getPointCount() >= 4)
        return;
    int32 pointIndex = 0;
    queue->addPoint(sampleOffset, value, pointIndex);
}

// Output is pinned to Normalized: per-capture loudness compensation on, and no calibration.
// Adjacent captures of one amp differ in measured loudness by more than a decibel and not
// monotonically, so this is what stops the level stepping at every crossing — it is applied per
// branch INSIDE the crossfade, before the mix, which is why the engine owns it rather than the
// output stage. The calibration argument is unused at this mode and is passed as the engine's
// own default.
constexpr int kOutputModeNormalized = 1;
constexpr double kUnusedCalLevelDbu = 12.0;

// Slim is fixed at full size, permanently and by decision — not deferred. These captures are
// slimmable containers and a smaller variant would genuinely cost less CPU, but this plug-in
// always plays them whole: an amp head that quietly swaps in a lesser model of itself is not
// what is being built.
//
// Carried as a named constant through the loader's `slim` argument rather than hard-coded 1.0 at
// each call site, because that keeps the one place the value is decided visible, and keeps the
// ported loader identical to the parent's instead of forking it to delete a parameter.
constexpr double kSlimFixed = 1.0;

} // namespace

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
    // The trigger detects level on the model INPUT and drives the gain stage that attenuates the
    // model OUTPUT, which is why the two sit on opposite sides of the engine in the chain.
    mNoiseGateTrigger.AddListener(&mNoiseGateGain);
    mEngine.setLoader(&mBankLoader);
}

//------------------------------------------------------------------------
RationsProcessor::~RationsProcessor()
{
    // The worker must be joined before anything it could still be writing into goes away. Doing
    // this only in terminate() is not enough: a host is free to destroy a component it never
    // initialised.
    mBankLoader.stop();
    ModelBank::destroyBank(mEngine.releaseBank());
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::initialize(FUnknown *context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    addAudioInput(STR16("Input"), SpeakerArr::kMono);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

    mBankLoader.start();
    // The captures ship inside the bundle, so there is nothing for a user to load and no reason
    // to wait for one: the bank is requested here and builds on the worker while the host is
    // still setting up. A missing or unreadable directory is not fatal — the engine outputs
    // ramped silence and ModelBank prints one warning.
    mBankLoader.loadDirectory(channelBankDir(kChannelClean), kSlimFixed, engine::kChunk);

    return kResultOk;
}

//------------------------------------------------------------------------
// The bundled bank directory for one channel. Resolved through the same resource lookup that
// finds img/ and fonts/, so a development override ($RATIONS_RESOURCE_DIR) moves all three
// together and there is one answer to "where does this build read its files from".
std::string RationsProcessor::channelBankDir(Channel ch)
{
    const int i = std::clamp(static_cast<int>(ch), 0, kChannelCount - 1);
    std::filesystem::path dir(resourceDir());
    dir /= "captures";
    dir /= kChannelDirName[i];
    return dir.string();
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::terminate()
{
    // Join before releasing the bank: a load in flight during teardown is otherwise writing into
    // memory that is about to be freed.
    mBankLoader.stop();
    ModelBank::destroyBank(mEngine.releaseBank());
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

    allocateBuffers();

    mResampler.configure(mSampleRate, mMaxBlockSize);
    mLatency.store(static_cast<uint32>(mResampler.latency()), std::memory_order_relaxed);
    mEngine.prepare(mResampler.maxNativeBlock(mMaxBlockSize), kNativeSampleRate);
    // Normalized, once, and never from a parameter: it is not a user choice in this plug-in.
    mEngine.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);

    // Bypass ramp length in samples, at least one sample so the step is finite.
    const double rampSamples = std::max(1.0, engine::kBypassRampMs * 0.001 * mSampleRate);
    mBypassStep = 1.0 / rampSamples;

    mToneStack.Reset(mSampleRate, mMaxBlockSize);
    mNoiseGateTrigger.SetSampleRate(mSampleRate);

    // Note what is deliberately NOT here: rebuilding models. Every model runs at the native rate
    // in fixed chunks, so it is Reset for (48 kHz, kChunk) at build time and neither the host's
    // rate nor its block size can invalidate it. Only the IR, which is resampled when it is
    // loaded, depends on the host rate.
    if (!mIrPathA.empty())
        loadIr(mIrPathA);

    return kResultOk;
}

//------------------------------------------------------------------------
void RationsProcessor::allocateBuffers()
{
    const size_t n = static_cast<size_t>(mMaxBlockSize);
    mWorkBufInput.assign(n, 0.0);
    mWorkBufOutput.assign(n, 0.0);
    mDryBuf.assign(n, 0.0);
    mWorkPtrInput = mWorkBufInput.data();
    mWorkPtrOutput = mWorkBufOutput.data();
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::setActive(TBool state)
{
    if (state) {
        // Pre-run the gate and tone stack once at the maximum block size so their internal
        // buffers are sized before the first RT block: AudioDSPTools allocates them lazily, and
        // the first allocation would otherwise land on the audio thread.
        const size_t n = static_cast<size_t>(mMaxBlockSize);
        std::fill(mWorkBufInput.begin(), mWorkBufInput.end(), 0.0);
        DSP_SAMPLE **warm = &mWorkPtrInput;
        warm = mNoiseGateTrigger.Process(warm, 1, n);
        warm = mNoiseGateGain.Process(warm, 1, n);
        mToneStack.Process(warm, 1, static_cast<int>(n));
        // Come up dry and ramp in, rather than opening on a half-built bank.
        mBypassMix = 1.0;
    } else {
        // Anything the audio thread retired is unreachable from it now; free it here.
        mRetiredIR.reset();
    }
    return AudioEffect::setActive(state);
}

//------------------------------------------------------------------------
uint32 PLUGIN_API RationsProcessor::getLatencySamples()
{
    return mLatency.load(std::memory_order_relaxed);
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
    rations_set_denormal_mode();

    handleParameterChanges(data.inputParameterChanges);

    // Take delivery of a newly published bank and hand the old one back to the worker. Never a
    // delete here: a delete is a free(), which takes the allocator lock.
    mEngine.pollBank();

    if (mIRPending.exchange(false, std::memory_order_acquire)) {
        mRetiredIR = std::move(mIR);
        mIR = std::move(mPendingIR);
    }

    if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0)
        return kResultOk;
    if (!data.inputs[0].channelBuffers32 || !data.outputs[0].channelBuffers32)
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
    float *out = outBus.channelBuffers32[0];
    if (!out)
        return kResultOk;

    // Where the dial is, before any audio is touched. Only the Clean dial reaches the engine in
    // this phase; the other three move their own parameters and nothing else, and the channel
    // rack is what gives them a bank each.
    mEngine.setPositionNorm(mChannelGainNorm[kChannelClean].load(std::memory_order_relaxed));

    // The host may hand us more than it promised in setupProcessing. Loop in whole sub-blocks
    // rather than clamping: clamping leaves the tail of the output buffer holding whatever was
    // there before, which is a stale-audio artefact, not a dropout.
    for (int32 done = 0; done < numSamples; done += mMaxBlockSize) {
        const int32 n = std::min(mMaxBlockSize, numSamples - done);
        applyDsp(in + done, out + done, n);
    }

    // Mono result -> every remaining output channel.
    for (int32 ch = 1; ch < outBus.numChannels; ++ch) {
        float *dst = outBus.channelBuffers32[ch];
        if (dst && dst != out)
            std::memcpy(dst, out, static_cast<size_t>(numSamples) * sizeof(float));
    }
    outBus.silenceFlags = 0;

    // Feedback to the editor, all through the output parameter queue — never a direct call into
    // the controller, which is a UI-thread object.
    if (data.outputParameterChanges) {
        double inPeak = 0.0, outPeak = 0.0;
        for (int32 i = 0; i < numSamples; ++i) {
            inPeak = std::max(inPeak, std::fabs(static_cast<double>(in[i])));
            outPeak = std::max(outPeak, std::fabs(static_cast<double>(out[i])));
        }
        writeOutputPoint(data.outputParameterChanges, kInputMeterId, peakToMeterNorm(inPeak), 0);
        writeOutputPoint(data.outputParameterChanges, kOutputMeterId, peakToMeterNorm(outPeak), 0);
        writeOutputPoint(data.outputParameterChanges, kBankProgressId, mBankLoader.progress(), 0);
        writeOutputPoint(data.outputParameterChanges, kActiveIndexId, mEngine.activeIndexNorm(), 0);
    }

    return kResultOk;
}

//------------------------------------------------------------------------
// One sub-block of the chain. Everything here is on the audio thread: no allocation, no locks, no
// file I/O, no logging, no destructors.
void RationsProcessor::applyDsp(const float *in, float *out, int32 numSamples)
{
    const double inputGain = dbToLinear(
        denorm(mInputGainNorm.load(std::memory_order_relaxed), ranges::kGainMin, ranges::kGainMax));
    const double outputGain = dbToLinear(denorm(mOutputGainNorm.load(std::memory_order_relaxed),
                                                ranges::kGainMin, ranges::kGainMax));
    const bool ngOn = mNoiseGateOn.load(std::memory_order_relaxed) > 0.5;

    // 1. float -> double: the dry copy for the bypass ramp, and the gained model input.
    for (int32 i = 0; i < numSamples; ++i) {
        const DSP_SAMPLE x = static_cast<DSP_SAMPLE>(in[i]);
        mDryBuf[static_cast<size_t>(i)] = x;
        mWorkBufInput[static_cast<size_t>(i)] = x * inputGain;
    }

    // 2. Noise-gate trigger (level detection; returns the gated signal).
    DSP_SAMPLE **processingInput = &mWorkPtrInput;
    if (ngOn) {
        const double ngThreshDb = denorm(mNgThresholdNorm.load(std::memory_order_relaxed),
                                         ranges::kNgMin, ranges::kNgMax);
        const dsp::noise_gate::TriggerParams triggerParams(0.01, ngThreshDb, 0.1, 0.005, 0.01,
                                                           0.05);
        mNoiseGateTrigger.SetParams(triggerParams);
        processingInput =
            mNoiseGateTrigger.Process(&mWorkPtrInput, 1, static_cast<size_t>(numSamples));
    }

    // 3. The crossfade engine, at the native rate, in fixed chunks. The resampler is a straight
    // call-through at 48 kHz and is not even constructed there.
    mResampler.process(reinterpret_cast<NAM_SAMPLE **>(processingInput),
                       reinterpret_cast<NAM_SAMPLE **>(&mWorkPtrOutput), numSamples, mEngine);
    DSP_SAMPLE **modelOutput = &mWorkPtrOutput;

    // 4. Noise-gate gain (applies the envelope to the model output).
    DSP_SAMPLE **gateOutput = modelOutput;
    if (ngOn)
        gateOutput = mNoiseGateGain.Process(modelOutput, 1, static_cast<size_t>(numSamples));

    // 5. Tone stack. Always on — unlike the parent plug-in there is no bypass for it, because an
    // amp head's tone controls are not a stage you switch out.
    mToneStack.SetParam("bass", denorm(mBassNorm.load(std::memory_order_relaxed), ranges::kToneMin,
                                       ranges::kToneMax));
    mToneStack.SetParam("middle", denorm(mMiddleNorm.load(std::memory_order_relaxed),
                                         ranges::kToneMin, ranges::kToneMax));
    mToneStack.SetParam("treble", denorm(mTrebleNorm.load(std::memory_order_relaxed),
                                         ranges::kToneMin, ranges::kToneMax));
    DSP_SAMPLE **tsOutput = mToneStack.Process(gateOutput, 1, numSamples);

    // 6. IR convolution. One slot in this phase; the second slot and the blend between them
    // arrive with the cabinet page's DSP.
    DSP_SAMPLE **irOutput = tsOutput;
    if (mIR)
        irOutput = mIR->Process(tsOutput, 1, static_cast<size_t>(numSamples));

    // 7. double -> float with output gain and the ramped bypass mix. Per-capture loudness
    // compensation is NOT applied here: it is per-capture and has to happen inside the crossfade,
    // before the two branches are mixed.
    const DSP_SAMPLE *finalBuf = irOutput[0];

    // Bypass is a per-sample ramp, never a switch: a hard mute or a hard hand-off to the dry
    // signal is itself a click, and it also exposes models that have been fed nothing while
    // bypassed. The chain above runs whether bypassed or not, so the bank stays primed and
    // un-bypassing is immediately correct. That costs CPU while bypassed, deliberately.
    const double target = mBypass.load(std::memory_order_relaxed) > 0.5 ? 1.0 : 0.0;
    double mix = mBypassMix;
    for (int32 i = 0; i < numSamples; ++i) {
        if (mix < target)
            mix = std::min(target, mix + mBypassStep);
        else if (mix > target)
            mix = std::max(target, mix - mBypassStep);
        const double wet = finalBuf[i] * outputGain;
        out[i] = static_cast<float>(wet + mix * (mDryBuf[static_cast<size_t>(i)] - wet));
    }
    mBypassMix = mix;
}

//------------------------------------------------------------------------
// Message thread only. An empty path clears the slot. The new IR is published to the audio thread
// through mIRPending; the audio thread hands the old one back by moving it into mRetiredIR, which
// is freed here on the next load or in setActive(false) — the audio thread never destroys one.
bool RationsProcessor::loadIr(const std::string &path)
{
    if (path.empty()) {
        mPendingIR.reset();
        mIRPending.store(true, std::memory_order_release);
        return true;
    }
    try {
        auto ir = std::make_unique<dsp::ImpulseResponse>(path.c_str(), mSampleRate);
        if (ir->GetWavState() != dsp::wav::LoadReturnCode::SUCCESS)
            return false;
        mPendingIR = std::move(ir);
        mIRPending.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception &) {
        // An IR file is untrusted input: a malformed WAV must be a refused load, not an
        // exception escaping into the host's message loop.
        return false;
    }
}

//------------------------------------------------------------------------
// Tell the controller what the four banks hold, so the editor can name the capture each dial is
// sitting on. Message thread only.
//
// The banks are built on worker threads, so the answer sent right after a load necessarily
// reports an empty set; the editor asks again (kMsgRequestCaps) until the real counts arrive.
// Only the Clean bank exists in this phase, but all four channels are reported — including the
// three empty ones — because the wire format is positional: the receiving side splits on the
// separator and assigns names to channels by position, so a channel that sends nothing at all
// would shift every channel after it.
void RationsProcessor::sendModelCaps()
{
    IPtr<IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgModelCaps);
    IAttributeList *attrs = message->getAttributes();
    if (!attrs)
        return;

    const std::vector<std::string> names = mBankLoader.captureNames();

    std::string blob;
    for (int c = 0; c < kChannelCount; ++c) {
        const bool wired = (c == kChannelClean);
        std::string attr(kCapsEntryCountAttr);
        attr += kChannelDirName[c];
        attrs->setInt(attr.c_str(), wired ? static_cast<int64>(names.size()) : 0);

        if (c > 0)
            blob.push_back('\f'); // channel separator
        if (!wired)
            continue;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0)
                blob.push_back('\n'); // capture separator
            blob += names[i];
        }
    }

    attrs->setBinary(kCapsNamesAttr, blob.data(), static_cast<uint32>(blob.size()));
    sendMessage(message);
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::notify(IMessage *message)
{
    if (!message)
        return kInvalidArgument;

    const char *id = message->getMessageID();

    // The editor asking what the banks turned out to hold. The scan and the builds run on the
    // worker, so the caps sent when the plug-in was created could not know the counts yet.
    if (id && strcmp(id, kMsgRequestCaps) == 0) {
        sendModelCaps();
        return kResultOk;
    }

    const bool isIrA = id && strcmp(id, kMsgLoadIrA) == 0;
    const bool isIrB = id && strcmp(id, kMsgLoadIrB) == 0;
    if (!isIrA && !isIrB)
        return AudioEffect::notify(message);

    const void *data = nullptr;
    uint32 size = 0;
    std::string path;
    if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data &&
        size > 0)
        path.assign(static_cast<const char *>(data), size);

    // A load is the message thread's chance to free whatever the audio thread retired earlier.
    mRetiredIR.reset();

    if (isIrB) {
        // Recorded so it survives a save and is there for the blend when that stage exists, but
        // it reaches no audio yet. Accepting the path and silently doing nothing with it would be
        // worse than refusing it, so the editor is told plainly that it did not take effect.
        mIrPathB = path;
        return kResultFalse;
    }

    mIrPathA = path;
    return loadIr(path) ? kResultOk : kResultFalse;
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
