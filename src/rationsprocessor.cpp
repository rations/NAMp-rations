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
RationsProcessor::RationsProcessor()
{
    // Every pedal control at the default kPedalParams gives it, normalized. The footswitches are
    // all off: a fresh instance is an amp with an empty board, and a project written before the
    // pedalboard existed must open sounding exactly as it did.
    for (int i = 0; i < kPedalParamCount; ++i)
        mPedalNorm[i].store(pedalNorm(kPedalParams[i], kPedalParams[i].def),
                            std::memory_order_relaxed);

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
    // Nothing is loaded here, and that is the change this build is about. The plug-in used to ship
    // four banks inside its own bundle and request all four at this point; it ships none now, so a
    // fresh instance comes up with four empty channels and the settings page asking for them. Empty
    // is a state the rack already handles correctly - it answers with ramped silence, never with
    // dry signal, which would jump in level the instant a model landed - so there is nothing to add
    // here beyond not doing it.
    //
    // The four workers still exist and still start now. What makes the build breadth-first is one
    // worker per channel, and that is a property of the rack rather than of when the loads arrive:
    // four folders picked one after another still build in parallel.
    return kResultOk;
}

//------------------------------------------------------------------------
// Load one channel's captures. Message thread: this reaches the filesystem, and ModelBank does the
// rest of it on that channel's own worker.
//
// A directory and a single file are both ordinary answers and neither is a special case below this
// line: a lone capture is a bank of one, which the engine plays without knowing the difference. The
// only thing that changes is what the dial has to sweep, and the editor is what says so.
void RationsProcessor::loadCaptureSource(int channel, const std::string &path, bool isDirectory)
{
    const int c = std::clamp(channel, 0, kChannelCount - 1);
    mCapturePath[c] = path;
    // A cleared channel is not a directory, whatever the message said. Recording it as one would
    // make the next state blob claim a folder that is not there.
    mCaptureIsDir[c] = !path.empty() && isDirectory;
    mRack.loadChannel(static_cast<Channel>(c), path, mCaptureIsDir[c], kSlimFixed, engine::kChunk);
    // Say the output section again. A load republishes the bank, and a republished bank is exactly
    // the case where an engine's compensation would otherwise be carrying the last bank's metadata.
    publishOutputMode();
}

