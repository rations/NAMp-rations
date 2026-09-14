// Rations — the Reverb pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Built on deps/wdl/verbengine.h — Cockos' WDL_ReverbEngine, itself derived from Jezar at
// Dreampoint's public-domain Freeverb. Vendored whole and wrapped rather than written, because a
// Schroeder/Moorer reverberator that somebody tuned by listening is worth more than a fresh one
// with the same block diagram, and because a hand-rolled reverb is the easiest thing on this board
// to get subtly wrong. RULES §5's "port, do not re-invent", applied to the one pedal where the
// permissive prior art is not a primitive but the whole effect.
//
// THE REFERENCE IS PASP's "Freeverb" section and the two pages under it — "Freeverb Main Loop" and
// "Lowpass-Feedback Comb Filter" — copies in third_party/refs/pedals/pasp/. Everything this pedal
// decides for itself is decided from what those pages say the engine's own controls MEAN:
//
//   LBCF(N,f,d) = z^-N / (1 - f*(1-d)/(1 - d*z^-1)*z^-N)
//
//     the lowpass-feedback comb filter, one per comb, with a UNITY-GAIN one-pole in the loop:
//     H(z) = (1-d)/(1 - d z^-1). Unity gain at DC is the whole reason the two knobs separate
//     cleanly — see Decay and Tone below.
//
//   f < 1                        required for DC stability, which is what bounds the Decay map.
//
//   "the roomsize parameter can be interpreted as setting the low-frequency T60 ... while the
//   damping parameter controls how rapidly T60 shortens as a function of increasing frequency"
//
//     That sentence is the panel. Decay is the engine's f and Tone is its d, and the sentence is
//     also the GATE: rations_pedalcheck measures T60 in a low band and a high band separately and
//     asserts that Tone moves the second and leaves the first alone. A plain low-pass across the
//     wet output would sound similar on a first listen and would fail that, because it shortens
//     nothing.
//
//   "A lower-limit on T60 is given by the four diffusion allpass filters."
//
//     Which is why the short end of the Decay map is 0.4 s and not 0.05: the allpass chain rings
//     for about 0.13 s whatever f is, so a shorter target would be a knob asking for something the
//     structure cannot deliver.
//
// WDL IS NOT FREEVERB, and the differences are load-bearing rather than cosmetic. It runs TEN
// combs and SIX allpasses per channel where Freeverb has eight and four; it takes the comb
// feedback f directly instead of through Freeverb's `roomsize*0.28 + 0.7` map; and — the one that
// matters most here — it is a TRUE STEREO engine. Freeverb sums its two inputs to mono and feeds
// that to both banks (PASP quotes the loop: `input = (*inputL + *inputR) * gain`); WDL feeds the
// left input to the left bank and the right to the right. This pedal is last in a POST chain that
// the Flanger and the Delay have already made genuinely stereo, so throwing that away at the
// reverb would undo both of them.
//
// WIDTH IS FIXED AT 1.0 AND IS NOT ON THE PANEL. In WDL's mixer that is the setting where the left
// output is the left bank and the right output is the right bank, with no cross-feed — PASP's
// `wet2 = 0`, "maximally different left and right reverberation signals". The two banks are
// already detuned against each other by the engine's own `stereospread` of 23 samples, so full
// separation is what makes that detuning audible; anything less is a width control that spends its
// travel undoing the tuning. A pedal with four knobs does not get a fifth for that.
//
// WHERE THE LEVEL COMPENSATION COMES FROM, and it is the one piece of arithmetic this wrapper adds
// to the engine rather than merely configuring. A feedback comb's power gain for a broadband input
// is 1/(1-f^2) — the sum of a geometric series in f^2 over the taps of its impulse response — so
// raising Decay from one end of its range to the other makes the wet signal about 9 dB louder all
// by itself. On a reverb with its own wet fader that is correct and expected; on a pedal whose one
// mix knob has to stay put while the player auditions decay times it is a fault. So the wet is
// scaled by sqrt(1 - f^2), which cancels that growth exactly for the comb bank and leaves the
// endpoints of the Decay knob at the same loudness. Damping is deliberately NOT compensated: a
// damped room IS quieter, that is what absorption does, and Tone would stop being a physical
// control if it were levelled.
#pragma once

