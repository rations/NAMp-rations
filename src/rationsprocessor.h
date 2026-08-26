// Rations processor — audio component (mono or stereo in, mono or stereo out).
//
// PHASE 0 SKELETON. This is the parameter, state and bus plumbing only: process() copies its
// input to its output. The DSP chain arrives with the phases that own it, and will be
//
//   input gain -> noise-gate trigger -> [ native-rate resampler { channel rack } ]
//     -> noise-gate gain -> tone stack -> IR A/B blend -> output gain -> bypass ramp
//
// with the gate, tone stack and IR running at the host rate on the whole block, once per block
// (their AudioDSPTools buffers are allocated lazily and pre-warmed at exactly the maximum block
// size, and handing them a varying size would re-trigger that growth on the audio thread), and
// every model call going through a fixed chunk loop at the models' native rate.
//
// Real-time contract: process() never allocates, locks, does file I/O, logs, or destroys an
// object. That holds from here on, including for the channel-switch catch-up burst, which runs
// inside process() like everything else.

#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"

#include "rationsids.h"

#include <atomic>
#include <string>

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
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) SMTG_OVERRIDE;

private:
    void handleParameterChanges(Steinberg::Vst::IParameterChanges *changes);

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

    // Message-thread only.
    std::string mIrPathA;
    std::string mIrPathB;

    double mSampleRate = 48000.0;
    Steinberg::int32 mMaxBlockSize = 512;
};

// Decode kChannelId's normalized value to a Channel. A kIsList parameter with N steps reports
// value i as i / (N - 1), so this is the inverse, rounded and clamped: a host is free to hand
// over any double in [0, 1], including one that lands between steps.
Channel channelFromNorm(double norm);
double normFromChannel(Channel ch);

} // namespace Rations
