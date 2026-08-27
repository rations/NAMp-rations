// Rations processor implementation. See the header for what this phase does and does not cover.

#include "rationsprocessor.h"
#include "platform/respath.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
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
}

//------------------------------------------------------------------------
RationsProcessor::~RationsProcessor()
{
    // The workers must be joined before anything they could still be writing into goes away.
    // Doing this only in terminate() is not enough: a host is free to destroy a component it
    // never initialised. The rack does the same in its own destructor; both are idempotent, and
    // ordering it here keeps the teardown readable in one place.
    mRack.stop();
    mRack.releaseBanks();
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::initialize(FUnknown *context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    addAudioInput(STR16("Input"), SpeakerArr::kMono);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    // Without this bus NO MIDI arrives at all - not the notes that come through inputEvents, and
    // not the CC and Program Change that come through the parameter queues either, because a host
    // that sees no event input has no reason to route MIDI here in the first place. One bus, all
    // sixteen channels, which is also what IUnitInfo::getUnitByBus is answering for on the
    // controller side.
    addEventInput(STR16("MIDI In"), 16);

    mRack.start();
    // The captures ship inside the bundle, so there is nothing for a user to load and no reason
    // to wait for one: all four banks are requested here and build on their own workers while the
    // host is still setting up. Four workers rather than one is what makes the build breadth-
    // first — every channel becomes switchable after roughly one model's build time instead of
    // after all thirty-five are done. A missing or unreadable directory is not fatal: that
    // channel outputs ramped silence, ModelBank prints one warning, and a switch to it is held.
    for (int c = 0; c < kChannelCount; ++c) {
        const Channel ch = static_cast<Channel>(c);
        mRack.loadChannel(ch, channelBankDir(ch), kSlimFixed, engine::kChunk);
    }

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
    // Join before releasing the banks: a load in flight during teardown is otherwise writing into
    // memory that is about to be freed.
    mRack.stop();
    mRack.releaseBanks();
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
    mRack.prepare(mResampler.maxNativeBlock(mMaxBlockSize), kNativeSampleRate);
    // Normalized, once, and never from a parameter: it is not a user choice in this plug-in.
    mRack.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);

    // Bypass ramp length in samples, at least one sample so the step is finite.
    const double rampSamples = std::max(1.0, engine::kBypassRampMs * 0.001 * mSampleRate);
    mBypassStep = 1.0 / rampSamples;

    mToneStack.Reset(mSampleRate, mMaxBlockSize);
    mNoiseGateTrigger.SetSampleRate(mSampleRate);

    // Note what is deliberately NOT here: rebuilding models. Every model runs at the native rate
    // in fixed chunks, so it is Reset for (48 kHz, kChunk) at build time and neither the host's
    // rate nor its block size can invalidate it. Only the IR, which is resampled when it is
    // loaded, depends on the host rate.
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        if (!mIrPath[slot].empty())
            loadIr(slot, mIrPath[slot]);
    }

    return kResultOk;
}