#include "pedal.h"
#include "primitives.h"
#include "../rationsids.h"

// WDL. <cstdlib> and <cstring> come first because heapbuf.h calls malloc/realloc/free and memset
// and relies on its includer having pulled the C library in already — upstream's own convention,
// and not something this tree may fix, since deps/ is vendored verbatim. The two pragmas are there
// for the same reason: verbengine.h compares an int loop counter against a sizeof, and the file is
// not ours to edit, so the warning is suppressed at the include rather than in the file.
#include <cstdlib>
#include <cstring>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "wdl/verbengine.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "wdl/denormal.h"

namespace Rations
{
namespace pedals
{

namespace reverbdef
{
// The knob's own span, read out of the parameter table rather than repeated here, so the pre-delay
// line can never be sized for a range the host does not offer.
inline constexpr double kPreDelayMaxMs = kPedalParams[pedalParamIndex(kReverbPreDelayId)].max;

// What Decay sweeps, geometrically, so equal turns of the knob are equal ratios of decay time —
// which is how the ear reads a reverb tail, and the same treatment the Delay's Tone gets.
//
// The ENDS are a voicing decision and are pinned against literals written in rations_pedalcheck,
// for the reason the Delay's tone corners are: a span is a design choice, and reading these
// constants for the expectation would move both sides of the comparison at once. What is NOT a
// voicing decision is that the short end clears the allpass chain's own ring-out (about 0.13 s at
// the fixed g = 0.5 the engine uses), because below that the knob would be asking for something
// no value of f can produce.
inline constexpr double kT60MinSec = 0.4;
inline constexpr double kT60MaxSec = 6.0;

// The engine's longest comb, in samples at the rate its tuning table is written for. Both numbers
// are read out of verbengine.h — `wdl_verb__combtunings[]` ends at 1748, and its own comment says
// the table "represent lengths in samples at 44.1khz but are scaled accordingly". The floor()
// below reproduces the engine's own `(int)(tuning * srate/44100)` truncation rather than an
// idealisation of it, so the lap length this pedal computes f from is the lap length the engine
// actually runs.
inline constexpr double kLongestCombAt44k = 1748.0;
inline constexpr double kTuningRate = 44100.0;

// PASP: "Since f < 1 is required for dc stability". 0.98 is a T60 of about 14 s at 48 kHz, far
// past the knob's own top end, and exists as a backstop rather than as a value anything reaches.
inline constexpr double kFeedbackMax = 0.98;
inline constexpr double kFeedbackMin = 0.02;

// What the wet is multiplied by after the sqrt(1 - f^2) compensation, so that Mix at 100 % is the
// same loudness as the dry signal it replaces. DERIVED FROM THE ENGINE'S STRUCTURE, not fitted to
// a run of it, and the derivation is short enough to give in full.
//
// For a broadband input the power gain of a filter is the sum of the squares of its impulse
// response. The engine is ten combs in parallel feeding six allpass sections in series:
//
//   comb      after the sqrt(1 - f^2) compensation each contributes exactly 1, and ten of them in
//             parallel have delays that share almost no common multiples, so their cross terms
//             vanish and the powers add: 10.
//
//   allpass   PASP's "Freeverb Allpass Approximation" gives what Freeverb actually implements,
//             which is not an allpass: (-1 + (1+g)z^-N)/(1 - g z^-N). Expanding it, the impulse
//             response is {-1, 1, g, g^2, ...} at taps 0, N, 2N, ..., so its sum of squares is
//             1 + 1/(1 - g^2) = 7/3 at the g = 0.5 the engine fixes. Six in series: (7/3)^3.
//
//   0.015     Freeverb's `fixedgain`, carried into WDL verbatim, applied to the engine's input.
//
// So the raw broadband amplitude gain is 0.015 * sqrt(10) * (7/3)^3 = 0.6025, and this is its
// reciprocal. Measured on the built pedal: 0.5940, which is 1.4 % below the prediction — the
// residual is the tap collisions the "cross terms vanish" step above waves away. rations_pedalcheck
// recomputes the prediction from those structural constants written out again and asserts the
// measurement against it, so neither this number nor that one can drift alone.
inline constexpr double kWetNorm = 1.6597;

// What converts the T60 the LONGEST comb would give on its own into the T60 a measurement actually
// reports for the whole bank, and it is a property of the ten comb lengths rather than a fudge.
//
// PASP's LBCF analysis gives the decay of ONE comb exactly. A reverb is ten of them at once, with
// lengths spanning 1214 to 1902 samples at 48 kHz, so the early tail is steeper than the late one:
// the short combs are still contributing while the long one sets the asymptote. A T30 fit over the
// standard -5 to -35 dB window therefore lands short of the asymptotic figure, by this much.
//
// It is a CONSTANT rather than a curve, and that is provable rather than lucky. Each comb's energy
// decays as exp(2*t*fs*ln(f)/N_i), so the whole bank's decay curve depends on t and f only through
// the product t*ln(f) — change f and the curve stretches without changing shape, so any fit over
// any fixed dB window scales with it and the ratio is fixed. Computed from the ten lengths and that
// window: 0.8925, and the same value comes out at 44.1, 48 and 96 kHz, which is why this pedal's
// T60 does not depend on the sample rate.
//
// Measured end to end on the built pedal it is 0.884 rather than 0.8925 — 1 % short, drifting from
// 0.887 at the shortest decay to 0.882 at the longest. That residual is not noise, and the
// reference names it: the six diffusion allpasses ring at a FIXED g = 0.5 whatever f is — PASP's
// "a lower-limit on T60 is given by the four diffusion allpass filters" — and a floor that does not
// scale is exactly what breaks the scale invariance above, most at the short end, which is the
// direction the drift goes. 1 % of a decay time is not a thing anyone can hear, so the derived
// value is kept and the residual is reported rather than absorbed.
inline constexpr double kEnsembleT60Ratio = 0.8925;

// Fast enough not to lag a hand, slow enough not to step.
inline constexpr double kSmoothSec = 0.020;

// Pre-delay is slower, and for the Delay's reason: a moving read head pitch-bends, and a pre-delay
// dragged across 200 ms should sweep rather than chirp. It is upstream of the engine, so like the
// two above it cannot be seen to click at the output — what IS gated is where it puts the first
// wet sample, which rations_pedalcheck measures to the sample at six settings.
inline constexpr double kPreDelaySmoothSec = 0.050;

// How often the engine's own coefficients are re-pushed, in samples.
//
// This is the one place the wrapper cannot follow the rest of the board. Every other pedal here
// smooths its coefficients PER SAMPLE, because that is what makes a dragged knob continuous; the
// reverb's feedback and damping live inside thirty-two vendored filter objects and are set through
// an API — SetRoomSize / SetDampening / Reset(false) — that is a loop over all of them, so
// per-sample would mean that loop per sample. A short fixed chunk is the same answer NAMp-D5's
// `kChunk` gives to the same shape of problem, and it costs about eight operations a sample.
//
// IT IS MARGIN RATHER THAN NECESSITY, and the measurement says so plainly enough to be worth
// writing down. Everything upstream of the engine — these two coefficients, and the pre-delay
// too — reaches the output only through the engine, which begins by multiplying its input by
// Freeverb's `fixedgain` of 0.015: 36 dB of attenuation before anything can be heard, and what
// survives it is spread across ten comb lengths that are not whole multiples of the block, of this
// chunk, or of each other. rations_pedalcheck tried the faults: snapping Decay, Tone or Pre-delay
// outright — no smoothing at all — is not detectable at this pedal's output by any check in that
// file, and neither is widening this chunk to a whole block. Snapping Mix, which scales the OUTPUT,
// is caught immediately. So 16 is kept because it costs about eight operations a sample and buys
// headroom against a knob nobody has swept yet, not because a measurement demanded it.
inline constexpr int kCoefChunk = 16;

static_assert(kT60MinSec > 0.0 && kT60MaxSec > kT60MinSec,
              "Decay must sweep upwards over a positive span");
static_assert(kFeedbackMax < 1.0,
              "PASP: f < 1 is required for DC stability, and f = 1 is a reverb that never stops");
static_assert(kPreDelayMaxMs > 0.0, "the pre-delay line needs a length to be sized for");
} // namespace reverbdef

class Reverb final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kDecay = 1, kTone = 2, kPreDelay = 3, kMix = 4 };

