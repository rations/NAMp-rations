// Rations — the Flanger pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// The same modulated-delay core as the Chorus but at 0.5-8 ms and WITH feedback, which is the
// whole difference: a flanger is a comb filter swept through itself. Regen is signed — negative
// feedback gives the hollow jet, positive gives peaks. This is where the POST chain widens to
// stereo, by taking the two channels' LFO phases 180 degrees apart.
#pragma once

#include "pedal.h"

namespace Rations
{
namespace pedals
{

class Flanger final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kRate = 1, kDepth = 2, kManual = 3, kRegen = 4 };

    void setParams(const double *plain) override
    {
        mRate = plain[kRate];
        mDepth = plain[kDepth];
        mManual = plain[kManual];
        mRegen = plain[kRegen];
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
    double mRate = 0.3, mDepth = 70.0, mManual = 30.0, mRegen = 50.0;
};

} // namespace pedals
} // namespace Rations
