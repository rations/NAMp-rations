// Rations — the Boost pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// An Ibanez TS-9 Tube Screamer. Every component value, every equation and both diode parameters
// below are read out of David T. Yeh, "Digital Implementation of Musical Distortion Circuits by
// Analysis and Simulation" (Stanford CCRMA, 2009), section 2.4 — the PDF and a text extract of
// that section are in third_party/refs/pedals, and each constant names the equation or figure it
// comes from. Nothing here is from recollection of how a Tube Screamer works.
//
// TWO FACTS SHAPE EVERYTHING AND NEITHER IS OBVIOUS.
//
// It is NOT a clipper in series with the signal. The clipped copy is SUMMED with the clean input
// (eq. 2.13, V_o = V + V_i), and that summation is most of why the pedal reads as a boost rather
// than as a distortion: however hard the clipped path limits, the clean path goes straight through
// underneath it. A series clipper would not sound like this however well the diodes were modelled.
//
// And the clipping stage is NOT a memoryless curve. The 51 pF feedback capacitor is state, so
// eq. 2.12 is a first-order nonlinear ODE, and its linear pole 1/(R2·Cc) MOVES WITH DRIVE — 61 kHz
// with the pot down, 5.7 kHz with it up — so the pedal darkens as it is driven. A waveshaper
// throws that away, which is why this solves the ODE instead.
#pragma once

#include "pedal.h"

// The non-finite test the Newton guard below uses. Not <cmath>'s: see the header for why.
#include "finite.h"

// Two vendored headers, and they do NOT share a namespace: Filters.h declares bbm, Oversampler.h
// declares bbmh. Both are byte-identical to their originals (see NOTICE), so the inconsistency is
// theirs and is left alone rather than tidied - a local edit there would be reverted by the next
// re-vendor and would break the byte-identity check that makes the provenance claim checkable.
#include "bbm/Filters.h"
#include "bbm/Oversampler.h"

#include <cmath>
#include <vector>

namespace Rations
{
namespace pedals
{

// The circuit. Yeh section 2.4; the figure or equation each group belongs to is named.
namespace ts9
{
// 2.4.1 — the two input high-passes.
inline constexpr double kInHp1Hz = 15.9;
inline constexpr double kInHp2Hz = 15.6;

// 2.4.2, Fig. 2.25 and eq. 2.10 — the leg from the input to the op amp's summing node. R1 with Cz
// high-passes the CLIPPED path at 1/(R1·Cz) = 720.5 Hz, which is the famous Tube Screamer bass cut
// and is why it stays tight into a driven amp. The clean path of eq. 2.13 keeps its bass.
inline constexpr double kR1 = 4.7e3;
inline constexpr double kCz = 47e-9;

// 2.4.2, eq. 2.12 — the feedback network. Cc is the state that makes this an ODE; R2 is the Drive
// pot in series with a fixed 51k, so it never reaches zero and the stage always has a finite
// linear gain.
inline constexpr double kCc = 51e-12;
inline constexpr double kR2Fixed = 51e3;
inline constexpr double kR2Drive = 500e3;

// The antiparallel diode pair, as 2·Is·sinh(V/Vt). These two numbers are Yeh's own extracted
// device parameters (eq. 3.14 and the paragraph under eq. 3.15, "I_s = 2.52 nA, and V_T =
// 45.3 mV"), and they carry over to this circuit because section 2.4.2 says so in as many words:
// "this ODE is the same as that for the Distortion pedal, (3.15)". Vt is the EFFECTIVE thermal
// voltage — it already carries the ideality factor, which is why it is 45.3 mV and not the 25.9 mV
// of the bare physical constant. Do not "correct" it.
inline constexpr double kIs = 2.52e-9;
inline constexpr double kVt = 45.3e-3;

// 2.4.3, Fig. 2.26 and eq. 2.14 — the tone stage, a second-order section with variable
// coefficients whose character is essentially low-pass (Fig. 2.27).
inline constexpr double kToneRs = 1e3;
inline constexpr double kToneCs = 0.22e-6;
inline constexpr double kToneRi = 10e3;
inline constexpr double kToneRf = 1e3;
inline constexpr double kTonePot = 20e3;
inline constexpr double kToneRz = 220.0;
inline constexpr double kToneCz = 0.22e-6;

// Fig. 2.26, output — a 100k LEVEL pot fed through 1k.
inline constexpr double kLevelPot = 100e3;
inline constexpr double kLevelSeries = 1e3;

// The pot's ends are approached but never reached, exactly as Yeh's own Fig. 2.27 plots them
// (t = 0.0001 and t = 0.9999): eq. 2.14's Rl and Rr are a pot's two halves, and both endpoints
// collapse a term the algebra assumes is present.
inline constexpr double kPotEnd = 1.0e-4;

// FULL SCALE IS ONE VOLT, and that is a calibration rather than a convenience. Yeh verified this
// model against a real TS-9 "for a 220 Hz sine signal with amplitude of 100 mV" (section 2.4.4),
// which is also what a guitar pickup actually delivers — so under this convention his test signal
// is a nominal −20 dBFS and a player's normal working level lands where the circuit was measured.
// A hotter input clips harder, which is what the real pedal does too; the plug-in's own input gain
// sits ahead of the whole board for exactly that reason.
inline constexpr double kFullScaleVolts = 1.0;

// The stiff term is bounded so that a transient can never overflow the junction and poison the
// signal — the technique, and the reason for it, are the author's own DiodeClipper.h in
// bigpimuffvst3. 40 is V/Vt at 1.8 V, far past anything this circuit reaches, so the true sinh is
// used everywhere that matters and the linear extension beyond it exists only to keep Newton
// monotonic and finite.
inline constexpr double kExpMax = 40.0;
inline constexpr int kMaxNewtonIters = 8;
inline constexpr double kNewtonTolVolts = 1.0e-10;
// The largest step one Newton iteration may take. See solveClipper: this is what makes the
// iteration globally rather than only locally convergent, and it is in thermal volts because that
// is the scale the stiffness is set by.
inline constexpr double kMaxStepVolts = 8.0 * kVt;

// How fast Drive and Tone follow the knob. They set filter coefficients, which cannot be
// recomputed per sample, so they are recomputed once per block from a value that is smoothed —
// which bounds how far a coefficient can move in one step and is what keeps a dragged knob from
// zippering. Level is a plain gain and is ramped per sample instead.
inline constexpr double kControlSmoothSec = 0.030;
} // namespace ts9

class Boost final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over a
    // pointer to the start of that slice, so these index from zero.
    enum Param { kDrive = 1, kTone = 2, kLevel = 3 };