    void setParams(const double *plain) override
    {
        // Decay -> the engine's comb feedback f, through the T60 it is being asked for. PASP's
        // "Achieving Desired Reverberation Times" gives the relation in the form used here:
        // propagation through n60 samples must attenuate by 60 dB, so G = 10^(-3/n60) per sample,
        // and one lap of the longest comb is mLapSeconds of those.
        const double t60 = reverbdef::kT60MinSec
                           * std::pow(reverbdef::kT60MaxSec / reverbdef::kT60MinSec,
                                      std::clamp(plain[kDecay] * 0.1, 0.0, 1.0));
        const double f =
            std::clamp(std::pow(10.0, -3.0 * mLapSeconds * reverbdef::kEnsembleT60Ratio / t60),
                       reverbdef::kFeedbackMin, reverbdef::kFeedbackMax);

        // Tone -> the engine's damping, INVERTED, because the knob reads as brightness and the
        // engine's parameter is loss: Tone 10 is d = 0, the undamped LBCF, which PASP notes
        // "reduces to the feedback comb filter ... in which the feedback was not filtered".
        const double damp = std::clamp(1.0 - plain[kTone] * 0.1, 0.0, 1.0);

        const double pd = std::clamp(plain[kPreDelay], 0.0, reverbdef::kPreDelayMaxMs)
                          * mSampleRate * 0.001;
        const double mix = std::clamp(plain[kMix] * 0.01, 0.0, 1.0);

        // The level compensation the header explains. Computed from the TARGET f rather than from
        // the smoothed one so that the gain and the feedback arrive together; both are then
        // smoothed at the same rate, so they stay in step on the way.
        const double wet = reverbdef::kWetNorm * std::sqrt(std::max(0.0, 1.0 - f * f));

        // The first push after a prepare or a reset lands; every one after it sweeps. See the same
        // note in chorus.h.
        if (mPrimed) {
            mFeedback.setTarget(f);
            mDamp.setTarget(damp);
            mPreDelaySamples.setTarget(pd);
            mMixAmt.setTarget(mix);
            mWetGain.setTarget(wet);
        } else {
            mFeedback.snap(f);
            mDamp.snap(damp);
            mPreDelaySamples.snap(pd);
            mMixAmt.snap(mix);
            mWetGain.snap(wet);
            mPrimed = true;
        }
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;

        // ALLOCATES, and this is the only call in the pedal that does: SetSampleRate re-sizes all
        // thirty-two delay lines when the rate changes. Everything after it — including the
        // Reset(true) in resetImpl — finds the sizes already right and does nothing but memset.
        mEngine.SetSampleRate(sampleRate);

        // Full stereo separation, which is the header's reasoning and PASP's wet2 = 0.
        mEngine.SetWidth(1.0);

        // The engine's own truncation, reproduced rather than idealised. See kLongestCombAt44k.
        mLapSeconds =
            std::floor(reverbdef::kLongestCombAt44k * sampleRate / reverbdef::kTuningRate)
            / sampleRate;

        // +2 for the cubic kernel's reach past the read point.
        const int maxPre =
            static_cast<int>(std::ceil(reverbdef::kPreDelayMaxMs * sampleRate * 0.001)) + 2;
        for (int c = 0; c < 2; ++c)
            mPre[c].prepare(maxPre);

        // Coefficients only; the values are set by the first setParams, which is always called
        // before the first block of audio.
        mFeedback.prepare(sampleRate, reverbdef::kSmoothSec, mFeedback.target());
        mDamp.prepare(sampleRate, reverbdef::kSmoothSec, mDamp.target());
        mMixAmt.prepare(sampleRate, reverbdef::kSmoothSec, mMixAmt.target());
        mWetGain.prepare(sampleRate, reverbdef::kSmoothSec, mWetGain.target());
        mPreDelaySamples.prepare(sampleRate, reverbdef::kPreDelaySmoothSec,
                                 mPreDelaySamples.target());
    }

