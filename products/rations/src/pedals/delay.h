// Rations — the Delay pedal.
// Copyright (c) 2026 rations. MIT licence (see LICENSE).
//
// Two fractional delay lines with a filtered feedback loop, so repeats darken as they go round
// rather than repeating identically forever. Stereo, with a ping-pong mode that crosses the two
// loops. Delay time is smoothed per sample, so dragging Time pitch-bends the repeats the way
// moving a real delay's read head does.
//
// THE STRUCTURE IS A FEEDBACK COMB FILTER, and the reference is Julius O. Smith III, *Physical
// Audio Signal Processing*, "Feedback Comb Filters" — a copy is in third_party/refs/pedals/pasp/.
// Two things are taken from that page directly:
//
//   y(n) = x(n) + g*y(n-M)     "a computational model of a series of echoes, exponentially
//                              decaying and uniformly spaced in time"
//
//   |g| < 1                    the stability condition, stated there as the thing that separates a
//                              decaying series of echoes from "a never-ending, growing series". It
//                              is why kDelayFeedbackId stops at 95 and not at 100, and it is what
//                              rations_pedalcheck's decay measurement asserts.
//
// Smith also notes that the output may be taken "from the end of the delay line instead of the
// beginning", which "merely delays the output signal by M samples". That is the tap this pedal
// uses: the wet signal IS the line read, so the first repeat arrives one delay after the note
// rather than on top of it, which is what a delay pedal is.
//
// WHERE THE LOOP FILTER GOES, and it is the one placement that changes what the pedal sounds like.
// It filters what is WRITTEN to the line — the input and the returning feedback together — so
// repeat n has been through it n times and the darkening is cumulative. That is the analog
// arrangement rather than a convenience: in a bucket-brigade delay the signal passes the
// anti-alias filter on its way INTO the device and the reconstruction filter on its way out, so
// even the first repeat is already duller than the dry. Putting the filter on the feedback return
// only would leave the first repeat a clean copy, which is a digital delay's behaviour.
//
// WHY THE PEDAL DOES NOT MODEL A BBD's FILTER, which is worth writing down because it looks like
// the obvious thing to do. A bucket-brigade device is a discrete-time analog line clocked at
// f_clk, so its usable bandwidth is bounded by f_clk/2, and its delay is stages/(2*f_clk) — which
// means bandwidth and delay time are not independent: bandwidth <= stages/(4*T). The relation is
// confirmed by the two operating points Panasonic's 2048-stage MN3208 is specified at, ~102 ms at
// a 10 kHz clock and ~2 ms at 500 kHz (2048/(2*10e3) = 102.4 ms; 2048/(2*500e3) = 2.048 ms), and
// it is the reason a real analog delay gets darker as it gets longer. Reproduced literally over
// this pedal's 20-2000 ms span it would be unusable: a 2048-stage device set to 2 s would have a
// 512 Hz clock and 256 Hz of bandwidth. That is exactly why long delays went digital, so Tone is
// an ordinary independent control and the darkening does not track the time knob.
//
// Time is in milliseconds and is IGNORED while Sync names a note division; the knob stays live and
// keeps its value, so unsyncing returns to where the player left it. The tempo comes from the
// host's ProcessContext and falls back to free-running when the host supplies none — verified
// against the pristine SDK before a line of it was written: ProcessContext::tempo is documented
// "(optional)" and is only valid when `state & kTempoValid` is set, and the host is not obliged to
// supply a ProcessContext at all (pluginterfaces/vst/ivstprocesscontext.h). Both are checked in
// RationsProcessor::process. A beat is a quarter note, which the SDK states for projectTimeMusic
// ("1.0 equals 1 quarter note") rather than for tempo itself; kDelaySyncBeats is written in those
// terms.
#pragma once

#include "pedal.h"
#include "primitives.h"
#include "../rationsids.h"

// WDL, for the feedback path's denormal flush. See NOTICE.
#include "wdl/denormal.h"

