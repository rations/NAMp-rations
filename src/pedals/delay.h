// Rations — the Delay pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Stereo fractional delay lines with a filtered feedback loop, so repeats darken the way an
// analog delay's do rather than repeating identically forever. Delay time is smoothed, so dragging
// Time pitch-bends the repeats the way a real delay does.
//
// Time is in milliseconds and is ignored while Sync names a note division; the knob stays live and
// keeps its value, so unsyncing returns to where the player left it. The tempo comes from the
// host's ProcessContext and falls back to free-running when the host supplies none.
#pragma once

#include "pedal.h"

namespace Rations
{
namespace pedals
{

class Delay final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kTime = 1, kFeedback = 2, kTone = 3, kMix = 4, kSync = 5, kPingPong = 6 };

    // The host's tempo, for the sync divisions. Zero or negative means the host supplied none,
    // and the free-running time is used instead — which is also what happens when Sync is "Free".
    void setTempo(double bpm) { mTempoBpm = bpm; }

    void setParams(const double *plain) override
    {
        mTimeMs = plain[kTime];
        mFeedback = plain[kFeedback];
        mTone = plain[kTone];
        mMix = plain[kMix];
        mSync = static_cast<int>(plain[kSync]);
        mPingPong = plain[kPingPong] > 0.5;
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
        // A pass-through until P6. The chain, the parameters, the state blob and the panel are
        // wired first and proved to change nothing, so that when the DSP lands the only thing that
        // has changed is the DSP.
        (void)l;
        (void)r;
        (void)numSamples;
    }

private:
    double mTimeMs = 400.0, mFeedback = 35.0, mTone = 5.0, mMix = 30.0;
    int mSync = 0;
    bool mPingPong = false;
    double mTempoBpm = 0.0;
};

} // namespace pedals
} // namespace Rations
