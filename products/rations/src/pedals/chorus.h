// Rations — the Chorus pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Two interpolating taps on one modulated delay line, no feedback. Mono, because it sits in the
// PRE chain feeding one mono amp head — which is what a chorus in front of an amp physically is.
//
// THE STRUCTURE is the one Julius O. Smith III gives for the effect in *Physical Audio Signal
// Processing*, "Chorus Effect": "An efficient chorus-effect implementation may be based on
// multiple interpolating taps working on a single delay line. The taps oscillate back and forth
// about the positions they would have while implementing a fixed tapped delay line."
// https://ccrma.stanford.edu/~jos/pasp/Chorus_Effect.html — a copy is in
// third_party/refs/pedals/pasp/. Smith adds that each tap should be individually spatialized; that
// half does not apply here, because this pedal is upstream of a mono amp and there is nowhere to
// pan to. The taps are summed.
//
// The topology reference for the code shape is DaisySP's `ChorusEngine` (MIT,
// third_party/DaisySP/Source/Effects/chorus.cpp), which is two engines summed with an equal
// dry/wet mix. Two things here are deliberately NOT DaisySP's: its two engines run on the same LFO
// phase unless the host sets them apart, so by default they are one voice counted twice; and its
// delay line interpolates linearly (see primitives.h for why this one does not).
//
// A SINE LFO, where the Flanger uses a triangle, and the reason is what the ear is listening to in
// each case. Delay modulation shifts pitch by the DERIVATIVE of the delay, so a triangle — whose
// derivative is a square wave — detunes by a constant amount, flips sign abruptly, and holds. That
// is the sound of a swept comb, which is the flanger's business. A sine's derivative is a sine, so
// the pitch wavers smoothly, which is the sound of several players not quite in unison, which is
// this pedal's business. Both shapes are named as ordinary choices for a delay-modulating LFO by
// PASP's "Flanger Speed and Excursion"; which of them goes where is a decision taken here.
#pragma once

#include "pedal.h"
#include "primitives.h"

namespace Rations
{
namespace pedals
{

namespace chorusdef
{
// The two taps' rest positions. Both are inside the 5-30 ms a chorus works over: below about 5 ms
// the effect stops being a chorus and starts being a flanger (a comb sparse enough to hear as
// pitch), and above about 30 ms it stops being a chorus and starts being an audible slap.
inline constexpr double kTap0Ms = 10.0;
inline constexpr double kTap1Ms = 18.0;

// Excursion at Depth 100 %, either side of the rest position. Bounded by the SEPARATION of the two
// taps and not by the range above: the taps run in quadrature, so their spacing is
// (kTap1Ms - kTap0Ms) +- sweep*sqrt(2), and a sweep large enough to let them cross would make the
// two voices briefly identical — which sums to +6 dB and audibly pumps once per LFO cycle. At 4 ms
// the closest they ever come is 2.34 ms, which is a comb notch at 214 Hz and not a collision.
inline constexpr double kSweepMs = 4.0;

// The second tap's LFO offset, in turns. Quadrature.
inline constexpr double kTap1Phase = 0.25;

// How fast Rate, Depth and Mix follow their knobs. Depth and Mix must be smoothed because both
// scale something per sample — a step in Depth is a step in the delay, which is a click, and a step
// in Mix is a step in level. Rate needs it least, because LFO phase is continuous across a rate
// change and so a rate step cannot click; it is smoothed anyway so that a dragged Rate sweeps
// rather than jumps.
inline constexpr double kSmoothSec = 0.020;

// The prose above makes three claims about these four numbers. They are cheap to state as build
// errors, and a claim that is only in a comment is a claim that stops being true silently.
static_assert(kTap0Ms - kSweepMs >= 5.0 && kTap1Ms + kSweepMs <= 30.0,
              "a tap leaves the 5-30 ms a chorus works over at full Depth");
static_assert(kTap1Ms - kTap0Ms > kSweepMs * 1.4143,
              "in quadrature the taps' spacing is (kTap1Ms - kTap0Ms) +- kSweepMs*sqrt(2); this "
              "sweep lets them cross, which sums two identical voices to +6 dB once per cycle");
static_assert(kTap1Phase > 0.0 && kTap1Phase < 1.0, "the second tap's offset is a phase in turns");
} // namespace chorusdef

class Chorus final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kRate = 1, kDepth = 2, kMix = 3 };

    void setParams(const double *plain) override
    {
        const double rate = plain[kRate];
        const double depth = plain[kDepth] * 0.01;
        const double mix = plain[kMix] * 0.01;
        // The FIRST push after a prepare or a reset lands; every one after it sweeps. A pedal
        // coming out of reset has no history to be discontinuous with, so smoothing there would
        // only mean the first 20 ms sound like a knob being turned; a push while audio is running
        // is a host moving a control, and that one must not step.
        if (mPrimed) {
            mRateHz.setTarget(rate);
            mDepth.setTarget(depth);
            mMix.setTarget(mix);
        } else {
            mRateHz.snap(rate);
            mDepth.snap(depth);
            mMix.snap(mix);
            mPrimed = true;
        }
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;
        const double msToSamples = sampleRate * 0.001;
        mTap0 = chorusdef::kTap0Ms * msToSamples;
        mTap1 = chorusdef::kTap1Ms * msToSamples;
        mSweep = chorusdef::kSweepMs * msToSamples;

        mLine.prepare(static_cast<int>(std::ceil(mTap1 + mSweep)) + 4);
        mLfo.prepare(sampleRate);

        // Land the smoothers on their targets rather than sweeping up to them from zero the first
        // time audio arrives.
        // Coefficients only; the values themselves are set by the first setParams, which is
        // always called before the first block of audio.
        mRateHz.prepare(sampleRate, chorusdef::kSmoothSec, mRateHz.target());
        mDepth.prepare(sampleRate, chorusdef::kSmoothSec, mDepth.target());
        mMix.prepare(sampleRate, chorusdef::kSmoothSec, mMix.target());
    }

    void resetImpl() override
    {
        mLine.reset();
        mLfo.reset();
        mPrimed = false;
    }

    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        (void)r; // PRE is mono: it feeds one mono amp head, which is what a real rig does.

        for (int i = 0; i < numSamples; ++i) {
            const double x = l[i];
            mLine.write(x);

            mLfo.setRate(mRateHz.next());
            const double sweep = mSweep * mDepth.next();
            const double a = mLine.read(mTap0 + sweep * mLfo.sineAt(0.0));
            const double b = mLine.read(mTap1 + sweep * mLfo.sineAt(chorusdef::kTap1Phase));
            mLfo.advance();

            // Half, not the sum: two voices summed at unity would be +6 dB wherever they happen to
            // agree, and a pedal with no output level control must not do that.
            const double wet = 0.5 * (a + b);
            const double mix = mMix.next();
            l[i] = (1.0 - mix) * x + mix * wet;
        }
    }

private:
    FracDelay mLine;
    Lfo mLfo;
    Smoothed mRateHz, mDepth, mMix;
    double mTap0 = 0.0, mTap1 = 0.0, mSweep = 0.0;
    bool mPrimed = false;
};

} // namespace pedals
} // namespace Rations