namespace Rations
{
namespace pedals
{

namespace delaydef
{
// The knob's own span, read out of the parameter table rather than repeated here, so the line can
// never be sized for a range the host does not offer.
inline constexpr double kKnobMinMs = kPedalParams[pedalParamIndex(kDelayTimeId)].min;
inline constexpr double kKnobMaxMs = kPedalParams[pedalParamIndex(kDelayTimeId)].max;

// What the LINE holds, which is deliberately longer than the knob's span. A synced whole note is
// four beats, so at 60 BPM it is 4 s; clamping that to the knob's 2 s would leave the delay an
// octave out of time, which is worse than long. Four seconds carries 1/1 down to 60 BPM and 1/2
// down to 30, and below that the division is clamped and says so here rather than silently.
inline constexpr double kLineMaxMs = 4000.0;

// What Tone sweeps the loop's low-pass over, geometrically. The default (Tone 5) therefore lands
// at sqrt(800*12000) = 3.1 kHz, which is where a companded bucket-brigade delay's repeats sit and
// is the voicing this pedal is centred on; the ends are a darker-than-analog and a
// near-digital repeat. These two numbers are a VOICING DECISION and not a measurement of any
// circuit — the same status as the Chorus's tap positions, and treated the same way: the gate
// measures the filter against literals written in the test, not against these.
inline constexpr double kToneLoHz = 800.0;
inline constexpr double kToneHiHz = 12000.0;

// The loop's fixed high-pass. This one is NOT taste. A low-pass has unity gain at DC, so without
// it the loop's DC gain is the feedback coefficient itself and any offset in the input accumulates
// to 1/(1-g) — a factor of 20 at the maximum feedback this pedal offers. Guitar's lowest string is
// 82 Hz, so a corner at 40 removes the offset and the subsonic content without touching anything
// the player is going to hear.
inline constexpr double kLoopHpHz = 40.0;

// Time is smoothed slowly ENOUGH TO BE HEARD, which is the opposite of every other smoother in
// this tree. A delay whose read head moves is pitch-shifting by the rate of change of its own
// delay, and that shift is what a player expects when they turn the knob mid-repeat; a fast
// smoother would make the change a chirp too brief to read as anything but a glitch.
inline constexpr double kTimeSmoothSec = 0.050;

// Everything else: fast enough not to lag a knob, slow enough not to step.
inline constexpr double kSmoothSec = 0.020;

// The prose above is only true while these hold.
static_assert(kLineMaxMs >= kKnobMaxMs,
              "the delay line must hold at least the longest time the knob can ask for");
static_assert(kToneLoHz > kLoopHpHz * 2.0 && kToneLoHz < kToneHiHz,
              "Tone's low-pass must sweep upwards and must stay clear of the fixed high-pass, or "
              "the two corners cross and the loop passes nothing at all");
} // namespace delaydef

// The loop's tone control: one low-pass, then one high-pass, both one-pole.
//
// The high-pass is built as `x - lowpass(x)` rather than as its own difference equation because
// that form has no separate coefficient to keep in step and cannot drift from the low-pass it is
// the complement of.
//
// BOTH STATES ARE FLUSHED, AND NO TEST CAN SEE IT, which is worth saying plainly rather than
// leaving as an unexamined line. The output is flushed too, and that one IS observable and is
// asserted by the gate. This one is not: whatever these states hold is written to the line and
// read back out through the output flush, so a subnormal here never reaches a buffer anything can
// look at. Its justification is therefore cost and not correctness, and the cost is measured —
// removing these two lines takes rations_pedalcheck from 1.94 s to 4.39 s (three runs each,
// spread under 0.02 s), because without them the line spends about seven seconds of every decay
// holding subnormals rather than zeros. FTZ/DAZ on the audio thread would hide that, and this DSP
// is deliberately not allowed to depend on it.
class LoopTone
{
public:
    void reset() { mLp = mHp = 0.0; }