    // What the 4x oversampler costs, in base-rate samples, at DC. REPORTED WHETHER THE PEDAL IS
    // ENGAGED OR NOT — see RationsProcessor::setupProcessing. A latency that changed when a
    // footswitch was stomped would make the host recompute delay compensation mid-song, and some
    // hosts glitch when it moves; the half-band filters run either way, so the delay is there
    // either way.
    static constexpr double kLatencySamples = bbmh::Oversampler4x::kLatencySamples;

    void setParams(const double *plain) override
    {
        mDriveTarget = clamp01(plain[kDrive] * 0.1);
        mToneTarget = clampPot(plain[kTone] * 0.1);
        mLevelTarget = levelGain(clamp01(plain[kLevel] * 0.1));
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        mSampleRate = sampleRate;
        mOsRate = sampleRate * static_cast<double>(bbmh::Oversampler4x::kFactor);
        mHalfStep = 0.5 / mOsRate;

        mOs.prepare(static_cast<std::size_t>(maxBlock));
        mScratch.assign(static_cast<std::size_t>(maxBlock), 0.0f);

        // The two input high-passes are linear and well below Nyquist, so they run at the base
        // rate, before anything is upsampled.
        mInHp1.prepare(sampleRate, static_cast<float>(ts9::kInHp1Hz));
        mInHp2.prepare(sampleRate, static_cast<float>(ts9::kInHp2Hz));
        // The clipped path's own high-pass feeds the nonlinearity, so it belongs INSIDE the
        // oversampled region and runs at 4x.
        mClipHp.prepare(mOsRate, static_cast<float>(1.0 / (2.0 * kPi * ts9::kR1 * ts9::kCz)));

        // Land the smoothed controls on their targets rather than sweeping up to them from zero
        // the first time audio arrives.
        mDrive = mDriveTarget;
        mTone = mToneTarget;
        mLevel = mLevelTarget;
        updateDrive();
        updateTone();
    }

