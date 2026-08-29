// Rations — the Reverb pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Built on deps/wdl/verbengine.h — Cockos' WDL_ReverbEngine, itself derived from Jezar at
// Dreampoint's public-domain Freeverb, so eight comb filters and four allpasses per channel with
// the right channel detuned against the left. Vendored whole and wrapped rather than written,
// because a tuned Schroeder/Moorer reverb is better than a fresh one and because a hand-rolled
// reverb is the easiest thing on this board to get subtly wrong.
//
// What this class adds around it: pre-delay, a damping/tone control and the wet/dry mix.
#pragma once

#include "pedal.h"

namespace Rations
{
namespace pedals
{

class Reverb final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kDecay = 1, kTone = 2, kPreDelay = 3, kMix = 4 };

    void setParams(const double *plain) override
    {
        mDecay = plain[kDecay];
        mTone = plain[kTone];
        mPreDelayMs = plain[kPreDelay];
        mMix = plain[kMix];
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        (void)sampleRate;
        (void)maxBlock;
    }
    void resetImpl() override {}
    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        // A pass-through until P7. The chain, the parameters, the state blob and the panel are
        // wired first and proved to change nothing, so that when the DSP lands the only thing that
        // has changed is the DSP.
        (void)l;
        (void)r;
        (void)numSamples;
    }

private:
    double mDecay = 4.0, mTone = 5.0, mPreDelayMs = 20.0, mMix = 25.0;
};

} // namespace pedals
} // namespace Rations
