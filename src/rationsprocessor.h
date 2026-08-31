// Rations processor — audio component (mono or stereo in, mono or stereo out).
//
// The chain:
//
//   input gain -> noise-gate trigger -> [ native-rate resampler { channel rack } ]
//     -> noise-gate gain -> tone stack -> IR -> output gain -> bypass ramp
//
// with the gate, tone stack and IR running at the host rate on the whole block, once per block
// (their AudioDSPTools buffers are allocated lazily and pre-warmed at exactly the maximum block
// size, and handing them a varying size would re-trigger that growth on the audio thread), and
// every model call going through a fixed chunk loop at the models' native rate.
//
// All four channels are wired: each loads its own bank from a folder or a file the user picked,
// each has its own dial sweeping that bank, and kChannelId picks which one sounds. The rack owns
// the switch between them and the input history that makes it click-free; the processor's job is
// only to keep it fed and to hand the host back which channel is ACTUALLY sounding, which is not
// always the one the parameter asks for. Both IR slots and their blend are wired, and so is the
// MIDI learn table that lets a footswitch change the channel with the editor closed.
//
// Output mode is a user choice (Raw / Normalized / Calibrated) and is PUBLISHED to the rack every
// block rather than applied to the engines directly: three of the four engines belong to the prime
// worker at any instant, and writing into them from here is the shared access the ownership rule
// exists to forbid. Input calibration is published the same way and per channel — see the note on
// it in channelrack.h, which is the one part of this that is not a copy of the parent plug-in.
//
// Real-time contract: process() never allocates, locks, does file I/O, logs, or destroys an
// object. That holds from here on, including for the channel-switch catch-up burst, which runs
// inside process() like everything else.

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "channelrack.h"
#include "irblend.h"
#include "midilearn.h"
#include "nativeresampler.h"
#include "pedals/pedalchain.h"
#include "rationsids.h"

#include "ImpulseResponse.h"
#include "NoiseGate.h"
#include "ToneStack.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Rations
{

//------------------------------------------------------------------------
class RationsProcessor : public Steinberg::Vst::AudioEffect
{
public:
    RationsProcessor();
    ~RationsProcessor() override;

    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IAudioProcessor *)new RationsProcessor();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement *outputs,
                                                     Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) SMTG_OVERRIDE;

    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;

    // Controller -> processor messages (message thread).
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *message) SMTG_OVERRIDE;

