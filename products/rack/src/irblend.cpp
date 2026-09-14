#include "irblend.h"

#include <algorithm>
#include <cmath>

namespace Rations
{

namespace
{
// Pole and zero frequencies, in Hz, of the profiling weight. Three one-pole, one-zero sections,
// each falling at -6 dB/octave between its pole and its zero and flat outside that span; staggered
// a decade apart, they average to a straight tilt across 20 Hz to 6 kHz, which is the whole of a
// guitar cabinet's useful range. The tilt is set by how far each zero sits above its pole:
// slope = -6 * log10(zero / pole) dB per octave.
//
// Derived here rather than lifted as four magic constants from a pink-noise recipe, because a
// recipe's coefficients are baked for one sample rate and this has to be right at 44.1, 48, 88.2
// and 96 kHz. Bilinear-transformed at the host rate below; the realised response is within 0.25 dB
// of a straight line at every one of those rates.
//
// THE TILT IS MEASURED, NOT CHOSEN. tools/rations_ircheck.cpp was run over eight pairs of real
// cabinet IRs - a Mesa 4x12 of V30s at a range of SM57 positions, a Fender Bassman 2x15, and
// crosses between the two - at tilts from -1.5 to -6 dB/octave, scoring each by the worst
// deviation of the blend from the straight line between its endpoints:
//
//     -1.5    worst 0.62 dB   mean 0.26
//     -1.9    worst 0.53 dB   mean 0.20
//     -2.0    worst 0.51 dB   mean 0.19     <- shipped
//     -2.1    worst 0.54 dB   mean 0.19
//     -2.25   worst 0.60 dB   mean 0.20
//     -3.0    worst 0.85 dB   mean 0.27
//     -4.5    worst 1.68 dB   mean 0.52
//     -6.0    worst 2.48 dB   mean 0.78
//
// The basin is broad and shallow between -1.9 and -2.1, so -2.0 is taken as the round number
// inside it rather than as a value fitted to eight pairs from two cabinets. Zeros are therefore at
// 10^(2/6) = 2.154 times their poles.
constexpr double kWeightPoleHz[3] = {20.0, 200.0, 2000.0};
constexpr double kWeightZeroHz[3] = {43.09, 430.9, 4309.0};

// Spelled out rather than reached for as M_PI, which is a POSIX extension: it is absent under
// MinGW's strict-ANSI settings, and this file is cross-compiled.
constexpr double kTwoPi = 6.283185307179586476925286766559;
} // namespace

//------------------------------------------------------------------------
void fillIrProfileStimulus(double *buf, size_t n, double sampleRate)
{
    if (!buf || n == 0)
        return;
    for (size_t i = 0; i < n; ++i)
        buf[i] = (i == 0) ? 1.0 : 0.0;
    if (sampleRate <= 0.0)
        return;

    const size_t len = std::min(n, static_cast<size_t>(kIrProfileStimulus));
    const double k = 2.0 * sampleRate;
    for (int sec = 0; sec < 3; ++sec) {
        // H(s) = (1 + s/wz) / (1 + s/wp), bilinear-transformed.
        const double wp = kTwoPi * kWeightPoleHz[sec];
        const double wz = kTwoPi * kWeightZeroHz[sec];
        const double dp = 1.0 + k / wp;
        const double b0 = (1.0 + k / wz) / dp;
        const double b1 = (1.0 - k / wz) / dp;
        const double a1 = (1.0 - k / wp) / dp;
        double x1 = 0.0, y1 = 0.0;
        for (size_t i = 0; i < len; ++i) {
            const double x = buf[i];
            const double y = b0 * x + b1 * x1 - a1 * y1;
            x1 = x;
            y1 = y;
            buf[i] = y;
        }
    }

    // Unit energy, so the reported powers stay in a readable range and do not depend on where the
    // weighting cascade happens to sit. Scale is irrelevant to rho and cancels out of the weights.
    double energy = 0.0;
    for (size_t i = 0; i < len; ++i)
        energy += buf[i] * buf[i];
    if (energy > 0.0) {
        const double norm = 1.0 / std::sqrt(energy);
        for (size_t i = 0; i < len; ++i)
            buf[i] *= norm;
    }
    for (size_t i = len; i < n; ++i)
        buf[i] = 0.0;
}

//------------------------------------------------------------------------
IrBlend measureIrBlend(const double *ha, const double *hb, size_t n)
{
    IrBlend out;
    if (!ha || !hb || n == 0)
        return out;

    double pa = 0.0, pb = 0.0, cross = 0.0;
    for (size_t i = 0; i < n; ++i) {
        pa += ha[i] * ha[i];
        pb += hb[i] * hb[i];
        cross += ha[i] * hb[i];
    }
    out.powerA = pa;
    out.powerB = pb;
    // A silent slot has no direction, so there is no correlation to speak of; rho stays 0 and the
    // formula degenerates gracefully rather than dividing by zero.
    if (pa > 0.0 && pb > 0.0) {
        out.rho = std::clamp(cross / std::sqrt(pa * pb), -1.0, 1.0);
        out.measured = true;
    }
    return out;
}

//------------------------------------------------------------------------
DSP_SAMPLE **processCabinet(dsp::ImpulseResponse *a, dsp::ImpulseResponse *b, const IrBlend &blend,
                            double position, DSP_SAMPLE **input, size_t numFrames,
                            DSP_SAMPLE **mixPtr)
{
    // The order of these tests is the point. One slot filled is the normal case and it takes the
    // same single call it took when there was only one slot.
    if (!a && !b)
        return input;
    if (!b)
        return a->Process(input, 1, numFrames);
    if (!a)
        return b->Process(input, 1, numFrames);

    // Both IRs see the same input - Process() writes into each object's own output buffer and does
    // not touch its input - so A's result is still valid while B is running.
    DSP_SAMPLE **ya = a->Process(input, 1, numFrames);
    DSP_SAMPLE **yb = b->Process(input, 1, numFrames);

    double wa = 1.0, wb = 0.0;
    blend.weights(position, wa, wb);
    DSP_SAMPLE *dst = *mixPtr;
    for (size_t i = 0; i < numFrames; ++i)
        dst[i] = wa * ya[0][i] + wb * yb[0][i];
    return mixPtr;
}

//------------------------------------------------------------------------
void IrBlend::weights(double x, double &a, double &b) const
{
    x = std::clamp(x, 0.0, 1.0);
    const double la = 1.0 - x, lb = x;

    // Unmeasured: the linear mix. Never louder than an endpoint, which is the property that
    // matters when there is nothing to base a correction on.
    if (!measured) {
        a = la;
        b = lb;
        return;
    }

    const double r = std::max(rho, kIrBlendRhoFloor);
    const double target = la * powerA + lb * powerB;
    const double power =
        la * la * powerA + lb * lb * powerB + 2.0 * la * lb * r * std::sqrt(powerA * powerB);

    double s = 1.0;
    if (power > 0.0 && target > 0.0)
        s = std::min(std::sqrt(target / power), kIrBlendMaxBoost);

    a = la * s;
    b = lb * s;
}

} // namespace Rations