    // Runs on the AUDIO THREAD, once, at the moment the disengage ramp reaches zero — the base
    // class's true-bypass rule. Reset(true) is a memset over the engine's thirty-two lines and
    // reallocates nothing, because prepareImpl already sized them for this rate; with the two
    // pre-delay lines that is about half a megabyte, against the Delay's four. Measured in
    // rations_pedalcheck beside the Delay's, for the same reason: it is the one bounded burst this
    // pedal puts on that thread and it should not be assumed small.
    void resetImpl() override
    {
        mEngine.Reset(true);
        for (int c = 0; c < 2; ++c)
            mPre[c].reset();
        // Nothing has been pushed to the engine since it was cleared, and a NaN sentinel would be
        // worse than an impossible value: -1 is outside both parameters' ranges, so the first
        // chunk after a reset always pushes.
        mPushedF = mPushedD = -1.0;
        mPrimed = false;
    }

    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        int i = 0;
        while (i < numSamples) {
            const int n = std::min(reverbdef::kCoefChunk, numSamples - i);
            pushCoefficients();

            for (int k = 0; k < n; ++k) {
                const size_t j = static_cast<size_t>(i + k);
                const double pd = mPreDelaySamples.next();
                const double wetGain = mWetGain.next();
                const double mix = mMixAmt.next();
                // Stepped even though the chunk push is what reads them, so that every smoother in
                // this pedal advances on the same clock and a chunk boundary cannot land the two
                // halves of the level compensation on different samples.
                mFeedback.next();
                mDamp.next();

                const double xl = l[j];
                // A null right channel is the base class's mono contract. The POST chain never
                // hands one over, but the contract has to hold: both banks are then fed the same
                // signal and the left one is what comes out, which is a reverb rather than a
                // half-built one.
                const double xr = r ? r[j] : xl;

                // Pre-delay. Written before it is read, so a read at D returns the sample from D
                // ago — no feedback here, so the Delay's one-short convention does not apply. The
                // line's own floor is one sample (the cubic kernel reaches one sample newer than
                // the read point), so Pre at 0 is 21 us at 48 kHz rather than nothing; that is
                // below the shortest comb by three orders of magnitude and is what the gate's
                // arrival measurement expects.
                mPre[0].write(xl);
                mPre[1].write(xr);
                double wl = mPre[0].read(pd);
                double wr = mPre[1].read(pd);

                mEngine.ProcessSample(&wl, &wr);
                wl *= wetGain;
                wr *= wetGain;

                // The engine flushes its own thirty-two states through WDL's own
                // denormal_filter_double. The OUTPUT still needs flushing, and that is not
                // belt-and-braces: a reverb tail is the longest decay on this board, so it
                // spends longer in the subnormal range than anything else, and whatever it hands
                // on goes straight into the plug-in's output buffer. Same lesson as the Flanger's,
                // found the same way.
                double yl = xl + mix * (wl - xl);
                denormal_fix_double(&yl);
                l[j] = yl;
                if (r) {
                    double yr = xr + mix * (wr - xr);
                    denormal_fix_double(&yr);
                    r[j] = yr;
                }
            }
            i += n;
        }
    }

private:
    // The engine's two settings, pushed together because Reset(false) is what makes either of them
    // take effect — verbengine.h's own comment: "call this after changing roomsize or dampening".
    // Guarded on change because that call walks all thirty-two filters, and a knob nobody is
    // touching should cost nothing.
    void pushCoefficients()
    {
        const double f = mFeedback.current();
        const double d = mDamp.current();
        if (f == mPushedF && d == mPushedD)
            return;
        mEngine.SetRoomSize(f);
        mEngine.SetDampening(d);
        mEngine.Reset(false); // sizes are unchanged, so this reallocates nothing and clears nothing
        mPushedF = f;
        mPushedD = d;
    }

    WDL_ReverbEngine mEngine;
    FracDelay mPre[2];
    Smoothed mFeedback, mDamp, mPreDelaySamples, mMixAmt, mWetGain;
    double mLapSeconds = 1748.0 / 44100.0;
    double mPushedF = -1.0, mPushedD = -1.0;
    bool mPrimed = false;
};

} // namespace pedals
} // namespace Rations