    void resetImpl() override
    {
        mOs.reset();
        mInHp1.reset();
        mInHp2.reset();
        mClipHp.reset();
        mV = 0.0;
        mDeriv = 0.0;
        mT1 = mT2 = 0.0;
    }

    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        (void)r; // PRE is mono: it feeds one mono amp head, which is what a real rig does.
        if (numSamples <= 0)
            return;
        const std::size_t n = static_cast<std::size_t>(numSamples);

        // One smoothing step and one coefficient rebuild per block. exp() once per block, never
        // per sample.
        const double alpha =
            1.0
            - std::exp(-(static_cast<double>(numSamples) / mSampleRate) / ts9::kControlSmoothSec);
        const double driveWas = mDrive;
        const double toneWas = mTone;
        mDrive += (mDriveTarget - mDrive) * alpha;
        mTone += (mToneTarget - mTone) * alpha;
        if (mDrive != driveWas)
            updateDrive();
        if (mTone != toneWas)
            updateTone();

        // Level is a scalar, so it gets the treatment a scalar deserves: a per-sample ramp across
        // the block, which cannot zipper at all.
        const double levelFrom = mLevel;
        mLevel += (mLevelTarget - mLevel) * alpha;
        const double levelStep = (mLevel - levelFrom) / static_cast<double>(numSamples);

        // Base rate: into volts, then the two input high-passes.
        for (std::size_t i = 0; i < n; ++i) {
            const float v = static_cast<float>(l[i] * ts9::kFullScaleVolts);
            mScratch[i] = mInHp2.processHighpass(mInHp1.processHighpass(v));
        }

        // 4x: the clipped path's high-pass, the ODE, eq. 2.13's summation AND the tone stage. The
        // first three have to be here because the sum is of the clean input with a signal that has
        // just had harmonics added to it.
        //
        // The TONE STAGE is here for a different reason, and it was moved in after being measured
        // at the base rate. It is linear, so it needs no oversampling for its own sake - but it is
        // bilinear-transformed from eq. 2.14, and at 48 kHz that transform warps 12 kHz to an
        // analog 15.3 kHz, which cost 1.9 dB of roll-off the circuit does not have. At 4x the same
        // corner warps by a quarter as much. It is also the more faithful place: in the real pedal
        // the tone stage is analog and sees the clipper's full bandwidth, and a low-pass ahead of
        // the decimator is content that never has to be filtered out again.
        float *up = mOs.processSamplesUp(mScratch.data(), n);
        if (!up)
            return; // more samples than prepare() was given; the base leaves the dry copy alone
        const std::size_t un = n * bbmh::Oversampler4x::kFactor;
        for (std::size_t i = 0; i < un; ++i) {
            const double vi = static_cast<double>(up[i]);
            const double in = static_cast<double>(mClipHp.processHighpass(up[i])) / ts9::kR1;
            up[i] = static_cast<float>(tone(solveClipper(in) + vi)); // eq. 2.13, then eq. 2.14
        }
        mOs.processSamplesDown(mScratch.data(), n);

