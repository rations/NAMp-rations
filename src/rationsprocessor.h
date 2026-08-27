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
// All four channels are wired: each loads its own bank out of the bundle's Resources/captures,
// each has its own dial sweeping that bank, and kChannelId picks which one sounds. The rack owns
// the switch between them and the input history that makes it click-free; the processor's job is
// only to keep it fed and to hand the host back which channel is ACTUALLY sounding, which is not
// always the one the parameter asks for. The second IR slot and its blend are still stubbed.
//
// Output is pinned to Normalized. It is not a user choice here, so there is no parameter for it
// and the engine is told once — see the setOutputMode call in setupProcessing.
//
// Real-time contract: process() never allocates, locks, does file I/O, logs, or destroys an
// object. That holds from here on, including for the channel-switch catch-up burst, which runs
// inside process() like everything else.

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "channelrack.h"
#include "irblend.h"
#include "nativeresampler.h"
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
    void applyDsp(const float *in, float *out, Steinberg::int32 numSamples);
    bool loadIr(int slot, const std::string &path); // message thread only
    // Run a unit impulse through a freshly loaded IR to capture what it actually does - after
    // resampling to the host rate, after the class's own gain, after its 8192-sample truncation -
    // then flush it back to an all-zero history so the audio thread still gets a pristine object.
    // Doubles as the warm-up that sizes the IR's lazily allocated buffers off the audio thread.
    void profileIr(int slot); // message thread only
    void remeasureBlend();    // message thread only
    void sendModelCaps();     // message thread only
    void allocateBuffers();   // message thread only
    // The bundled bank for one channel: <resources>/captures/<Clean|Crunch|OD1|OD2>. Message
    // thread only — it touches the filesystem.
    static std::string channelBankDir(Channel ch);

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

    std::atomic<double> mIrBlendNorm{0.0}; // 0 = IR A; inert unless both slots are filled

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

    // Bypass mix position: 0 = fully processed, 1 = fully dry. Ramped, never switched.
    double mBypassMix = 0.0;
    double mBypassStep = 1.0;

    // Message-thread only.
    std::string mIrPath[kIrSlotCount];

    double mSampleRate = kNativeSampleRate;
    Steinberg::int32 mMaxBlockSize = 512;
};

// Decode kChannelId's normalized value to a Channel. A kIsList parameter with N steps reports
// value i as i / (N - 1), so this is the inverse, rounded and clamped: a host is free to hand
// over any double in [0, 1], including one that lands between steps.
Channel channelFromNorm(double norm);
double normFromChannel(Channel ch);

} // namespace Rations
