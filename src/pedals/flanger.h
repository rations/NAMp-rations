// Rations — the Flanger pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// A feedforward comb filter whose delay is swept, with feedback around the same delay line. This
// is where the POST chain widens to stereo: the two channels run the same LFO 180 degrees apart.
//
// THE STRUCTURE IS JULIUS O. SMITH III'S, *Physical Audio Signal Processing*, chapter 5. Copies of
// every page cited are in third_party/refs/pedals/pasp/.
//
//   "Flanging" (eq. 5.1)      y(n) = x(n) + g*x[n-M(n)], "modeled quite accurately as a
//   Flanging.html             feedforward comb filter ... in which the delay M is varied over
//                             time". And the fact this pedal is measured against: "The notches are
//                             thus spaced at intervals of fs/M Hz ... the notch spacing is
//                             inversely proportional to delay-line length."
//
//   "Flanger Speed and        M(n) = M0*[1 + A*sin(2*pi*f*n*T)] — f the speed, A the excursion, M0
//   Excursion"                the average delay. Manual sets M0, Depth sets A, Rate sets f. The
//                             excursion is a RATIO of M0 rather than a fixed number of
//                             milliseconds, which is Smith's own form and is also the musical one:
//                             notch spacing goes as 1/M, so a fixed ratio sweeps a fixed interval
//                             wherever Manual is set.
//
//   "Flanger Depth Control"   g = 1 for maximum effect, g = 0 for none.
//
//   "Flanger Inverted Mode"   g = -1 trades the peaks and notches. "In practice, the depth control
//                             g is usually constrained to the interval [0,1], and a sign inversion
//                             for g is controlled separately."
//
//   "Flanger Feedback         "Many modern commercial flangers have a control knob labeled
//   Control"                  'feedback' or 'regen.' This control sets the level of feedback from
//                             the output to the input of the delay line, thereby creating a
//                             feedback comb filter in addition to the feedforward comb filter."
//
// TWO DEPARTURES FROM THE EQUATIONS AS WRITTEN, both deliberate.
//
// The output is 0.5*(x + d) rather than eq. 5.1's x + g*x[n-M] with g fixed at 1. The equation's
// form peaks at +6 dB, and this pedal has no output level control, so engaging it would be a
// 6 dB jump. Half of the sum is also the physical picture Smith opens the chapter with — two tape
// machines playing the same tape "with their outputs added together (mixed equally)", Fig. 5.2 —
// so the peaks land at unity and the notches are still true nulls. Being quieter than bypass on
// average is not a defect: a flanger removes energy at its notches, and that is the effect.
//
// The sign lives on REGEN, not on g. Smith puts the inversion on the depth control and says it is
// usually a separate switch; the panel here has a signed Regen knob and no switch, and mda's
// ThruZero (MIT, in the SDK tree) maps its own feedback control the same signed way. A negative
// regen inverts the feedback comb's peaks into notches, which is the same trade of peaks for
// notches the inverted mode describes, reached with one control instead of two.
//
// The parameter RANGES follow the same two references: +-0.95 on regen is mda ThruZero's
// `1.9*p - 0.95`, and 0.5-8 ms of delay is the span DaisySP's flanger sweeps (MIT,
// third_party/DaisySP/Source/Effects/flanger.cpp, ".1 to 7 ms" plus its one-sample offset).
#pragma once

#include "pedal.h"
#include "primitives.h"

// WDL, for the feedback path's denormal flush. See NOTICE.
#include "wdl/denormal.h"

namespace Rations
{
namespace pedals
{

namespace flangerdef
{
// The span Manual sweeps M0 over. Below about 0.5 ms the first notch is above 1 kHz and the comb
// is too sparse to read as flanging; above about 8 ms the notches are dense enough to read as an
// echo instead.
inline constexpr double kMinMs = 0.5;
inline constexpr double kMaxMs = 8.0;

// Excursion at Depth 100 %, as a fraction of M0 — Smith's `A`. Stops short of 1.0 because A = 1
// would take the delay to zero at the bottom of every sweep, where there is no comb at all and no
// interpolator has anything to read.
inline constexpr double kMaxExcursion = 0.9;

// The right channel's LFO offset, in turns. Antiphase: when the left channel's notches are at
// their widest the right channel's are at their narrowest, which is what makes one mono signal
// arrive as two different ones.
inline constexpr double kRightPhase = 0.5;

// The largest regen either way. mda ThruZero's own limit, and it is a stability bound as much as a
// taste one: the loop gain is |regen| times the interpolator's magnitude response, which for
// Catmull-Rom never exceeds unity, so |regen| < 1 decays and |regen| = 1 would not.
inline constexpr double kMaxRegen = 0.95;

inline constexpr double kSmoothSec = 0.020;

// As in chorus.h: the paragraphs above are only true while these hold.
static_assert(kMinMs > 0.0 && kMinMs < kMaxMs, "Manual's span runs the wrong way");
static_assert(kMaxExcursion > 0.0 && kMaxExcursion < 1.0,
              "A = 1 takes the delay to zero at the bottom of every sweep, where there is no comb "
              "and nothing for the interpolator to read");
static_assert(kMaxRegen > 0.0 && kMaxRegen < 1.0,
              "the loop gain is |regen| times an interpolator whose magnitude response never "
              "exceeds one, so |regen| < 1 is what makes the tail decay");
static_assert(kRightPhase > 0.0 && kRightPhase < 1.0,
              "the two channels differ only by this offset; at zero the POST chain is not stereo");
} // namespace flangerdef

class Flanger final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kRate = 1, kDepth = 2, kManual = 3, kRegen = 4 };

