// Rations — the pedalboard's two chains. See pedalchain.h.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
#include "pedalchain.h"

namespace Rations
{
namespace pedals
{

void PedalChain::prepare(double sampleRate, int maxBlock)
{
    for (int i = 0; i < kPedalCount; ++i)
        mAll[i]->prepare(sampleRate, maxBlock);
}

void PedalChain::reset()
{
    for (int i = 0; i < kPedalCount; ++i)
        mAll[i]->reset();
}

void PedalChain::setParams(const double *plain)
{
    for (int i = 0; i < kPedalCount; ++i) {
        const int first = pedalParamFirst(i);
        // Slice 0 is the footswitch, by the static_assert in rationsids.h.
        mAll[i]->setEngaged(plain[first] > 0.5);
        mAll[i]->setParams(plain + first);
    }
    mDelay.setTempo(mTempoBpm);
}

void PedalChain::processPre(DSP_SAMPLE *mono, int numSamples)
{
    // Mono: the right channel is null, and neither of these two ever looks at it. A real rig's
    // pre-amp pedals feed one mono amp input, so this is the physical arrangement rather than a
    // simplification.
    mBoost.process(mono, nullptr, numSamples);
    mChorus.process(mono, nullptr, numSamples);
}

void PedalChain::processPost(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples)
{
    mFlanger.process(l, r, numSamples);
    mDelay.process(l, r, numSamples);
    mReverb.process(l, r, numSamples);
}

bool PedalChain::preActive() const
{
    return mBoost.active() || mChorus.active();
}

bool PedalChain::postActive() const
{
    return mFlanger.active() || mDelay.active() || mReverb.active();
}

} // namespace pedals
} // namespace Rations