//------------------------------------------------------------------------
void RationsProcessor::allocateBuffers()
{
    const size_t n = static_cast<size_t>(mMaxBlockSize);
    mWorkBufInput.assign(n, 0.0);
    mWorkBufOutput.assign(n, 0.0);
    mDryBuf.assign(n, 0.0);
    mIrMixBuf.assign(n, 0.0);
    mWorkPtrInput = mWorkBufInput.data();
    mWorkPtrOutput = mWorkBufOutput.data();
    mIrMixPtr = mIrMixBuf.data();
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
        for (int slot = 0; slot < kIrSlotCount; ++slot)
            mRetiredIR[slot].reset();
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

        const ParamID id = queue->getParameterId();

        // The MIDI block. These parameters exist for no other purpose than to be the place a
        // footswitch's messages land, so they are decoded back into the message they came from
        // and handed to the learn table rather than being stored as values.
        if (id >= kMidiCcBaseId && id <= kMidiCcLastId) {
            const int cc = static_cast<int>(id - kMidiCcBaseId);
            const int ccValue = std::clamp(static_cast<int>(std::lround(value * 127.0)), 0, 127);
            const int last = mCcLast[cc];
            mCcLast[cc] = static_cast<std::uint8_t>(ccValue);

            // Matching fires on the PRESS: not on the release, and not once per block while a
            // foot rests on a latching pedal. 64 is the MIDI switch threshold, and the rising
            // edge is what makes a momentary pedal and a latching one behave the same way.
            //
            // LEARNING asks a different question, and gating it on the same edge is a bug that
            // only shows up in the most ordinary re-mapping there is: moving a pedal that is
            // already bound onto a different row. That pedal was last seen pressed, so its next
            // press is not an edge, so nothing is learned and the pedal reads as dead. While a
            // row is listening, any change is an answer - "which pedal is this" does not care
            // which way it moved - and so is a press, which is what covers a pedal that keeps
            // sending the same value. A host flushing initial zeroes at a parameter that was
            // already zero is neither, and does not teach anything.
            const bool learning = mMidiLearnRow.load(std::memory_order_relaxed) >= 0;
            const bool fire =
                learning ? (ccValue != last || ccValue >= 64) : (ccValue >= 64 && last < 64);
            if (fire)
                midiTrigger(MidiMsg::ControlChange, kMidiAnyChannel, cc);
            continue;
        }
        if (id == kMidiProgramChangeId) {
            // The parameter is the program list's own, so its value is the program number over
            // the list's step count - 127 steps for 128 programs. A Program Change has no
            // release, so there is no edge to look for.
            const int program =
                std::clamp(static_cast<int>(std::lround(value * (kMidiProgramCount - 1))), 0, 127);
            midiTrigger(MidiMsg::ProgramChange, kMidiAnyChannel, program);
            continue;
        }

        switch (id) {
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
// Note On, the one of the three message types that arrives as an actual MIDI event and the one
// that therefore still knows which MIDI channel it came from.
void RationsProcessor::handleInputEvents(IEventList *events)
{
    if (!events)
        return;
    const int32 count = events->getEventCount();
    for (int32 i = 0; i < count; ++i) {
        Event e = {};
        if (events->getEvent(i, e) != kResultOk)
            continue;
        if (e.type != Event::kNoteOnEvent)
            continue;
        // A note on with zero velocity is a note OFF - the oldest convention in MIDI, and one a
        // pedal is entitled to use. Acting on it would switch the channel again when the foot came
        // up, which is the same defect the CC edge test exists to prevent.
        if (e.noteOn.velocity <= 0.0f)
            continue;
        midiTrigger(MidiMsg::NoteOn, e.noteOn.channel, e.noteOn.pitch);
    }
}

//------------------------------------------------------------------------
// One decoded message. Audio thread: no allocation, no lock, no destructor, and no call into the
// controller - what the plug-in does to itself here is reported to the host through the output
// parameter queue like every other RT-to-outside message in this file.
void RationsProcessor::midiTrigger(MidiMsg msg, int channel, int data1)
{
    MidiBinding incoming;
    incoming.msg = msg;
    incoming.channel = channel;
    incoming.data1 = data1;
    const std::uint32_t word = packBinding(incoming);

    const int learning = mMidiLearnRow.load(std::memory_order_relaxed);
    if (learning >= 0 && learning < kMidiLearnRowCount) {
        // A button can only mean one thing. Teaching a message that some other row already
        // answers to takes it away from that row, rather than leaving two rows fighting over one
        // press and the winner decided by loop order.
        for (int r = 0; r < kMidiLearnRowCount; ++r)
            if (r != learning && mMidiBinding[r].load(std::memory_order_relaxed) == word)
                mMidiBinding[r].store(0, std::memory_order_relaxed);
        mMidiBinding[learning].store(word, std::memory_order_release);
        mMidiLearnRow.store(-1, std::memory_order_release);
        // The press that taught a row does not also perform it. Otherwise learning "OD2" would
        // switch to OD2 as a side effect of being taught, which is a channel change the user did
        // not ask for while their attention is on the pedal.
        return;
    }

    for (int r = 0; r < kMidiLearnRowCount; ++r) {
        const MidiBinding bound = unpackBinding(mMidiBinding[r].load(std::memory_order_acquire));
        if (!bindingMatches(bound, msg, channel, data1))
            continue;

        const MidiLearnTarget &target = kMidiLearnRows[r];
        if (target.param == kChannelId)
            mChannelNorm.store(target.value, std::memory_order_relaxed);
        // Remember to tell the host. A parameter the plug-in changed by itself and did not report
        // leaves the host's automation lane and the editor's copy disagreeing with the audio,
        // until the next thing that writes it snaps the channel back under the player's feet.
        if (mEchoCount < kMidiLearnRowCount) {
            mEcho[mEchoCount].id = target.param;
            mEcho[mEchoCount].value = target.value;
            ++mEchoCount;
        }
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::process(ProcessData &data)
{
    rations_set_denormal_mode();

    // Anything the MIDI table makes this plug-in do to itself is collected here and reported
    // before the first early return below, so a message that lands on a block with no audio in it
    // is not silently dropped.
    mEchoCount = 0;
    handleParameterChanges(data.inputParameterChanges);
    handleInputEvents(data.inputEvents);
    for (int i = 0; i < mEchoCount; ++i)
        writeOutputPoint(data.outputParameterChanges, mEcho[i].id, mEcho[i].value, 0);

    // Take delivery of newly published banks and hand the old ones back to their workers. Never a
    // delete here: a delete is a free(), which takes the allocator lock.
    mRack.pollBanks();

    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        if (mIRPending[slot].exchange(false, std::memory_order_acquire)) {
            mRetiredIR[slot] = std::move(mIR[slot]);
            mIR[slot] = std::move(mPendingIR[slot]);
        }
    }
    // The blend profile arrives on its own flag rather than with either slot, because it is a
    // property of the PAIR: loading B changes what the correct weights for A are.
    if (mBlendPending.exchange(false, std::memory_order_acquire))
        mBlend = mPendingBlend;

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

    // Where every dial is, before any audio is touched. All four are pushed, not just the
    // sounding one: an idle channel's dial decides which capture a switch to it would land on,
    // and which capture its worker should build first.
    for (int c = 0; c < kChannelCount; ++c)
        mRack.setPositionNorm(static_cast<Channel>(c),
                              mChannelGainNorm[c].load(std::memory_order_relaxed));
    // And which channel the host wants. The rack decides when the audio can follow.
    mRack.requestChannel(channelFromNorm(mChannelNorm.load(std::memory_order_relaxed)));

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
        writeOutputPoint(data.outputParameterChanges, kBankProgressId, mRack.progress(), 0);
        writeOutputPoint(data.outputParameterChanges, kActiveIndexId, mRack.activeIndexNorm(), 0);
        // Which channel is SOUNDING, which is not always the one kChannelId asks for: a switch
        // whose target capture is still being built is held, and the editor's LED must not light
        // over a channel that is not there yet. The parameter is the request; this is the answer.
        writeOutputPoint(data.outputParameterChanges, kActiveChannelId,
                         normFromChannel(mRack.soundingChannel()), 0);
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
                       reinterpret_cast<NAM_SAMPLE **>(&mWorkPtrOutput), numSamples, mRack);
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

    // 6. The cabinet: one IR, two blended, or none. The weights come from the correlation measured
    // between the two files at load time, so the middle of the dial neither bumps (two mic
    // positions on one cabinet) nor digs a hole (two different cabinets) - one sqrt per block, and
    // the endpoints exact by construction. See irblend.h.
    DSP_SAMPLE **irOutput = processCabinet(mIR[0].get(), mIR[1].get(), mBlend,
                                           mIrBlendNorm.load(std::memory_order_relaxed), tsOutput,
                                           static_cast<size_t>(numSamples), &mIrMixPtr);

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
bool RationsProcessor::loadIr(int slot, const std::string &path)
{
    if (slot < 0 || slot >= kIrSlotCount)
        return false;
    if (path.empty()) {
        mPendingIR[slot].reset();
        mIrProfile[slot].clear();
        remeasureBlend();
        mIRPending[slot].store(true, std::memory_order_release);
        return true;
    }
    try {
        auto ir = std::make_unique<dsp::ImpulseResponse>(path.c_str(), mSampleRate);
        if (ir->GetWavState() != dsp::wav::LoadReturnCode::SUCCESS)
            return false;
        mPendingIR[slot] = std::move(ir);
        profileIr(slot);
        // The blend goes out BEFORE the IR it belongs to, and the order is not cosmetic. These are
        // two separate publications, so the audio thread can run a block between them; if the IR
        // landed first, that block would mix two live IRs with weights measured when one of the
        // slots was still empty - a linear mix, which on two uncorrelated cabinets is a 3 dB dip
        // for one block, which is a click. Published this way round, the block in the middle sees
        // the new weights against the OLD pair, and the old pair has a slot empty, so the blend is
        // not consulted at all and nothing happens.
        remeasureBlend();
        mIRPending[slot].store(true, std::memory_order_release);
        return true;
    } catch (const std::exception &) {
        // An IR file is untrusted input: a malformed WAV must be a refused load, not an
        // exception escaping into the host's message loop.
        return false;
    }
}

//------------------------------------------------------------------------
// Message thread only, and deliberately so: this runs the IR twice over several thousand samples.
//
// Two jobs at once. It captures what the IR actually does — reading the private weight vector
// would miss the resampling to the host rate, the class's own gain and its 8192-sample
// truncation, and would get the time alignment wrong when the two files are different lengths,
// because the weights are applied oldest-first and so are stored reversed. Feeding an impulse and
// recording what comes out sidesteps all of that: the answer is the impulse response, aligned to
// the impulse, whatever the class did internally.
//
// And it warms the object. AudioDSPTools sizes its history and output buffers lazily on the first
// Process call, which for an IR loaded mid-session would otherwise be a malloc on the audio
// thread. Running it here means the first RT block finds everything already sized.
//
// The flush at the end matters: after the impulse the IR's history holds the impulse's tail, and
// handing that to the audio thread would splat it onto the first block. Feeding zeros until the
// whole history window is zero again leaves the object exactly as a freshly constructed one, so
// the warm-up is not audible.
void RationsProcessor::profileIr(int slot)
{
    mIrProfile[slot].clear();
    dsp::ImpulseResponse *ir = mPendingIR[slot].get();
    if (!ir)
        return;

    const size_t block = static_cast<size_t>(std::max<int32>(mMaxBlockSize, 1));
    const size_t want = static_cast<size_t>(kIrProfileSamples);

    // The stimulus is the -3 dB/octave weighting filter's own impulse response followed by
    // silence, so what comes back is the IR's response weighted the way musical signal energy
    // actually is. See irblend.h for why that matters more than it sounds like it should.
    std::vector<DSP_SAMPLE> stim(want + block, 0.0);
    fillIrProfileStimulus(stim.data(), want, mSampleRate);

    mIrProfile[slot].reserve(want);
    for (size_t pos = 0; mIrProfile[slot].size() < want; pos += block) {
        DSP_SAMPLE *stimPtr = stim.data() + pos;
        DSP_SAMPLE **out = ir->Process(&stimPtr, 1, block);
        const size_t take = std::min(block, want - mIrProfile[slot].size());
        mIrProfile[slot].insert(mIrProfile[slot].end(), out[0], out[0] + take);
    }

    // Flush the tail back out of the history so the audio thread gets a pristine object. The
    // history is at most the IR's own length, and the profile above is already longer than that,
    // so one more pass of the same length is always enough.
    std::fill(stim.begin(), stim.end(), 0.0);
    DSP_SAMPLE *silence = stim.data();
    for (size_t done = 0; done < want; done += block)
        ir->Process(&silence, 1, block);
}

//------------------------------------------------------------------------
// Message thread only. The blend weights depend on both IRs together, so this is recomputed
// whenever either slot changes, and published on its own flag.
void RationsProcessor::remeasureBlend()
{
    const size_t n = std::min(mIrProfile[0].size(), mIrProfile[1].size());
    mPendingBlend = measureIrBlend(mIrProfile[0].data(), mIrProfile[1].data(), n);
    mBlendPending.store(true, std::memory_order_release);
}

//------------------------------------------------------------------------
// Tell the controller what the four banks hold, so the editor can name the capture each dial is
// sitting on. Message thread only.
//
// The banks are built on worker threads, so the answer sent right after a load necessarily
// reports an empty set; the editor asks again (kMsgRequestCaps) until the real counts arrive.
// All four channels are always reported, an empty one included, because the wire format is
// positional: the receiving side splits on the separator and assigns names to channels by
// position, so a channel that sent nothing at all would shift every channel after it.
void RationsProcessor::sendModelCaps()
{
    IPtr<IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgModelCaps);
    IAttributeList *attrs = message->getAttributes();
    if (!attrs)
        return;

    std::string blob;
    for (int c = 0; c < kChannelCount; ++c) {
        const std::vector<std::string> names = mRack.captureNames(static_cast<Channel>(c));

        std::string attr(kCapsEntryCountAttr);
        attr += kChannelDirName[c];
        attrs->setInt(attr.c_str(), static_cast<int64>(names.size()));

        if (c > 0)
            blob.push_back('\f'); // channel separator
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
// The learn table, as the editor sees it. Sent in reply to a request and after every edit; the
// editor polls while a row is armed, because the moment a learn completes is on the audio thread
// and the audio thread cannot send a message.
void RationsProcessor::sendMidiTable()
{
    IPtr<IMessage> message = owned(allocateMessage());
    if (!message)
        return;
    message->setMessageID(kMsgMidiTable);
    IAttributeList *attrs = message->getAttributes();
    if (!attrs)
        return;

    std::uint32_t words[kMidiLearnRowCount] = {};
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        words[row] = mMidiBinding[row].load(std::memory_order_acquire);
    attrs->setBinary(kMidiTableAttr, words, static_cast<uint32>(sizeof(words)));
    attrs->setInt(kMidiArmedAttr, mMidiLearnRow.load(std::memory_order_acquire));
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

    // MIDI learn. All three are message-thread work on message-thread state, except that the
    // table itself is also read by the audio thread - which is why every write below is a single
    // atomic store of a packed word rather than an edit of a struct.
    if (id && strcmp(id, kMsgMidiLearn) == 0) {
        int64 row = -1;
        if (message->getAttributes()->getInt(kMidiRowAttr, row) != kResultOk)
            row = -1;
        const bool valid = row >= 0 && row < kMidiLearnRowCount;
        mMidiLearnRow.store(valid ? static_cast<int>(row) : -1, std::memory_order_release);
        sendMidiTable();
        return kResultOk;
    }
    if (id && strcmp(id, kMsgMidiClear) == 0) {
        int64 row = -1;
        if (message->getAttributes()->getInt(kMidiRowAttr, row) == kResultOk && row >= 0 &&
            row < kMidiLearnRowCount) {
            mMidiBinding[static_cast<int>(row)].store(0, std::memory_order_release);
            // Clearing the row that is listening also stops it listening: the user has just said
            // what they want that row to be, and it is nothing.
            int armed = static_cast<int>(row);
            mMidiLearnRow.compare_exchange_strong(armed, -1, std::memory_order_release,
                                                  std::memory_order_relaxed);
        }
        sendMidiTable();
        return kResultOk;
    }
    if (id && strcmp(id, kMsgRequestMidi) == 0) {
        sendMidiTable();
        return kResultOk;
    }

    int slot = -1;
    for (int i = 0; i < kIrSlotCount; ++i) {
        if (id && strcmp(id, kMsgLoadIr[i]) == 0)
            slot = i;
    }
    if (slot < 0)
        return AudioEffect::notify(message);

    const void *data = nullptr;
    uint32 size = 0;
    std::string path;
    if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data &&
        size > 0)
        path.assign(static_cast<const char *>(data), size);

    // A load is the message thread's chance to free whatever the audio thread retired earlier.
    for (int i = 0; i < kIrSlotCount; ++i)
        mRetiredIR[i].reset();

    const bool ok = loadIr(slot, path);
    if (ok)
        mIrPath[slot] = path;
    // No remeasure here: loadIr publishes the blend itself, in the order that keeps the two
    // publications safe to observe apart. A refused load changes neither slot, so there is
    // nothing to remeasure.
    return ok ? kResultOk : kResultFalse;
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
    if (!streamer.readInt32(version) || version < 1 || version > kStateVersion)
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
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        mIrPath[slot].clear();
        if (char8 *p = streamer.readStr8()) {
            mIrPath[slot] = p;
            delete[] p;
        }
    }

    // Recording the paths is not enough: they have to be LOADED. On a project open this is
    // redundant, because setupProcessing runs afterwards and loads them again at the host's real
    // sample rate — but a host is also free to push state at a plug-in that is already active, for
    // a preset change, and then setupProcessing never comes. Without this that preset would carry
    // over whatever cabinet the previous one had, or none.
    //
    // Message-thread work on the message thread: this is file I/O and it publishes to the audio
    // thread through exactly the same pending-flag handover a load from the editor uses.
    for (int slot = 0; slot < kIrSlotCount; ++slot) {
        if (!loadIr(slot, mIrPath[slot]))
            mIrPath[slot].clear(); // a path that no longer resolves is an empty slot, not a lie
    }

    // The MIDI learn table, added in state version 2. A version 1 blob has nothing here and that
    // is not a failure - it is a project saved before the pedal could do anything - so it opens
    // with an unlearned table rather than being rejected.
    //
    // Every word goes through unpackBinding, which is where an out-of-range message type or
    // channel from an untrusted blob becomes an unlearned row instead of a row that answers to
    // something nobody can name. Learning is disarmed either way: a project cannot open with the
    // plug-in already listening for a pedal the user has not asked it to listen for.
    mMidiLearnRow.store(-1, std::memory_order_release);
    for (int row = 0; row < kMidiLearnRowCount; ++row) {
        std::uint32_t word = 0;
        if (version >= 2) {
            int32 raw = 0;
            if (!streamer.readInt32(raw))
                return kResultFalse;
            word = packBinding(unpackBinding(static_cast<std::uint32_t>(raw)));
        }
        mMidiBinding[row].store(word, std::memory_order_release);
    }

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::getState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    streamer.writeInt32(kStateVersion);

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

    for (int slot = 0; slot < kIrSlotCount; ++slot)
        streamer.writeStr8(mIrPath[slot].c_str());

    // Version 2 onwards: the MIDI learn table, one packed word per row. Which row is currently
    // ARMED is deliberately not written - it is a transient state of the editor's, not a property
    // of the session, and a project that reopened still listening for a pedal would learn
    // whatever the player happened to press next.
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        streamer.writeInt32(static_cast<int32>(mMidiBinding[row].load(std::memory_order_acquire)));
    return kResultOk;
}

} // namespace Rations