    void setParams(const double *plain) override
    {
        const double rate = plain[kRate];
        const double depth = plain[kDepth] * 0.01 * flangerdef::kMaxExcursion;
        // Manual is a position along the delay span, not a time: the knob reads 0-100 %.
        const double manual = flangerdef::kMinMs
                              + (flangerdef::kMaxMs - flangerdef::kMinMs) * plain[kManual] * 0.01;
        const double regen =
            std::clamp(plain[kRegen] * 0.01, -flangerdef::kMaxRegen, flangerdef::kMaxRegen);

        // The first push after a prepare or a reset lands; every one after it sweeps. See the same
        // note in chorus.h.
        if (mPrimed) {
            mRateHz.setTarget(rate);
            mDepth.setTarget(depth);
            mManualMs.setTarget(manual);
            mRegen.setTarget(regen);
        } else {
            mRateHz.snap(rate);
            mDepth.snap(depth);
            mManualMs.snap(manual);
            mRegen.snap(regen);
            mPrimed = true;
        }
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;
        mMsToSamples = sampleRate * 0.001;
        // The longest delay ever asked for is M0 at its maximum, swept to its longest.
        const int maxDelay =
            static_cast<int>(std::ceil(flangerdef::kMaxMs * (1.0 + flangerdef::kMaxExcursion)
                                       * mMsToSamples))
            + 4;
        for (int c = 0; c < 2; ++c)
            mLine[c].prepare(maxDelay);
        mLfo.prepare(sampleRate);

        // Coefficients only; the values are set by the first setParams, which is always called
        // before the first block of audio.
        mRateHz.prepare(sampleRate, flangerdef::kSmoothSec, mRateHz.target());
        mDepth.prepare(sampleRate, flangerdef::kSmoothSec, mDepth.target());
        mManualMs.prepare(sampleRate, flangerdef::kSmoothSec, mManualMs.target());
        mRegen.prepare(sampleRate, flangerdef::kSmoothSec, mRegen.target());
    }

    void resetImpl() override
    {
        for (int c = 0; c < 2; ++c)
            mLine[c].reset();
        mLfo.reset();
        mPrimed = false;
    }

    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        // POST is stereo, but the base allows a null right channel and the contract has to hold:
        // with one channel there is one delay line and no antiphase to take.
        const int channels = r ? 2 : 1;

        for (int i = 0; i < numSamples; ++i) {
            mLfo.setRate(mRateHz.next());
            const double m0 = mManualMs.next() * mMsToSamples;
            const double a = mDepth.next();
            const double regen = mRegen.next();

            for (int c = 0; c < channels; ++c) {
                // Smith's M(n) = M0*[1 + A*sin(...)], with a triangle in place of the sine: the
                // audible object here is the NOTCH POSITION, which follows the delay directly, and
                // a triangle sweeps it evenly where a sine dwells at the turning points. See the
                // converse argument in chorus.h.
                const double phase = (c == 0) ? 0.0 : flangerdef::kRightPhase;
                const double m = m0 * (1.0 + a * mLfo.triangleAt(phase));

                const double x = (c == 0) ? l[i] : r[i];
                // READ ONE SHORT, and this is not an adjustment — it is what makes the knob mean
                // what it says. The line has to be read before this sample is written to it,
                // because the value written depends on what was read, so a read at M returns the
                // sample from M+1 ago. Left uncorrected the feedforward path is x[n-M-1] and the
                // notches come out at fs/(M+1) — measurably, and by exactly one sample: the gate
                // read 97 where the circuit says 96 and 193 where it says 192, and the notch
                // spacing followed it to 494.9 Hz against 500. Reading at M-1 makes the
                // feedforward path exactly x[n-M] and the feedback loop exactly M samples round.
                const double d = mLine[c].read(m - 1.0);
                // Feedback comb around the same line as the feedforward comb — PASP's
                // "Flanger Feedback Control". Flushed rather than left to underflow: this loop can
                // ring for seconds at high regen, and a denormal tail costs a hundred times what
                // the audio does.
                double back = x + regen * d;
                denormal_fix_double(&back);
                mLine[c].write(back);

                // The OUTPUT is flushed as well as the loop, and the second one is not
                // redundant. Flushing `back` stops the delay line ever holding a subnormal, but
                // the line still holds ordinary values just above the subnormal floor for one
                // delay's worth of samples after the loop has bottomed out, and half of one of
                // those is subnormal. Two consequences, neither hypothetical: those samples are
                // what the Delay and the Reverb downstream are fed, and they would seed a
                // subnormal into the next feedback loop along; and this pedal must not depend on
                // the audio thread's FTZ/DAZ for correctness, because the same DSP is reachable
                // from tools and tests that have not armed it — which is how this was found.
                double y = 0.5 * (x + d);
                denormal_fix_double(&y);
                if (c == 0)
                    l[i] = y;
                else
                    r[i] = y;
            }
            mLfo.advance();
        }
    }

private:
    FracDelay mLine[2];
    Lfo mLfo;
    Smoothed mRateHz, mDepth, mManualMs, mRegen;
    double mMsToSamples = 48.0;
    bool mPrimed = false;
};

} // namespace pedals
} // namespace Rations
