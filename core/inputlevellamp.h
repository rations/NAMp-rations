// InputLevelLamp — whether the level the sounding capture hears is the level it was trained on.
//
// The green lamp beside the Input knob. It answers one question, and it is a question about the
// MODEL INPUT, not about the interface: the signal after the Input knob and after the sounding
// capture's own input calibration. That gain is exactly 1.0 when Calibrate is off or when the
// capture states no input_level_dbu, so a capture without metadata is judged by the same rule as
// one with it, and nothing is guessed on its behalf. With Calibrate on, it is still the model
// input that is judged: calibration is a gain on that input, so the lamp follows it rather than
// having a second rule for that mode.
//
// The rule is a window on the block's sample peak:
//
//   * LIT when the peak is above -6.0 dBFS and at most 0.0 dBFS. The bottom edge is the one
//     measured on ML Sound Lab's Amped, whose input LED is off for a -6.0 dBFS peak and on for a
//     -5.9 dBFS one, and which reads peak rather than RMS (a burst with the same peak and 10 dB
//     less RMS still lights it). It agrees within 2 dB with the NAM training signal itself, the
//     same material in the trainer's v2 and v3 inputs: over 300 ms windows, 90% of its peaks are
//     at or below -7.8 dBFS. The top edge is that signal's loudest sample, -0.3 dBFS, rounded to
//     full scale: above it the capture is hearing a level it was never shown.
//   * OFF below the window, once the hold has run out, so a played phrase reads as a steady light
//     rather than flickering between notes.
//   * OFF above the window at once, and held off for the same hold. The second half is not
//     decoration: a hot pick attack decays back through the window within milliseconds, and
//     without the hold the lamp would relight before any editor repaint could show it was ever
//     dark. Too hot would then be indistinguishable from just right, which is the one thing the
//     lamp exists to tell apart. Amped never does this; its LED stays lit to +12 dBFS.
//   * OFF at once when no capture is hearing anything (gain 0), so an empty channel, or one still
//     building, never shows a light left over from the capture before it.
//
// The hold is a choice, not a measurement. Amped holds full brightness for about 0.2 s and then
// fades over about 2 s; this lamp has two states, so one hold stands in for both.
//
// Real-time contract: process() allocates nothing, takes no lock and calls nothing; the state is
// plain members owned by the audio thread. reset() is called from setupProcessing(), when the
// audio thread is not running.

#pragma once

#include <algorithm>
#include <cmath>

namespace Rations
{

//------------------------------------------------------------------------
class InputLevelLamp
{
public:
    // The window, as sample peaks in dBFS at the model input. Lit means kLitAboveDb < peak <=
    // kLitAtMostDb. See the top of this file for where each number comes from.
    static constexpr double kLitAboveDb = -6.0;
    static constexpr double kLitAtMostDb = 0.0;
    // How long a peak in the window keeps the lamp lit, and a peak above it keeps the lamp dark.
    static constexpr double kHoldSeconds = 1.0;

    // Non-RT. The hold is counted in samples, so it has to know the rate it is counting at.
    void reset(double sampleRate)
    {
        mHoldSamples = static_cast<long long>(std::llround(kHoldSeconds * sampleRate));
        mLitLeft = 0;
        mHotLeft = 0;
    }

    // RT. One block of the signal the sounding capture is fed, before its input calibration, and
    // that calibration's gain. A gain of 0 means no capture is hearing it.
    void process(const double *x, int numFrames, double gain)
    {
        if (!(gain > 0.0)) {
            mLitLeft = 0;
            mHotLeft = 0;
            return;
        }
        // A NaN sample loses every comparison and so is ignored rather than propagated; an
        // infinite one reads as hot, which is the right answer for it.
        double peak = 0.0;
        for (int i = 0; i < numFrames; ++i)
            peak = std::max(peak, std::fabs(x[i]));
        peak *= gain;

        // Count down by this block first, then let this block restart whichever hold it earned,
        // so a peak at the end of a block holds for the full time from there.
        const long long elapsed = numFrames > 0 ? numFrames : 0;
        mLitLeft = std::max(0LL, mLitLeft - elapsed);
        mHotLeft = std::max(0LL, mHotLeft - elapsed);
        if (peak > mHotAbove) {
            mHotLeft = mHoldSamples;
            mLitLeft = 0;
        } else if (peak > mLitAbove && mHotLeft == 0) {
            mLitLeft = mHoldSamples;
        }
    }

    bool lit() const
    {
        return mLitLeft > 0 && mHotLeft == 0;
    }

private:
    // The window's edges as linear peaks. std::pow is not constexpr, so they are worked out once,
    // at construction, from the dB constants above rather than written out as bare numbers.
    double mLitAbove = std::pow(10.0, kLitAboveDb / 20.0);
    double mHotAbove = std::pow(10.0, kLitAtMostDb / 20.0);

    long long mHoldSamples = 0;
    long long mLitLeft = 0;
    long long mHotLeft = 0;
};

} // namespace Rations
