// Rations — the Chorus pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// A modulated delay in the 5-30 ms range with two voices in phase quadrature and no feedback.
// Mono, because it sits in the PRE chain feeding one mono amp head — which is what a chorus in
// front of an amp physically is.
#pragma once

#include "pedal.h"

namespace Rations
{
namespace pedals
{

class Chorus final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kRate = 1, kDepth = 2, kMix = 3 };

    void setParams(const double *plain) override
    {
        mRate = plain[kRate];
        mDepth = plain[kDepth];
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
        // A pass-through until P5. The chain, the parameters, the state blob and the panel are
        // wired first and proved to change nothing, so that when the DSP lands the only thing that
        // has changed is the DSP.
        (void)l;
        (void)r;
        (void)numSamples;
    }

private:
    double mRate = 0.8, mDepth = 50.0, mMix = 50.0;
};

} // namespace pedals
} // namespace Rations