    double process(double x, double aLp, double aHp)
    {
        mLp += aLp * (x - mLp);
        denormal_fix_double(&mLp);
        mHp += aHp * (mLp - mHp);
        denormal_fix_double(&mHp);
        return mLp - mHp;
    }

private:
    double mLp = 0.0, mHp = 0.0;
};

class Delay final : public Pedal
{
public:
    // Its own controls, in the order kPedalParams lists them for this pedal. The base hands over
    // a pointer to the start of that slice, so these index from zero.
    enum Param { kTime = 1, kFeedback = 2, kTone = 3, kMix = 4, kSync = 5, kPingPong = 6 };

    // The host's tempo, for the sync divisions. Zero or negative means the host supplied none,
    // and the free-running time is used instead — which is also what happens when Sync is "Free".
    // Must be pushed BEFORE setParams, because that is where the division is turned into a time;
    // PedalChain::setParams does so, and says why at the call.
    void setTempo(double bpm) { mTempoBpm = bpm; }

    void setParams(const double *plain) override
    {
        const int sync = static_cast<int>(plain[kSync]);
        // Free is a VALUE of the list rather than a second control, so there is one place that
        // decides which source is in force and a host cannot automate the pedal into both.
        double ms = plain[kTime];
        if (sync > 0 && sync < kDelaySyncCount && mTempoBpm > 0.0)
            ms = kDelaySyncBeats[sync] * 60000.0 / mTempoBpm;
        // A slow enough tempo asks for longer than the line holds. Clamped rather than wrapped:
        // the wrong length is recoverable by turning the tempo up, and a wrapped division would be
        // in time with nothing at all.
        ms = std::clamp(ms, delaydef::kKnobMinMs, delaydef::kLineMaxMs);

        const double samples = ms * mSampleRate * 0.001;
        const double fb = std::clamp(plain[kFeedback] * 0.01, 0.0, 1.0);
        const double mix = std::clamp(plain[kMix] * 0.01, 0.0, 1.0);
        const double cross = plain[kPingPong] > 0.5 ? 1.0 : 0.0;
        // Tone is 0..10 on the knob and geometric in frequency, so equal turns of it are equal
        // musical intervals. The COEFFICIENT is what gets smoothed, not the frequency, so the
        // per-sample path carries no exp().
        const double toneHz = delaydef::kToneLoHz
                              * std::pow(delaydef::kToneHiHz / delaydef::kToneLoHz,
                                         std::clamp(plain[kTone] * 0.1, 0.0, 1.0));
        const double aLp = onePoleCoef(toneHz);

        // The first push after a prepare or a reset lands; every one after it sweeps. See the same
        // note in chorus.h.
        if (mPrimed) {
            mDelaySamples.setTarget(samples);
            mFeedback.setTarget(fb);
            mMixAmt.setTarget(mix);
            mCross.setTarget(cross);
            mToneCoef.setTarget(aLp);
        } else {
            mDelaySamples.snap(samples);
            mFeedback.snap(fb);
            mMixAmt.snap(mix);
            mCross.snap(cross);
            mToneCoef.snap(aLp);
            mPrimed = true;
        }
    }

protected:
    void prepareImpl(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;
        // +2 because the read is one short (see the read below) and the cubic kernel reaches one
        // sample past the read point.
        const int maxDelay =
            static_cast<int>(std::ceil(delaydef::kLineMaxMs * sampleRate * 0.001)) + 2;
        for (int c = 0; c < 2; ++c)
            mLine[c].prepare(maxDelay);

        mHpCoef = onePoleCoef(delaydef::kLoopHpHz);

        // Coefficients only; the values are set by the first setParams, which is always called
        // before the first block of audio.
        mDelaySamples.prepare(sampleRate, delaydef::kTimeSmoothSec, mDelaySamples.target());
        mFeedback.prepare(sampleRate, delaydef::kSmoothSec, mFeedback.target());
        mMixAmt.prepare(sampleRate, delaydef::kSmoothSec, mMixAmt.target());
        mCross.prepare(sampleRate, delaydef::kSmoothSec, mCross.target());
        mToneCoef.prepare(sampleRate, delaydef::kSmoothSec, mToneCoef.target());
    }