private:
    void handleParameterChanges(Steinberg::Vst::IParameterChanges *changes);
    // The MIDI half of a block, both on the audio thread. A CC or Program Change arrives as a
    // parameter change and is picked out of handleParameterChanges; a Note On arrives as a real
    // event and comes through here. Both end at midiTrigger().
    void handleInputEvents(Steinberg::Vst::IEventList *events);
    // One incoming message, already decoded. Either captures it into the row being learned or
    // matches it against the table and performs what that row says. Audio thread; allocates
    // nothing, takes no lock, and never calls the controller.
    void midiTrigger(MidiMsg msg, int channel, int data1);
    // Stereo out, because the POST pedals are where this plug-in stops being mono. `outR` is null
    // when the host gave a mono output bus; everything up to and including the cabinet is mono
    // either way, so only the tail changes.
    void applyDsp(const float *in, float *outL, float *outR, Steinberg::int32 numSamples);
    bool loadIr(int slot, const std::string &path); // message thread only
    // Run a unit impulse through a freshly loaded IR to capture what it actually does - after
    // resampling to the host rate, after the class's own gain, after its 8192-sample truncation -
    // then flush it back to an all-zero history so the audio thread still gets a pristine object.
    // Doubles as the warm-up that sizes the IR's lazily allocated buffers off the audio thread.
    void profileIr(int slot); // message thread only
    void remeasureBlend();    // message thread only
    void sendModelCaps();     // message thread only
    void sendMidiTable();     // message thread only
    void allocateBuffers();   // message thread only
    // Load one channel's bank from a folder (isDirectory) or a single file. An empty path clears
    // the channel, which the rack answers with ramped silence. Message thread only — file I/O.
    void loadCaptureSource(int channel, const std::string &path, bool isDirectory);
    // Publish the output section to the rack. Safe from EITHER thread - it is nothing but atomic
    // stores - which it has to be, because a radio click on the settings page arrives as a host
    // parameter change on the audio thread while a load or a state restore arrives on the message
    // thread.
    void publishOutputMode();

    // Normalized parameter values. Written by RT parameter handling AND by setState on the
    // message thread, so the accesses are atomic to stay tear-free; relaxed ordering is enough
    // because each one stands alone.
    std::atomic<double> mBypass{0.0};
    std::atomic<double> mInputGainNorm{0.5};   // plain 0 dB
    std::atomic<double> mOutputGainNorm{0.5};  // plain 0 dB
    std::atomic<double> mNgThresholdNorm{0.2}; // plain -80 dB
    std::atomic<double> mBassNorm{0.5};        // plain 5
    std::atomic<double> mMiddleNorm{0.5};      // plain 5
    std::atomic<double> mTrebleNorm{0.5};      // plain 5
    std::atomic<double> mNoiseGateOn{1.0};

    // The active channel, as the normalized value of the kChannelId list parameter. Stored
    // normalized rather than as a Channel so setState, automation and the RT path all speak the
    // one representation the host does; channelFromNorm() is the only place it is decoded.
    std::atomic<double> mChannelNorm{0.0}; // Clean
    std::atomic<double> mChannelGainNorm[kChannelCount] = {{0.0}, {0.0}, {0.0}, {0.0}};
    // Per-channel output trim, normalized. 0.5 is 0 dB, which is what the range's centre
    // denormalizes to and what every one of these has to start at: an amp that came up with a
    // channel quieter than the player left it would be worse than one with no trim at all.
    std::atomic<double> mChannelLevelNorm[kChannelCount] = {{0.5}, {0.5}, {0.5}, {0.5}};

    std::atomic<double> mIrBlendNorm{0.0}; // 0 = IR A; inert unless both slots are filled

    // The output section. Normalized is the default because it is what every build before state
    // version 4 was hard-wired to, so a project made against one of those sounds the same here.
    std::atomic<double> mOutputModeNorm{normFromOutputMode(kOutputNormalized)};
    std::atomic<double> mCalibrateInput{0.0}; // off
    std::atomic<double> mCalLevelNorm{0.6};   // 0.6 over -60 .. +60 is +12 dBu

    // --- MIDI learn ---------------------------------------------------------------------
    //
    // The table lives here rather than in the controller because a footswitch has to work with
    // the editor closed, and it is read on the audio thread. One packed word per row (midilearn.h
    // does the packing) so a row is published in a single atomic store and the audio thread can
    // never read half of an edit. Only the BINDING is published: what a row performs is fixed at
    // compile time in kMidiLearnRows, so there is no target to hand across a thread boundary.
    std::atomic<std::uint32_t> mMidiBinding[kMidiLearnRowCount] = {};
    // Which row is waiting to be taught, or -1. Armed from the editor on the message thread,
    // cleared by the audio thread the moment it captures something.
    std::atomic<int> mMidiLearnRow{-1};
    // Last value seen on each controller number, so a learned CC fires on the press and not again
    // on the release, and not once per block for as long as a pedal is held down. Audio thread
    // only. uint8 rather than a float: what matters is which side of 64 it was on.
    std::uint8_t mCcLast[kMidiCcCount] = {};
    // ... and which BLOCK each controller number last delivered a point in, which is what
    // separates a second press from a host writing the same value over and over. See the press
    // rule in handleParameterChanges. Block indices are compared, never subtracted from a clock:
    // 0 means "never", mBlockIndex counts from 1, and it is monotonic, so a stale entry can only
    // ever fail to match. Audio thread only.
    std::uint32_t mCcLastBlock[kMidiCcCount] = {};
    // The same pair for Program Change, which arrives on one parameter carrying a program NUMBER
    // rather than on 128 of them. -1 is "nothing seen yet", which no program number can be.
    int mPcLast = -1;
    std::uint32_t mPcLastBlock = 0;
    // Counts process() calls, so "the block before this one" is a comparison rather than a time.
    // Wraps after 132 years at a 128-frame period, and a wrap costs one missed suppression.
    std::uint32_t mBlockIndex = 0;

    // --- what the processor has heard, for the settings page to report ------------------
    //
    // Published by the audio thread as three relaxed atomic stores and read on the message thread
    // by sendMidiTable. Not a debug facility: while a row is listening the page says what is
    // arriving, because "I pressed it and nothing happened" is otherwise unsplittable into "the
    // plug-in never received it" and "the plug-in received it and the row is wrong", and the two
    // want completely different fixes. The block count is there so a silent MIDI count can be told
    // apart from an audio thread that is not running.
    std::atomic<std::uint32_t> mSeenWord{0};   // packBinding of the last message, plus its value
    std::atomic<std::uint32_t> mSeenCount{0};  // every message the table was offered, ever
    std::atomic<std::uint32_t> mBlockCount{0}; // process() calls
    // Parameter changes this plug-in made to itself this block, to be echoed to the host so its
    // automation lane and the editor agree with the audio. Fixed capacity, filled and drained
    // inside one process() call, so it never grows on the audio thread.
    struct ParamEcho {
        Steinberg::Vst::ParamID id;
        double value;
    };
    ParamEcho mEcho[kMidiLearnRowCount];
    int mEchoCount = 0;

    // Four banks, four crossfade engines, one input ring and the switch between them. The single
    // native-rate block processor the shared resampler drives.
    ChannelRack mRack;

    // Two IR slots. Slot A alone is the normal case and takes exactly the path it took before the
    // second slot existed - one Process call, no mixing, the blend not consulted - so a user who
    // never opens slot B cannot tell it is there. See applyDsp.
    std::unique_ptr<dsp::ImpulseResponse> mPendingIR[kIrSlotCount];
    std::unique_ptr<dsp::ImpulseResponse> mIR[kIrSlotCount];
    std::unique_ptr<dsp::ImpulseResponse> mRetiredIR[kIrSlotCount];
    std::atomic<bool> mIRPending[kIrSlotCount] = {{false}, {false}};

    // The measured relationship between the two loaded IRs, published the same way the IRs
    // themselves are. Recomputed on the message thread whenever either slot changes; the audio
    // thread only ever copies it.
    IrBlend mPendingBlend;
    IrBlend mBlend;
    std::atomic<bool> mBlendPending{false};
    // Each slot's impulse response, captured at load time so the profile can be recomputed when
    // the OTHER slot changes without going back to the file. Message thread only.
    std::vector<double> mIrProfile[kIrSlotCount];

    // Latency reported to the host. Depends only on the resampler, so it changes on a sample-rate
    // change and never on a bank swap or a knob turn.
    std::atomic<Steinberg::uint32> mLatency{0};

    NativeResampler mResampler;

    dsp::noise_gate::Trigger mNoiseGateTrigger;
    dsp::noise_gate::Gain mNoiseGateGain;
    dsp::tone_stack::BasicNamToneStack mToneStack;

    // Pre-allocated double-precision work buffers, sized in setupProcessing for mMaxBlockSize.
    // mDryBuf keeps the ungained input so the bypass ramp has something to fade back to.
    std::vector<DSP_SAMPLE> mWorkBufInput;
    std::vector<DSP_SAMPLE> mWorkBufOutput;
    std::vector<DSP_SAMPLE> mDryBuf;
    // Where the two IRs are summed when both slots are filled. Only touched on that path, so the
    // single-IR case never writes through it.
    std::vector<DSP_SAMPLE> mIrMixBuf;
    DSP_SAMPLE *mIrMixPtr = nullptr;
    DSP_SAMPLE *mWorkPtrInput = nullptr;
    DSP_SAMPLE *mWorkPtrOutput = nullptr;

    // --- the pedalboard ---------------------------------------------------------------------
    pedals::PedalChain mPedals;
    // The two stereo buffers the POST chain works in. Allocated with everything else in
    // allocateBuffers; nothing downstream of the cabinet allocates on the audio thread.
    std::vector<DSP_SAMPLE> mPostBufL;
    std::vector<DSP_SAMPLE> mPostBufR;
    // Every pedal control, normalized, written by RT parameter handling and by setState. Denorm
    // happens once per sub-block into mPedalPlain, which never leaves the audio thread.
    std::atomic<double> mPedalNorm[kPedalParamCount];
    double mPedalPlain[kPedalParamCount] = {};

    // Bypass mix position: 0 = fully processed, 1 = fully dry. Ramped, never switched.
    double mBypassMix = 0.0;
    double mBypassStep = 1.0;

    // Message-thread only.
    std::string mIrPath[kIrSlotCount];
    // Where each channel's captures came from, and what the user calls that channel. Held here
    // because this is the half that writes the state blob; nothing on the audio path reads either.
    // An empty name is not a name - it means the channel has no override and shows the basename of
    // its path, or its default name when there is no path either.
    std::string mCapturePath[kChannelCount];
    bool mCaptureIsDir[kChannelCount] = {false, false, false, false};
    std::string mChannelName[kChannelCount];

    double mSampleRate = kNativeSampleRate;
    Steinberg::int32 mMaxBlockSize = 512;
};

} // namespace Rations
