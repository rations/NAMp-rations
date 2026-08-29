// Rations — the Boost pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// An Ibanez TS-9 Tube Screamer, modelled from David T. Yeh's analysis of the circuit (Digital
// Implementation of Musical Distortion Circuits by Analysis and Simulation, Stanford CCRMA, 2009,
// section 2.4 — in third_party/refs/pedals). Four stages: two input high-passes at 15.9 and
// 15.6 Hz, a non-inverting op-amp whose antiparallel diode pair sits in the FEEDBACK loop, the
// second-order tone stage of eq. 2.14, and an output attenuator.
//
// Two facts about this circuit shape everything and neither is obvious. It is not a clipper in
// series with the signal: the clipped copy is SUMMED with the clean input (eq. 2.13), which is
// most of why it reads as a boost rather than a distortion. And the clipping stage is not a
// memoryless curve but a first-order nonlinear ODE (eq. 2.12), because the 51 pF feedback
// capacitor is state — its linear pole moves from 61 kHz down to 5.7 kHz as Drive is turned up,
// so the pedal darkens as it is driven.
#pragma once

#include "pedal.h"

namespace Rations
{
namespace pedals
{

class Boost final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kDrive = 1, kTone = 2, kLevel = 3 };

    void setParams(const double *plain) override
    {
        mDrive = plain[kDrive];
        mTone = plain[kTone];
        mLevel = plain[kLevel];
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
        // A pass-through until P4. The chain, the parameters, the state blob and the panel are
        // wired first and proved to change nothing, so that when the DSP lands the only thing that
        // has changed is the DSP.
        (void)l;
        (void)r;
        (void)numSamples;
    }

private:
    double mDrive = 5.0, mTone = 5.0, mLevel = 5.0;
};

} // namespace pedals
} // namespace Rations
