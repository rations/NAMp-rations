// Rations — the pedalboard's shared primitives: an LFO, a fractional delay line, a smoother.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Three small pieces that the Chorus, the Flanger and (later) the Delay all need. They live here
// rather than in one of those files because the alternative — a `ModDelay` class general enough to
// be both a chorus and a flanger — would be a worse abstraction than the two short loops it
// replaced: the Chorus is two taps on ONE line with no feedback, the Flanger is one tap per channel
// on its OWN line WITH feedback, and a class covering both is mostly branches on which one it is.
//
// The primitives are shared; the topologies are not.
//
// REAL-TIME CONTRACT: only prepare() allocates. reset(), and everything on the per-sample path,
// is arithmetic on already-owned memory.
#pragma once

// AudioDSPTools, for _CubicInterpolation. Qualified with its directory because
// NeuralAmpModelerCore has headers of the same name and its include path is searched first.
#include "dsp/Resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Rations
{
namespace pedals
{

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

//------------------------------------------------------------------------------------------------
// A low-frequency oscillator, unit amplitude, phase measured in TURNS rather than radians.
//
// Turns, because every use of it here asks for the value at some fixed offset from the running
// phase — the Chorus's two taps in quadrature, the Flanger's two channels in antiphase — and an
// offset expressed as 0.25 or 0.5 is checkable by eye where pi/2 and pi are not. It also makes the
// wrap a subtraction of exactly 1.0 rather than of an irrational number, so the phase cannot drift
// by accumulating a rounded 2*pi.
//
// Both shapes are provided because the two pedals want different ones, and which one each wants
// follows from what the ear is actually listening to. See the note in chorus.h.
//
// Julius O. Smith III, *Physical Audio Signal Processing*, "Flanger Speed and Excursion":
// the delay-line length is modulated as M(n) = M0*[1 + A*sin(2*pi*f*n*T)], and the oscillator
// waveform "is usually triangular, sinusoidal, or exponential".
// https://ccrma.stanford.edu/~jos/pasp/Flanger_Speed_Excursion.html — a copy is in
// third_party/refs/pedals/pasp/.
class Lfo
{
public:
    void prepare(double sampleRate)
    {
        mInvRate = 1.0 / sampleRate;
        setRate(mRateHz);
        reset();
    }

    void reset() { mPhase = 0.0; }

    void setRate(double hz)
    {
        mRateHz = hz;
        mInc = hz * mInvRate;
    }

    // One sample of phase. Wrapped with floor() rather than a single subtraction so that a rate
    // high enough to advance more than a whole turn per sample still wraps correctly instead of
    // walking off — which cannot happen at the rates on the panel, but is one line to make
    // impossible rather than to argue about.
    void advance()
    {
        mPhase += mInc;
        if (mPhase >= 1.0 || mPhase < 0.0)
            mPhase -= std::floor(mPhase);
    }

    double phase() const { return mPhase; }

    // Value at `offsetTurns` ahead of the running phase. 0.25 is quadrature, 0.5 antiphase.
    double sineAt(double offsetTurns) const { return std::sin(kTwoPi * (mPhase + offsetTurns)); }
    double triangleAt(double offsetTurns) const { return triangle(mPhase + offsetTurns); }

    // Unit triangle: 0 at phase 0, +1 at 0.25, 0 at 0.5, -1 at 0.75. Continuous everywhere, so a
    // phase offset costs nothing and the delay it modulates never steps.
    static double triangle(double turns)
    {
        const double q = turns + 0.25;
        return 1.0 - 4.0 * std::fabs(q - std::floor(q) - 0.5);
    }

private:
    double mInvRate = 1.0 / 48000.0;
    double mRateHz = 1.0;
    double mInc = 0.0;
    double mPhase = 0.0;
};

//------------------------------------------------------------------------------------------------
// A circular delay line read at a fractional position by Catmull-Rom cubic interpolation.
//
// The interpolator is AudioDSPTools' own `dsp::_CubicInterpolation`, which is already a dependency
// of this plug-in — ported rather than written, per the project's rules. Signalsmith's
// `InterpolatorCubic` (third_party/signalsmith-dsp/delay.h) is the same polynomial written a
// different way, and is the reference this was checked against.
//
// WHY CUBIC AND NOT LINEAR, which is what both of the structural references for these two pedals
// use (DaisySP's DelayLine::Read, and mda's ThruZero): linear interpolation of a delay line is a
// lowpass whose corner depends on the FRACTIONAL part of the delay, so a delay that is being
// modulated drags a moving lowpass across the signal. That is inaudible in a chorus and audible in
// a flanger, whose whole business is the depth of a notch at high frequency. The cost of the
// choice is measured rather than assumed — see rations_pedalcheck's interpolator comparison, which
// reports both.
class FracDelay
{
public:
    // The only allocating call. `maxDelaySamples` is the largest delay that will ever be asked
    // for; the buffer is rounded up to a power of two so the wrap is a mask.
    void prepare(int maxDelaySamples)
    {
        // +3 for the cubic kernel's reach past the read point, and never smaller than 8 so a
        // degenerate request still leaves a legal buffer.
        size_t want = static_cast<size_t>(std::max(8, maxDelaySamples + 3));
        size_t size = 8;
        while (size < want)
            size <<= 1;
        mBuf.assign(size, 0.0);
        mMask = size - 1;
        mWrite = 0;
    }

    void reset()
    {
        std::fill(mBuf.begin(), mBuf.end(), 0.0);
        mWrite = 0;
    }

    void write(double x)
    {
        mBuf[mWrite] = x;
        mWrite = (mWrite + 1) & mMask;
    }

    // The largest delay this line can be read at. Callers clamp against it rather than the line
    // clamping silently, because a delay that was quietly shortened is a pedal that quietly
    // stopped doing what its knob says.
    double maxDelay() const { return static_cast<double>(mBuf.size()) - 3.0; }

    // The shortest legal delay. One sample, because the cubic kernel reaches one sample NEWER than
    // the read point and x[n] is the newest sample there is.
    static constexpr double kMinDelay = 1.0;

    double read(double delaySamples) const
    {
        const double d = std::clamp(delaySamples, kMinDelay, maxDelay());
        const int i = static_cast<int>(d);
        const double f = d - static_cast<double>(i);
        // Index runs backwards in time, so p[1] is the sample at the read point's integer part and
        // p[2] is one sample OLDER. _CubicInterpolation places p[1] at x=0 and p[2] at x=1, so the
        // parameter is the fractional part directly. Catmull-Rom is symmetric under this
        // reversal — it is the same polynomial read the other way round.
        double p[4] = {at(i - 1), at(i), at(i + 1), at(i + 2)};
        return dsp::_CubicInterpolation(p, f);
    }

    // Linear read, for the interpolator comparison in rations_pedalcheck. Not on any audio path.
    double readLinear(double delaySamples) const
    {
        const double d = std::clamp(delaySamples, kMinDelay, maxDelay());
        const int i = static_cast<int>(d);
        const double f = d - static_cast<double>(i);
        const double a = at(i), b = at(i + 1);
        return a + f * (b - a);
    }

private:
    // x[n-k], where x[n] is the most recently written sample.
    double at(int k) const
    {
        return mBuf[(mWrite - 1 - static_cast<size_t>(k)) & mMask];
    }

    std::vector<double> mBuf;
    size_t mMask = 0;
    size_t mWrite = 0;
};

//------------------------------------------------------------------------------------------------
// A one-pole parameter smoother, stepped once per sample.
//
// Per sample rather than the Boost's once-per-block-plus-linear-ramp, because these pedals' knobs
// set a DELAY TIME rather than a filter coefficient: a delay that steps is a click, and a delay
// that ramps is a pitch bend, which is what the hardware does when its knob is turned. A one-pole
// has no step anywhere by construction, so there is no zipper to tune away.
//
// It lands EXACTLY on its target rather than approaching it forever. A one-pole is asymptotic, and
// a delay time left a hair off its nominal value would make every measurement in the gate a
// measurement of the smoother; snapping when the remaining distance is below kSnap costs one
// comparison and makes "the knob says 4.0 ms" true rather than nearly true. Same reasoning as the
// crossfade ramps' snap to exact 0.0 and 1.0.
class Smoothed
{
public:
    void prepare(double sampleRate, double tauSeconds, double initial)
    {
        mAlpha = 1.0 - std::exp(-1.0 / (std::max(1.0e-9, tauSeconds) * sampleRate));
        snap(initial);
    }

    void setTarget(double v) { mTarget = v; }
    void snap(double v) { mCur = mTarget = v; }

    double next()
    {
        const double d = mTarget - mCur;
        if (std::fabs(d) < kSnap)
            mCur = mTarget;
        else
            mCur += d * mAlpha;
        return mCur;
    }

    double current() const { return mCur; }
    double target() const { return mTarget; }

private:
    static constexpr double kSnap = 1.0e-12;

    double mAlpha = 1.0;
    double mCur = 0.0;
    double mTarget = 0.0;
};

} // namespace pedals
} // namespace Rations