    // Clearing the two lines is the most expensive thing this pedal does, and unlike everything
    // else in it, it can happen ON THE AUDIO THREAD: the base resets a pedal once, at the moment
    // its disengage ramp reaches zero. Four seconds at 48 kHz rounds to 262144 doubles a side, so
    // this is a 4 MB memset — measured at a worst 153 us over 200 runs, against the 2667 us of a
    // 128-frame period. It is 5.7 % of one block, it is bounded, and it happens once per stomp
    // rather than per block, so it is accepted rather than spread over several blocks; spreading
    // it would mean reads could reach a region not yet cleared, since the knob may ask for the
    // longest delay on the very first block after the pedal comes back.
    void resetImpl() override
    {
        for (int c = 0; c < 2; ++c) {
            mLine[c].reset();
            mFilt[c].reset();
        }
        mPrimed = false;
    }

    void processImpl(DSP_SAMPLE *l, DSP_SAMPLE *r, int numSamples) override
    {
        // POST is stereo, but the base allows a null right channel and the contract has to hold.
        // With one channel there is one loop and ping-pong has nowhere to go, so it is held at
        // zero — the smoother is still stepped, so the mode is where it should be if a second
        // channel ever arrives.
        const double stereo = r ? 1.0 : 0.0;

        for (int i = 0; i < numSamples; ++i) {
            const double d = mDelaySamples.next();
            const double fb = mFeedback.next();
            const double mix = mMixAmt.next();
            const double aLp = mToneCoef.next();
            const double cross = mCross.next() * stereo;

            // READ ONE SHORT, for the reason flanger.h gives in full at its own read: the line is
            // read before this sample is written to it, because the value written depends on what
            // was read, so a read at D returns the sample from D+1 ago. Reading at D-1 makes the
            // loop exactly D samples round, which is what the knob says it is.
            const double dl = mLine[0].read(d - 1.0);
            const double dr = r ? mLine[1].read(d - 1.0) : 0.0;

            const double xl = l[i];
            const double xr = r ? r[i] : 0.0;

            // Ping-pong, as one continuous coefficient rather than a branch, so stomping the mode
            // is a 20 ms crossfade between two topologies instead of a step. At cross = 0 the two
            // loops are independent and each feeds itself; at cross = 1 the input goes to the left
            // loop only and each loop feeds the OTHER, so the repeats alternate L, R, L and lose
            // one feedback's worth of level per hop.
            //
            // The input collapses to the AVERAGE rather than to the sum, so a correlated pair —
            // which is what the cabinet hands over whenever the Flanger is not running — keeps its
            // level as the mode is crossed into.
            const double mid = 0.5 * (xl + xr);
            const double injL = xl + cross * (mid - xl);
            const double injR = xr - cross * xr;
            const double retL = dl + cross * (dr - dl);
            const double retR = dr + cross * (dl - dr);

            mLine[0].write(mFilt[0].process(injL + fb * retL, aLp, mHpCoef));
            if (r)
                mLine[1].write(mFilt[1].process(injR + fb * retR, aLp, mHpCoef));

            // Mix, as the Chorus mixes: a pedal with no output level control must not be able to
            // add 6 dB by turning one knob up.
            double yl = xl + mix * (dl - xl);
            denormal_fix_double(&yl);
            l[i] = yl;
            if (r) {
                double yr = xr + mix * (dr - xr);
                denormal_fix_double(&yr);
                r[i] = yr;
            }
        }
    }

private:
    // The one-pole's exact-decay discretisation: a pole at exp(-2*pi*f/fs), so the coefficient is
    // one minus it. Used for the loop's low-pass and its high-pass alike.
    double onePoleCoef(double hz) const
    {
        return 1.0 - std::exp(-kTwoPi * std::max(0.0, hz) / mSampleRate);
    }

    FracDelay mLine[2];
    LoopTone mFilt[2];
    Smoothed mDelaySamples, mFeedback, mMixAmt, mCross, mToneCoef;
    double mHpCoef = 0.0;
    double mTempoBpm = 0.0;
    bool mPrimed = false;
};

} // namespace pedals
} // namespace Rations