//------------------------------------------------------------------------
// Publish the output section to the rack. Callable from either thread, and it has to be: a user
// clicking a radio on the settings page reaches this through a host parameter queue on the AUDIO
// thread, while a load or a state restore reaches it on the message thread.
//
// That is affordable only because everything below is a store. The rack publishes and each engine
// applies on its own thread, exactly as the dial positions already do - see setPositionNorm.
void RationsProcessor::publishOutputMode()
{
    const OutputMode mode = outputModeFromNorm(mOutputModeNorm.load(std::memory_order_relaxed));
    const double calLevelDbu =
        denorm(mCalLevelNorm.load(std::memory_order_relaxed), ranges::kCalMin, ranges::kCalMax);
    const bool calibrate = mCalibrateInput.load(std::memory_order_relaxed) > 0.5;
    mRack.setOutputMode(static_cast<int>(mode), calLevelDbu, calibrate);

    // Input calibration, and this is the one place this plug-in cannot copy its parent. NAMp folds
    // the offset into the global input gain, which it can do because it has one bank and therefore
    // one input_level_dbu. Four channels may state four different levels, and the input the models
    // see IS the ring - the thing the switch replays and the convergence proof is a proof about. A
    // single global offset would prime every idle channel at a level that is wrong for it, and
    // would step at the instant the sounding channel changes, in the middle of the fade the switch
    // exists to make inaudible. So it goes per channel, on that engine's model input, inside the
    // rack: the mirror of the per-channel trim, which is per channel on the output for the same
    // reason.
    // Note what this does NOT do: work out each channel's offset here and push four numbers. That
    // would mean reading four banks' level metadata from this thread, and three of those banks
    // belong to the prime worker. The decision goes down with the mode instead, and each engine
    // resolves it against its own bound captures on its own thread - which is also why the offset
    // is per BRANCH rather than per channel, so a sweep across two captures that state different
    // input levels crossfades between them instead of stepping.
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
    // The pedalboard's contribution is UNCONDITIONAL - see PedalChain::latencySamples. It is added
    // here, once, and never changes again for the life of this setup.
    mLatency.store(static_cast<uint32>(mResampler.latency() + pedals::PedalChain::latencySamples()),
                   std::memory_order_relaxed);
    mRack.prepare(mResampler.maxNativeBlock(mMaxBlockSize), kNativeSampleRate);
    // prepare() resets the rack's published level state along with everything else, so the output
    // section has to be said again here rather than assumed to have survived.
    publishOutputMode();

    // Bypass ramp length in samples, at least one sample so the step is finite.
    const double rampSamples = std::max(1.0, engine::kBypassRampMs * 0.001 * mSampleRate);
    mBypassStep = 1.0 / rampSamples;

    mToneStack.Reset(mSampleRate, mMaxBlockSize);
    // The pedals, at the HOST rate: PRE runs before the resampler and POST after the cabinet, so
    // neither ever sees the native 48 kHz the models run at.
    mPedals.prepare(mSampleRate, mMaxBlockSize);
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
    // The POST chain's two channels. Sized here so no pedal ever grows a buffer on the audio
    // thread; PedalChain::prepare then sizes each pedal's own state against the same block.
    mPostBufL.assign(n, 0.0);
    mPostBufR.assign(n, 0.0);
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
        // The pedals need no warm-up run: every one of them allocates in prepare() and nothing
        // downstream of it is lazy. What they do need is clearing, so an instance that was
        // deactivated mid-delay-tail does not resume it seconds later.
        mPedals.reset();
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
            const std::uint32_t lastBlock = mCcLastBlock[cc];
            mCcLast[cc] = static_cast<std::uint8_t>(ccValue);
            mCcLastBlock[cc] = mBlockIndex;

            // Matching fires on the PRESS. 64 is the MIDI switch threshold, so anything at or
            // above it is a press and anything below it is a release, which does nothing.
            //
            // WHAT A PRESS LOOKS LIKE DEPENDS ON THE CONTROLLER, and this used to assume one kind
            // of them. The rule was a RISING edge - at or above 64 having previously been below -
            // which is exactly right for a MOMENTARY switch, 127 down and 0 up. A programmable
            // footswitch is normally not that: a slot programmed to send CC 4 value 127 sends the
            // identical message on every press and nothing at all on release, so the second press
            // was not an edge and did nothing. On a channel row that is invisible, because
            // selecting Clean twice is selecting Clean; on a pedal row it is a footswitch that
            // works once and is then dead. Measured against the built bundle, not reasoned about:
            // three presses of one value gave on, nothing, nothing.
            //
            // So a press is any value at or above 64, and what the edge test was really protecting
            // is done by the BLOCK instead. The thing that must not fire repeatedly is a host
            // writing the same value into this parameter every block - a drawn automation lane, or
            // a controller resend - and that is precisely a repeat in the immediately following
            // block. A human cannot press twice inside one 2.67 ms period, so no real press is
            // ever suppressed, and no wall clock is consulted.
            //
            // The one controller this does not fully serve is an ALTERNATING latching one, which
            // sends 127, then 0, then 127, one message per press: its releases are indistinguishable
            // from a momentary switch's, so it takes two stamps per change. That is a mode to avoid
            // programming rather than a case to guess at; midilearn.h sets out all three kinds.
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
            const bool resent = ccValue == last && lastBlock + 1 == mBlockIndex && lastBlock != 0;
            const bool fire =
                learning ? (ccValue != last || ccValue >= 64) : (ccValue >= 64 && !resent);
            if (fire)
                midiTrigger(MidiMsg::ControlChange, kMidiAnyChannel, cc);
            continue;
        }
        if (id == kMidiProgramChangeId) {
            // The parameter is the program list's own, so its value is the program number over
            // the list's step count - 127 steps for 128 programs. A Program Change has no release,
            // so every message is a press; the only thing to suppress is the same program arriving
            // again in the very next block, which is a host resending rather than a second stamp.
            // Same rule as the CC branch above and for the same reason.
            const int program =
                std::clamp(static_cast<int>(std::lround(value * (kMidiProgramCount - 1))), 0, 127);
            const bool resent = program == mPcLast && mPcLastBlock + 1 == mBlockIndex;
            mPcLast = program;
            mPcLastBlock = mBlockIndex;
            if (!resent)
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
            case kCleanLevelId:
            case kCrunchLevelId:
            case kOd1LevelId:
            case kOd2LevelId: {
                for (int c = 0; c < kChannelCount; ++c)
                    if (kChannelLevelId[c] == id)
                        mChannelLevelNorm[c].store(value, std::memory_order_relaxed);
                break;
            }
            case kIrBlendId:
                mIrBlendNorm.store(value, std::memory_order_relaxed);
                break;
            // The output section. All three republish, because all three feed one decision and the
            // rack has to be told the whole of it rather than the part that moved.
            // publishOutputMode is only atomic stores, so doing it here on the audio thread is
            // legitimate — see the note on it, and on setPositionNorm in channelrack.cpp, which
            // publishes the same way.
            case kOutputModeId:
                mOutputModeNorm.store(value, std::memory_order_relaxed);
                publishOutputMode();
                break;
            case kCalibrateInputId:
                mCalibrateInput.store(value, std::memory_order_relaxed);
                publishOutputMode();
                break;
            case kInputCalLevelId:
                mCalLevelNorm.store(value, std::memory_order_relaxed);
                publishOutputMode();
                break;
            default:
                // The pedalboard: twenty-five parameters that all behave alike, so they are one
                // table lookup rather than twenty-five cases. Stored NORMALIZED; the denorm
                // happens once per sub-block in applyDsp against the same kPedalParams the
                // controller declared them from, which is what keeps the two from drifting.
                if (const int pedalIndex = pedalParamIndex(id); pedalIndex >= 0)
                    mPedalNorm[pedalIndex].store(std::clamp(value, 0.0, 1.0),
                                                 std::memory_order_relaxed);
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

        // What the row performs, and the ONE place the two kinds of row differ. A channel row
        // stores its own step; a pedal row flips its footswitch, which means reading the value
        // back before writing it - see midilearn.h for why a footswitch has to toggle and a
        // channel must not.
        //
        // The value that is actually stored is what gets echoed, never target.value: for a toggle
        // the two are not the same thing, and reporting the wrong one would leave the host's lane
        // and the LED disagreeing with the pedal that is audibly running.
        const MidiLearnTarget &target = kMidiLearnRows[r];
        double performed = target.value;
        if (target.param == kChannelId) {
            mChannelNorm.store(performed, std::memory_order_relaxed);
        } else if (const int pedalIndex = pedalParamIndex(target.param); pedalIndex >= 0) {
            const double now = mPedalNorm[pedalIndex].load(std::memory_order_relaxed);
            performed = (target.action == MidiAction::Toggle) ? (now > 0.5 ? 0.0 : 1.0)
                                                              : target.value;
            mPedalNorm[pedalIndex].store(performed, std::memory_order_relaxed);
        } else {
            continue; // a row whose target this function does not know how to perform
        }

        // Remember to tell the host. A parameter the plug-in changed by itself and did not report
        // leaves the host's automation lane and the editor's copy disagreeing with the audio,
        // until the next thing that writes it snaps the channel back under the player's feet.
        if (mEchoCount < kMidiLearnRowCount) {
            mEcho[mEchoCount].id = target.param;
            mEcho[mEchoCount].value = performed;
            ++mEchoCount;
        }
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API RationsProcessor::process(ProcessData &data)
{
    rations_set_denormal_mode();

    // The block counter is bumped before anything reads it, so "the block before this one" is a
    // comparison of two indices rather than a duration. See the press rule in
    // handleParameterChanges.
    ++mBlockIndex;

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
    float *outL = outBus.channelBuffers32[0];
    if (!outL)
        return kResultOk;
    // The second channel, when the host gave one. Everything up to the cabinet is mono; the POST
    // pedals are what makes the two differ, and with an empty board they stay identical.
    float *outR = outBus.numChannels > 1 ? outBus.channelBuffers32[1] : nullptr;
    if (outR == outL)
        outR = nullptr; // a host that handed us the same buffer twice

    // Where every dial is, before any audio is touched. All four are pushed, not just the
    // sounding one: an idle channel's dial decides which capture a switch to it would land on,
    // and which capture its worker should build first.
    for (int c = 0; c < kChannelCount; ++c) {
        mRack.setPositionNorm(static_cast<Channel>(c),
                              mChannelGainNorm[c].load(std::memory_order_relaxed));
        // And how loud each channel is. Pushed for all four for the same reason: the rack ramps
        // whichever ones are producing output, and an idle channel's trim has to be in place
        // before a switch lands on it, not applied afterwards.
        mRack.setLevel(static_cast<Channel>(c),
                       dbToLinear(denorm(mChannelLevelNorm[c].load(std::memory_order_relaxed),
                                         ranges::kLevelMin, ranges::kLevelMax)));
    }
    // And which channel the host wants. The rack decides when the audio can follow.
    mRack.requestChannel(channelFromNorm(mChannelNorm.load(std::memory_order_relaxed)));

    // Every pedal control, denormalized once per block rather than once per sub-block: these are
    // controls, not signals, and the table lookup is the same work whichever loop it sits in.
    for (int i = 0; i < kPedalParamCount; ++i)
        mPedalPlain[i] =
            pedalPlain(kPedalParams[i], mPedalNorm[i].load(std::memory_order_relaxed));

    // The host's tempo, for the Delay's sync divisions. A host is not obliged to supply a
    // ProcessContext at all, and the ones that do are not obliged to have a valid tempo in it, so
    // both are checked and a missing tempo falls back to the Delay's own free-running time.
    double tempo = 0.0;
    if (data.processContext && (data.processContext->state & ProcessContext::kTempoValid))
        tempo = data.processContext->tempo;
    mPedals.setTempo(tempo);

    // The host may hand us more than it promised in setupProcessing. Loop in whole sub-blocks
    // rather than clamping: clamping leaves the tail of the output buffer holding whatever was
    // there before, which is a stale-audio artefact, not a dropout.
    for (int32 done = 0; done < numSamples; done += mMaxBlockSize) {
        const int32 n = std::min(mMaxBlockSize, numSamples - done);
        applyDsp(in + done, outL + done, outR ? outR + done : nullptr, n);
    }

    // Anything past the second channel gets the right channel, or the left when the host gave a
    // mono bus. Two channels are what the POST pedals produce; a host asking for more than that is
    // asking for a wider version of the same thing, and duplicating is the only honest answer.
    for (int32 ch = outR ? 2 : 1; ch < outBus.numChannels; ++ch) {
        float *dst = outBus.channelBuffers32[ch];
        const float *src = outR ? outR : outL;
        if (dst && dst != src)
            std::memcpy(dst, src, static_cast<size_t>(numSamples) * sizeof(float));
    }
    outBus.silenceFlags = 0;

    // Feedback to the editor, all through the output parameter queue — never a direct call into
    // the controller, which is a UI-thread object.
    if (data.outputParameterChanges) {
        double inPeak = 0.0, outPeak = 0.0;
        for (int32 i = 0; i < numSamples; ++i) {
            inPeak = std::max(inPeak, std::fabs(static_cast<double>(in[i])));
            outPeak = std::max(outPeak, std::fabs(static_cast<double>(outL[i])));
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
void RationsProcessor::applyDsp(const float *in, float *outL, float *outR, int32 numSamples)
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

    // 2a. The PRE pedals: Boost then Chorus, mono, at the host rate.
    //
    // AFTER the gate's trigger and not before it. A gate keys off the guitar, and a boost ahead of
    // it would amplify exactly the noise the gate exists to remove — which is also the order a real
    // rig is wired in. And upstream of the resampler rather than inside ChannelRack: the prime
    // worker's ring then captures the pedals' output as ordinary signal history, so the channel
    // switch's 1e-6 convergence proof is untouched. A pedal inside the rack would sit between the
    // ring and the models and make all four channels cold on every knob move.
    mPedals.setParams(mPedalPlain);
    if (mPedals.preActive())
        mPedals.processPre(*processingInput, numSamples);

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

    // 6a. The POST pedals: Flanger, Delay, Reverb — after the cabinet, which is where they live in
    // a real rig, and where this plug-in stops being mono. The chain takes the one mono signal and
    // writes two channels; everything above it is unchanged and still mono.
    //
    // Skipped entirely when the board is empty, which is the common case and the one that has to
    // cost nothing: with nothing engaged the mono result is used directly and not one sample is
    // copied. postActive() goes false only once the last ramp has landed, so a pedal switched off
    // a moment ago is still processed and its fade still completes.
    const bool postOn = mPedals.postActive();
    if (postOn) {
        for (int32 i = 0; i < numSamples; ++i) {
            const size_t k = static_cast<size_t>(i);
            mPostBufL[k] = finalBuf[i];
            mPostBufR[k] = finalBuf[i];
        }
        mPedals.processPost(mPostBufL.data(), mPostBufR.data(), numSamples);
    }

    // 7. double -> float with output gain and the ramped bypass mix, now on two channels.
    //
    // Bypass is a per-sample ramp, never a switch: a hard mute or a hard hand-off to the dry
    // signal is itself a click, and it also exposes models that have been fed nothing while
    // bypassed. The chain above runs whether bypassed or not, so the bank stays primed and
    // un-bypassing is immediately correct. That costs CPU while bypassed, deliberately.
    //
    // mDryBuf is mono — it is the plug-in's own input — so it is what BOTH channels fade back to.
    // A stereo pedal's width therefore collapses as bypass is engaged, which is correct: bypassed
    // means the plug-in is out of the circuit, and the signal coming in was mono.
    const double target = mBypass.load(std::memory_order_relaxed) > 0.5 ? 1.0 : 0.0;
    double mix = mBypassMix;
    for (int32 i = 0; i < numSamples; ++i) {
        if (mix < target)
            mix = std::min(target, mix + mBypassStep);
        else if (mix > target)
            mix = std::max(target, mix - mBypassStep);
        const size_t k = static_cast<size_t>(i);
        const double dry = mDryBuf[k];
        const double wetL = (postOn ? mPostBufL[k] : finalBuf[i]) * outputGain;
        outL[i] = static_cast<float>(wetL + mix * (dry - wetL));
        if (outR) {
            const double wetR = (postOn ? mPostBufR[k] : finalBuf[i]) * outputGain;
            outR[i] = static_cast<float>(wetR + mix * (dry - wetR));
        }
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
        const Channel ch = static_cast<Channel>(c);
        const std::vector<std::string> names = mRack.captureNames(ch);
        const CaptureLevels levels = mRack.captureLevels(ch);

        // One helper rather than five spellings of the same concatenation: the attribute names are
        // a wire format, and five places to get a suffix wrong is five places for a channel to go
        // quietly missing on the other side.
        auto setChannelAttr = [&](const char *prefix, int64 value) {
            std::string attr(prefix);
            attr += kChannelDefaultName[c];
            attrs->setInt(attr.c_str(), value);
        };
        setChannelAttr(kCapsEntryCountAttr, static_cast<int64>(names.size()));
        setChannelAttr(kCapsIsDirAttr, mCaptureIsDir[c] ? 1 : 0);
        // The REAL values, read off the built bank. The parent plug-in hard-codes the level pair to
        // zero at this exact point, and the consequence in its shipped build is that Calibrated and
        // the whole input-calibration block are permanently greyed even for captures that do carry
        // the metadata. The DSP either side of it works; only this message lies.
        setChannelAttr(kCapsHasLoudnessAttr, levels.hasLoudness ? 1 : 0);
        setChannelAttr(kCapsHasInLevelAttr, levels.hasInputLevel ? 1 : 0);
        setChannelAttr(kCapsHasOutLevelAttr, levels.hasOutputLevel ? 1 : 0);

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

    // A channel's name. Nothing on the audio path reads it; it is here because this half writes
    // the state blob, and a name the user typed has to survive a project save.
    if (id && strcmp(id, kMsgChannelName) == 0) {
        int64 row = -1;
        if (message->getAttributes()->getInt(kMidiRowAttr, row) != kResultOk || row < 0 ||
            row >= kChannelCount)
            return kResultFalse;
        const void *nameData = nullptr;
        uint32 nameSize = 0;
        std::string name;
        if (message->getAttributes()->getBinary(kMsgNameAttr, nameData, nameSize) == kResultOk &&
            nameData && nameSize > 0)
            name.assign(static_cast<const char *>(nameData), nameSize);
        mChannelName[static_cast<int>(row)] = name;
        return kResultOk;
    }

    int captureChannel = -1;
    for (int c = 0; c < kChannelCount; ++c) {
        if (id && strcmp(id, kMsgLoadCapture[c]) == 0)
            captureChannel = c;
    }

    int slot = -1;
    for (int i = 0; i < kIrSlotCount; ++i) {
        if (id && strcmp(id, kMsgLoadIr[i]) == 0)
            slot = i;
    }
    if (slot < 0 && captureChannel < 0)
        return AudioEffect::notify(message);

    const void *data = nullptr;
    uint32 size = 0;
    std::string path;
    if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data &&
        size > 0)
        path.assign(static_cast<const char *>(data), size);

    if (captureChannel >= 0) {
        int64 isDir = 0;
        message->getAttributes()->getInt(kMsgIsDirAttr, isDir);
        loadCaptureSource(captureChannel, path, isDir != 0);
        // Immediately, and then again when the editor asks. This first one necessarily reports an
        // empty bank — the worker has not built anything yet — but it is what clears the PREVIOUS
        // bank's names out of the editor, so the row does not keep describing captures that are no
        // longer loaded while the new ones build.
        sendModelCaps();
        return kResultOk;
    }

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
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        mMidiBinding[row].store(0, std::memory_order_release);

    // How many rows are in the blob is a property of the BLOB, not of this build. Before version 6
    // it was not written down and was always four; from version 6 it is, because the pedalboard's
    // footswitch rows made this build's table nine and a fixed count sitting in the middle of a
    // blob is what makes everything after it unreadable. See kStateVersion.
    int32 midiRows = (version >= 2) ? kMidiLearnRowsV2 : 0;
    if (version >= 6) {
        if (!streamer.readInt32(midiRows))
            return kResultFalse;
        if (midiRows < 0 || midiRows > kMidiRowStateMax)
            return kResultFalse;
    }
    for (int32 row = 0; row < midiRows; ++row) {
        int32 raw = 0;
        if (!streamer.readInt32(raw))
            return kResultFalse;
        // A row this build does not have is READ and dropped, never skipped by seeking: what has
        // to happen is that the stream ends up in the right place for the trims, and reading is
        // the only way to be sure it did.
        if (row < kMidiLearnRowCount)
            mMidiBinding[row].store(packBinding(unpackBinding(static_cast<std::uint32_t>(raw))),
                                    std::memory_order_release);
    }

    // The per-channel trims, added in state version 3. A version 1 or 2 project has nothing here
    // and opens with every trim at 0 dB - which is the level it was mixed at, so the older project
    // sounds exactly as it did.
    for (int c = 0; c < kChannelCount; ++c) {
        double level = 0.5;
        if (version >= 3 && !streamer.readDouble(level))
            return kResultFalse;
        mChannelLevelNorm[c].store(std::clamp(level, 0.0, 1.0), std::memory_order_relaxed);
    }

    // The output section, added in state version 4. The defaults for an older project are what
    // that project actually sounded like: every build before version 4 was hard-wired to Normalized
    // with no calibration, so those are the values a version 1-3 blob opens with, and it sounds
    // exactly as it did.
    double outputMode = normFromOutputMode(kOutputNormalized);
    double calibrate = 0.0;
    double calLevel = 0.6; // +12 dBu over the -60 .. +60 range
    if (version >= 4) {
        if (!streamer.readDouble(outputMode) || !streamer.readDouble(calibrate) ||
            !streamer.readDouble(calLevel))
            return kResultFalse;
    }
    // Snapped through the decoder, like the channel above: a blob written by a future version with
    // a fourth mode, or simply a corrupt one, must not leave an unnamed mode selected.
    mOutputModeNorm.store(normFromOutputMode(outputModeFromNorm(outputMode)),
                          std::memory_order_relaxed);
    mCalibrateInput.store(calibrate > 0.5 ? 1.0 : 0.0, std::memory_order_relaxed);
    mCalLevelNorm.store(std::clamp(calLevel, 0.0, 1.0), std::memory_order_relaxed);
    publishOutputMode();

    // The four capture sources. A version 1-3 project has none, and cannot be given any: those
    // builds resolved the banks from inside the bundle and never wrote down where they came from.
    // So an older project opens with four empty channels and the settings page asking for them,
    // which is silence a user can fix rather than a guess at a path.
    for (int c = 0; c < kChannelCount; ++c) {
        int32 isDir = 0;
        std::string path, name;
        if (version >= 4) {
            if (!streamer.readInt32(isDir))
                return kResultFalse;
            if (char8 *p = streamer.readStr8()) {
                path = p;
                delete[] p;
            }
            if (char8 *p = streamer.readStr8()) {
                name = p;
                delete[] p;
            }
        }
        mChannelName[c] = name;

        // Nothing here says which entry to build first, and the parent plug-in's setState does.
        // It does not need to: ModelBank re-reads its priority hint on every entry, it publishes
        // the bank before building anything, and CrossfadeEngine::hintPriority runs from the
        // restored dial position within a block of that. The first model takes ~150 ms to build
        // and the hint arrives inside 3 ms, so a reopened project is already playable at the
        // position it was saved at without this half of the blob knowing anything about indices.

        // Recording the path is not enough, it has to be LOADED - the same reason the IR paths are
        // reloaded above. A path that no longer resolves leaves the channel empty rather than
        // claiming a bank that is not there; ModelBank prints one warning and the channel is
        // silent, which is exactly what a missing folder should look like.
        //
        // But only when it has actually CHANGED. A host may push state at a plug-in that is already
        // playing - that is the whole reason this reload exists - and a preset naming the captures
        // that are already loaded must not rebuild them: republishing a bank costs thirty-odd
        // models, drops that channel to its ramped-silence gate while they build, and makes all
        // four channels go cold, so a preset change that alters nothing but the tone stack would
        // silence the amp for a second. Skipping the identical case is not an optimisation, it is
        // the difference between a preset change and a reload.
        //
        // Deliberately NOT done inside loadCaptureSource: a user who picks the same folder again in
        // the browser is asking for it to be re-read, and may well be doing it because they just
        // changed what is in it.
        if (mCapturePath[c] != path || mCaptureIsDir[c] != (isDir != 0))
            loadCaptureSource(c, path, isDir != 0);
    }

    // Version 5 onwards: the pedalboard, length-prefixed. A version 1-4 project has no block here
    // and opens with every pedal off and at its default, which is what those projects sounded
    // like — the pedals did not exist.
    if (version >= 5) {
        int32 count = 0;
        if (!streamer.readInt32(count))
            return kResultFalse;
        // A state blob is untrusted input. A negative count is malformed; a very large one is
        // either malformed or a future build with far more controls than this one, and neither is
        // worth stalling on, so the read is bounded before it is trusted.
        if (count < 0 || count > kPedalStateMax)
            return kResultFalse;
        for (int32 i = 0; i < count; ++i) {
            double value = 0.0;
            if (!streamer.readDouble(value))
                return kResultFalse;
            // Values this build does not have are read and discarded rather than skipped, because
            // the stream has to be consumed either way and there is nothing after this block.
            if (i < kPedalParamCount)
                mPedalNorm[i].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        // A SHORTER block than this build expects is an older version-5 project written before a
        // knob was added. Everything it did not carry keeps the default the constructor set, which
        // is why those defaults are set there and not here.
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
    //
    // Version 6 onwards the block is LENGTH-PREFIXED, and that is the whole of what version 6 is:
    // this table grew from four rows to nine when the pedalboard's footswitches joined it, and a
    // count that a reader has to guess is a count two builds can disagree about silently.
    streamer.writeInt32(kMidiLearnRowCount);
    for (int row = 0; row < kMidiLearnRowCount; ++row)
        streamer.writeInt32(static_cast<int32>(mMidiBinding[row].load(std::memory_order_acquire)));

    // Version 3 onwards: the four channel trims.
    for (int c = 0; c < kChannelCount; ++c)
        streamer.writeDouble(mChannelLevelNorm[c].load(std::memory_order_relaxed));

    // Version 4 onwards: the output section, then where each channel's captures came from and what
    // the user calls it. The fixed-width triple goes first so the variable-length half is all in
    // one place at the end, which is the shape the two IR paths already gave this blob.
    streamer.writeDouble(mOutputModeNorm.load(std::memory_order_relaxed));
    streamer.writeDouble(mCalibrateInput.load(std::memory_order_relaxed));
    streamer.writeDouble(mCalLevelNorm.load(std::memory_order_relaxed));

    // The path is stored absolute and whole, not as a basename. A capture bank is not part of this
    // plug-in any more and there is nowhere else to look for it, so a name without a directory
    // would be a project that cannot reopen. Whether it was a FOLDER is stored beside it rather
    // than re-derived on load: a bank of one is a bank of one either way, and asking the filesystem
    // months later answers about the disk rather than about what the user chose.
    for (int c = 0; c < kChannelCount; ++c) {
        streamer.writeInt32(mCaptureIsDir[c] ? 1 : 0);
        streamer.writeStr8(mCapturePath[c].c_str());
        streamer.writeStr8(mChannelName[c].c_str());
    }

    // Version 5 onwards: the pedalboard, LENGTH-PREFIXED. Every other block in this blob is a
    // fixed set of fields that a version bump has to be spent on, which is right for a field that
    // means something specific — but a pedal growing a knob is a thing that will happen more than
    // once, and spending a state version on each would make every such change a compatibility
    // event. A count lets a reader take what it understands and skip the rest.
    //
    // The values are NORMALIZED, like every other parameter here, so a range widening later
    // reinterprets old projects rather than corrupting them.
    streamer.writeInt32(kPedalParamCount);
    for (int i = 0; i < kPedalParamCount; ++i)
        streamer.writeDouble(mPedalNorm[i].load(std::memory_order_relaxed));
    return kResultOk;
}

} // namespace Rations