        // Base rate again: the level pot, then back out of volts.
        double level = levelFrom;
        for (std::size_t i = 0; i < n; ++i) {
            level += levelStep;
            l[i] = static_cast<double>(mScratch[i]) * level / ts9::kFullScaleVolts;
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
    static double clampPot(double v)
    {
        return v < ts9::kPotEnd ? ts9::kPotEnd : (v > 1.0 - ts9::kPotEnd ? 1.0 - ts9::kPotEnd : v);
    }
    // Fig. 2.26: the wiper against the 1k that feeds it.
    static double levelGain(double t)
    {
        return t * ts9::kLevelPot / (ts9::kLevelPot + ts9::kLevelSeries);
    }

    // sinh and cosh with the exponent bounded, continuous and monotonic across the bound so Newton
    // cannot be sent the wrong way by the extension.
    static void sinhCoshBounded(double x, double &sh, double &ch)
    {
        const double ax = std::fabs(x);
        if (ax <= ts9::kExpMax) {
            sh = std::sinh(x);
            ch = std::cosh(x);
            return;
        }
        const double e = 0.5 * std::exp(ts9::kExpMax);
        const double v = e * (1.0 + (ax - ts9::kExpMax));
        sh = (x > 0.0) ? v : -v;
        ch = e;
    }

    // Eq. 2.12, discretised by the trapezoidal rule and solved by Newton:
    //
    //     dV/dt = f(V, In),  f = In/Cc − V/(R2·Cc) − (2·Is/Cc)·sinh(V/Vt)
    //     V − Vp = (T/2)·( f(V, In) + f(Vp, In_prev) )
    //
    // Implicit because the equation is stiff: past the diode knee the sinh term's derivative is
    // enormous, and an explicit step would need a rate far above 4x to stay stable. Warm-started
    // from the previous sample, which for an ODE is not a heuristic — V is continuous, and at
    // 4x the previous value is already within a whisker of the answer, so this converges in one
    // or two iterations.
    double solveClipper(double in)
    {
        const double drive = in * mInvCc;    // the input current term, on its own
        const double base = drive + mDeriv;  // plus the trapezoid's stored past term

        // THE INITIAL GUESS IS NOT OPTIONAL, and this is the one thing in the Boost that was got
        // wrong and found by measurement rather than by reading. Warm-starting from the previous
        // sample is the obvious choice for an ODE - V is continuous - and it is excellent once the
        // solver is tracking. It is catastrophic when it is not: at the first loud sample the
        // previous value is zero, the unclamped first Newton step throws v to about 3 V, and up
        // there the sinh is so stiff that Newton crawls back at roughly one thermal volt per
        // iteration. Eight iterations cannot cover 2.5 V, so v stays high, mDeriv is poisoned, and
        // the clipped path LATCHES OFF for good - leaving only eq. 2.13's clean path, which sounds
        // like a working pedal that has simply stopped distorting. It measured as a pure sine with
        // harmonics 150 dB down while the peak level still showed compression.
        //
        // So the quasi-static solution is computed too, from the same asinh bound the author's own
        // DiodeClipper.h uses: with the derivative ignored the current either goes through R2 or
        // through the diodes, and the answer is bounded by whichever limb binds - the resistor
        // below the knee, the diode pair above it. asinh is overflow-free, so this is close for
        // any drive. Newton then starts from whichever of the two candidates actually has the
        // smaller residual, which costs one extra evaluation and cannot be fooled by either
        // candidate being wrong.
        const double vRes = drive / mInvR2Cc;
        const double vDio = ts9::kVt * std::asinh(drive / mK2IsCc);
        const double vQs = std::fabs(vRes) < std::fabs(vDio) ? vRes : vDio;

        double v = (std::fabs(residual(mV, base)) <= std::fabs(residual(vQs, base))) ? mV : vQs;

        for (int it = 0; it < ts9::kMaxNewtonIters; ++it) {
            double sh, ch;
            sinhCoshBounded(v * mInvVt, sh, ch);
            const double f = v - mV - mHalfStep * (base - v * mInvR2Cc - mK2IsCc * sh);
            const double df = 1.0 + mHalfStep * (mInvR2Cc + mK2IsCc * ch * mInvVt);
            double step = f / df;
            // Damping. The residual is monotonic in v, so a bounded step converges globally
            // instead of merely locally, and near the root the bound never binds and Newton keeps
            // its quadratic convergence. This is the belt to the initial guess's braces: either
            // alone fixes the latch-off above, and both together cost almost nothing.
            const double lim = ts9::kMaxStepVolts;
            step = step > lim ? lim : (step < -lim ? -lim : step);
            v -= step;
            if (std::fabs(step) < ts9::kNewtonTolVolts)
                break;
        }
        // Bit test, not std::isfinite: this target is compiled with -ffast-math, under which
        // std::isfinite folds to a constant true and this guard is removed from the build
        // altogether — so it stood here inert rather than catching anything. See finite.h.
        if (!isFinite(v))
            v = 0.0;
        // f at the accepted point becomes the trapezoid's past term for the next sample.
        double sh, ch;
        sinhCoshBounded(v * mInvVt, sh, ch);
        mDeriv = drive - v * mInvR2Cc - mK2IsCc * sh;
        mV = v;
        return v;
    }

    // The trapezoidal residual, used to choose between the two initial guesses above.
    double residual(double v, double base) const
    {
        double sh, ch;
        sinhCoshBounded(v * mInvVt, sh, ch);
        return v - mV - mHalfStep * (base - v * mInvR2Cc - mK2IsCc * sh);
    }

    // Eq. 2.12's linear coefficients. R2 = 51k + D·500k, so the pole 1/(R2·Cc) walks from 61.2 kHz
    // at Drive 0 down to 5.7 kHz at Drive 10 — the darkening this whole ODE exists to keep.
    void updateDrive()
    {
        const double r2 = ts9::kR2Fixed + mDrive * ts9::kR2Drive;
        mInvR2Cc = 1.0 / (r2 * ts9::kCc);
        mInvCc = 1.0 / ts9::kCc;
        mK2IsCc = 2.0 * ts9::kIs / ts9::kCc;
        mInvVt = 1.0 / ts9::kVt;
    }

    // Eq. 2.14, written out as (b1·s + b0) / (a2·s² + a1·s + a0) and then bilinear-transformed.
    // Expanding Yeh's form:
    //     numerator   (Rl·Rf + Y)·(s + W·ωz)  with  W = Y/(Rl·Rf + Y)   →   (Rl·Rf + Y)·s + Y·ωz
    //     denominator Y·Rs·Cs·(s + ωp)(s + ωz) + X·s
    void updateTone()
    {
        const double rl = mTone * ts9::kTonePot;
        const double rr = (1.0 - mTone) * ts9::kTonePot;
        const double rpar = rl * rr / (rl + rr);
        const double y = (rl + rr) * (ts9::kToneRz + rpar);
        const double wz = 1.0 / (ts9::kToneCz * (ts9::kToneRz + rpar));
        const double wp = 1.0 / (ts9::kToneCs * (ts9::kToneRs * ts9::kToneRi /
                                                 (ts9::kToneRs + ts9::kToneRi)));
        const double x = (rr / (rl + rr)) * wz;

        const double b1 = rl * ts9::kToneRf + y;
        const double b0 = y * wz;
        const double a2 = y * ts9::kToneRs * ts9::kToneCs;
        const double a1 = a2 * (wp + wz) + x;
        const double a0 = a2 * wp * wz;

        // Bilinear, s → K(1 − z⁻¹)/(1 + z⁻¹), at the OVERSAMPLED rate because that is where this
        // filter runs. No prewarping: this is a shape rather than a single corner, so there is no
        // one frequency to prewarp at, and at 4x the warping across the audio band is small enough
        // to measure and dismiss rather than correct.
        const double k = 2.0 * mOsRate;
        const double kk = k * k;
        const double n0 = b1 * k + b0;
        const double n1 = 2.0 * b0;
        const double n2 = b0 - b1 * k;
        const double d0 = a2 * kk + a1 * k + a0;
        const double d1 = 2.0 * (a0 - a2 * kk);
        const double d2 = a2 * kk - a1 * k + a0;
        const double inv = 1.0 / d0;
        mB0 = n0 * inv;
        mB1 = n1 * inv;
        mB2 = n2 * inv;
        mA1 = d1 * inv;
        mA2 = d2 * inv;
    }

    // Transposed direct form II: two state words, and the one that behaves best numerically when
    // the coefficients are rebuilt underneath it, which happens once a block while Tone moves.
    double tone(double x)
    {
        const double y = mB0 * x + mT1;
        mT1 = mB1 * x - mA1 * y + mT2;
        mT2 = mB2 * x - mA2 * y;
        return y;
    }

    // --- controls, smoothed ------------------------------------------------------------------
    double mDriveTarget = 0.5, mToneTarget = 0.5, mLevelTarget = levelGain(0.5);
    double mDrive = 0.5, mTone = 0.5, mLevel = levelGain(0.5);

    // --- rates -------------------------------------------------------------------------------
    double mSampleRate = 48000.0, mOsRate = 192000.0, mHalfStep = 0.5 / 192000.0;

    // --- the clipping stage ------------------------------------------------------------------
    bbmh::Oversampler4x mOs;
    std::vector<float> mScratch;
    bbm::OnePole mInHp1, mInHp2, mClipHp;
    double mV = 0.0;     // the ODE's state, in volts
    double mDeriv = 0.0; // f at the previous sample: the trapezoid's past term
    double mInvCc = 1.0 / ts9::kCc, mInvR2Cc = 0.0, mK2IsCc = 0.0, mInvVt = 1.0 / ts9::kVt;

    // --- the tone stage ----------------------------------------------------------------------
    double mB0 = 1.0, mB1 = 0.0, mB2 = 0.0, mA1 = 0.0, mA2 = 0.0;
    double mT1 = 0.0, mT2 = 0.0;
};

} // namespace pedals
} // namespace Rations
