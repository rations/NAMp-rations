// Rations — the pedalboard: two chains, five pedals, one parameter array.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// PRE is mono and runs between the noise gate's trigger and the resampler; POST is stereo and runs
// after the cabinet. The order within each is fixed and is the order a real rig is wired in:
//
//     guitar -> gate -> Boost -> Chorus -> [amp, cab] -> Flanger -> Delay -> Reverb -> out
//
// WHY PRE IS NOT INSIDE ChannelRack, which is the one placement decision here that could have gone
// wrong quietly. The prime worker replays a ring of the last receptive field of input into the
// three idle channels, and the switch's 1e-6 convergence proof is a proof about that input. A pedal
// UPSTREAM of the resampler is captured by the ring as ordinary signal history, so the proof is
// untouched. A pedal INSIDE the rack would sit between the ring and the models, and every knob move
// on it would make all four channels cold.
#pragma once

#include "boost.h"
#include "chorus.h"
#include "delay.h"
#include "flanger.h"
#include "pedal.h"
#include "reverb.h"
#include "../rationsids.h"

namespace Rations
{
namespace pedals
{

class PedalChain
{
public:
    // The only allocating call, from setupProcessing. Every pedal sizes its buffers here.
    void prepare(double sampleRate, int maxBlock);
    void reset();

    // The whole denormalized parameter array, in kPedalParams order, pushed once per sub-block
    // from the audio thread. Each pedal is handed its own slice; the first entry of every slice is
    // that pedal's footswitch, which is why the engage state needs no special case.
    void setParams(const double *plain);

    // Host tempo, for the Delay's sync divisions. Zero or negative means the host supplied none,
    // and the Delay falls back to its free-running time.
    void setTempo(double bpm) { mTempoBpm = bpm; }

    void processPre(DSP_SAMPLE *mono, int numSamples);
    void processPost(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples);

    // True while any pedal in that chain is engaged or still ramping out. The processor uses it to
    // skip the stereo tail entirely when the board is empty, which is the common case and the one
    // that has to cost nothing.
    bool preActive() const;
    bool postActive() const;

private:
    Boost mBoost;
    Chorus mChorus;
    Flanger mFlanger;
    Delay mDelay;
    Reverb mReverb;

    // Indexed by PedalIndex, so a loop over kPedalCount reaches them in table order.
    Pedal *mAll[kPedalCount] = {&mBoost, &mChorus, &mFlanger, &mDelay, &mReverb};
    double mTempoBpm = 0.0;
};

} // namespace pedals
} // namespace Rations
