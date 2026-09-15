// NativeResampler — runs a block of DSP at the models' native rate (48 kHz) regardless of the
// session rate, using AudioDSPTools' ResamplingContainer (Lanczos, A=12).
//
// There is exactly ONE of these per plug-in instance, and the whole crossfade runs INSIDE it.
// That is deliberate and it matters in three ways:
//
//   * Cost. Each LanczosResampler holds a 131072-sample buffer per channel; a ResamplingContainer
//     builds two of them, so it costs ~4 MiB. One per model would be ~40 MiB for a ten-capture
//     bank, for no benefit.
//   * Correctness. Two independent Lanczos instances fed the same signal drift in phase relative
//     to each other, so crossfading their outputs sweeps a comb filter through the fade. Fading
//     upstream of a single resampler cannot do that.
//   * Stability. Reported latency stays independent of which capture is sounding, so the plug-in
//     never has to tell the host its latency changed because the user turned a knob.
//
// At the native rate the container is NOT CONSTRUCTED AT ALL: process() calls straight through,
// and latency() reports 0. (The sibling single-capture plug-in keeps its container alive at
// 48 kHz and reports its 29-sample latency even while bypassing it, which misreports latency to
// the host by 29 samples. Fixed here by never building the object.)
//
// Known upstream hazards, flagged rather than hidden: ResamplingContainer::ProcessBlock can
// throw std::runtime_error and can write to std::cerr, neither of which is real-time safe. Both
// only fire on an internal inconsistency (the resampler failing to yield the samples it just
// promised), and fixing them would mean modifying an imported upstream tree. Do not add new
// exception or logging paths of our own on this route.

#pragma once

// Compatibility shims required by the iPlug2-derived LanczosResampler that AudioDSPTools'
// ResamplingContainer bundles. These symbols are normally provided by iPlug2's IPlugPlatform.h
// which we don't include.
#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 128
#endif
namespace iplug
{
static constexpr double PI = 3.14159265358979323846;
}

#include "NAM/dsp.h"
#include "ResamplingContainer/ResamplingContainer.h"

#include <cmath>
#include <memory>

namespace Rations
{

// The rate NAM captures are made at, and the rate every model in a bank must expect. A capture
// that reports something else is rejected at bank-build time rather than silently resampled to
// the wrong target.
inline constexpr double kNativeSampleRate = 48000.0;

//------------------------------------------------------------------------
// The DSP that runs at the native rate. Implemented by the crossfade engine. May be called more
// than once per host block, with a different length each time — which is exactly why the engine
// must not assume it sees whole host blocks.
class NativeBlockProcessor
{
public:
    virtual ~NativeBlockProcessor() = default;
    virtual void processNative(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames) = 0;
};

//------------------------------------------------------------------------
class NativeResampler
{
public:
    // Non-RT: called from setupProcessing only. Builds or drops the container to match the host
    // rate. Safe to call repeatedly with the same arguments (the container just clears).
    void configure(double hostSampleRate, int maxHostBlock, double nativeRate = kNativeSampleRate)
    {
        mHostSampleRate = hostSampleRate;
        mNativeSampleRate = nativeRate;
        mMaxHostBlock = maxHostBlock > 0 ? maxHostBlock : 1;

        if (hostSampleRate == nativeRate) {
            mContainer.reset(); // nothing to convert; do not pay for the object
            return;
        }
        if (!mContainer)
            mContainer = std::make_unique<Container>(nativeRate);
        mContainer->Reset(hostSampleRate, mMaxHostBlock);
    }

    bool engaged() const
    {
        return mContainer != nullptr;
    }

    // Samples of latency the conversion adds, as reported to the host. Zero when bypassed.
    int latency() const
    {
        return mContainer ? mContainer->GetLatency() : 0;
    }

    // Upper bound on the native-rate frame count a single host block of `hostBlock` frames can
    // turn into. Mirrors ResamplingContainer::MaxEncapsulatedBlockSize, which is private there.
    // Used to size native scratch buffers outside the RT path.
    int maxNativeBlock(int hostBlock) const
    {
        if (!mContainer)
            return hostBlock;
        const double ratio = mHostSampleRate / mNativeSampleRate;
        return static_cast<int>(std::ceil(static_cast<double>(hostBlock) / ratio));
    }

    // RT-safe. Runs `p` at the native rate, converting in and out as needed.
    void process(NAM_SAMPLE **in, NAM_SAMPLE **out, int numFrames, NativeBlockProcessor &p)
    {
        if (!mContainer) {
            p.processNative(in, out, numFrames);
            return;
        }
        // The lambda captures ONE reference, so the std::function ProcessBlock takes by value
        // stays inside libstdc++'s small-object buffer and does not allocate. Never widen this
        // capture list — ResamplingContainer.h warns about exactly this.
        mContainer->ProcessBlock(in, out, numFrames, [&p](NAM_SAMPLE **i, NAM_SAMPLE **o, int n) {
            p.processNative(i, o, n);
        });
    }

private:
    using Container = dsp::ResamplingContainer<NAM_SAMPLE, 1, 12>;

    std::unique_ptr<Container> mContainer;
    double mHostSampleRate = kNativeSampleRate;
    double mNativeSampleRate = kNativeSampleRate;
    int mMaxHostBlock = 1;
};

} // namespace Rations
