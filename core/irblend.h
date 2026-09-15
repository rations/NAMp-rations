// The cabinet page's blend between two impulse responses.
//
// D10 says one IR is the normal case and the second is opt-in, so the endpoints are fixed and not
// negotiable: at 0 the user hears IR A exactly as they would with slot B empty, and at 1 they hear
// IR B exactly as they would with slot A empty. The only open question is the shape in between,
// and it is open because the honest answer depends on the two files:
//
//   * Two mic positions on the SAME cabinet are strongly correlated. Their sum is nearly coherent,
//     so an amplitude-complementary mix (a + b = 1) holds the level flat and an equal-power mix
//     (a^2 + b^2 = 1) puts a +3 dB bump in the middle of the dial. This is the parent plug-in's
//     D12 situation exactly.
//   * Two DIFFERENT cabinets are close to uncorrelated. Their powers add rather than their
//     amplitudes, so it is the other way round: equal-power is flat and amplitude-complementary
//     digs a -3 dB hole in the middle.
//
// A fixed curve therefore cannot be right for both, and which one the user has loaded is not
// something the code can be told - so it is measured. Both impulse responses are known on the
// message thread, so their normalized inner product rho (the correlation of their outputs for the
// same input) is measured once at load time, and the weights are normalized against it:
//
//     a = (1 - x) * s,  b = x * s
//     s = sqrt( target(x) / power(x) )
//     target(x) = (1 - x) * PA + x * PB          the straight line between the two endpoints
//     power(x)  = (1-x)^2 PA + x^2 PB + 2 x (1-x) rho sqrt(PA PB)
//
// s is exactly 1 at both ends, so the endpoints stay untouched by construction. At rho = 1 with
// equal powers this collapses to a + b = 1, and at rho = 0 it collapses to a^2 + b^2 = 1 - the two
// textbook answers fall out as the two extremes of the same formula rather than being chosen
// between. Unequal-level IRs are handled too: the target is the straight line between whatever the
// endpoints actually are, not a flat line, because a blend that flattened a level difference the
// user can hear at the ends would be correcting something they did not ask to have corrected.
//
// The measurement that settles rho is in tools/rations_ircheck.cpp, run over real cabinet IRs.

#pragma once

#include "ImpulseResponse.h"

#include <cstddef>

namespace Rations
{

// The profile is the IR's response to a fixed WEIGHTED stimulus, not to a bare impulse, and that
// distinction is the difference between the measurement working and not working.
//
// rho has to answer "how correlated are these two cabinets' outputs for the signal a guitar amp
// actually sends them". A unit impulse answers it for a flat-spectrum signal, which weights 8 kHz
// as heavily as 200 Hz - and two cabinets that disagree completely up top can be near-identical
// down where the energy is. Measured that way against real cabinet IRs, a V30 4x12 blended with a
// Bassman 2x15 came out 0.89 dB off the line: the broadband rho said "uncorrelated" and the audible
// result said otherwise. Tilting the stimulus so it is weighted the way musical signal energy is
// distributed measures the correlation where the level actually comes from, and takes the worst of
// those eight real pairs to 0.51 dB. The tilt itself is measured - see irblend.cpp.
//
// The neat part is that no noise generator is needed. For an input whose power spectrum is |P|^2,
// the expected output correlation of two filters is exactly the plain inner product of h_A*p and
// h_B*p - so feeding each IR the weighting filter's own impulse response gives the weighted
// statistic directly, deterministically, in one pass and with no averaging.
inline constexpr int kIrProfileStimulus = 8192; // the weighting filter's impulse response
inline constexpr int kIrProfileSamples = 16384; // stimulus + the IR's own 8192-sample truncation

// Fill `buf` with the profiling stimulus at the given sample rate: the first kIrProfileStimulus
// samples are the weighting filter's impulse response, the rest are zeros so the convolution tail
// is captured. `n` must be kIrProfileSamples.
void fillIrProfileStimulus(double *buf, size_t n, double sampleRate);

// Two out-of-polarity IRs genuinely cancel in the middle of the dial, and no gain curve can undo
// that - the formula's denominator goes to zero and s runs away. So rho is floored and the boost
// is capped: past this point the blend stops pretending it can rescue the level and simply lets
// the cancellation be audible, which is the truth of what those two files do together.
inline constexpr double kIrBlendRhoFloor = -0.5;
inline constexpr double kIrBlendMaxBoost = 2.0; // +6 dB, a hard ceiling on s

// The measured relationship between the two loaded impulse responses. Computed on the message
// thread at load time; read on the audio thread, where it costs one sqrt per block.
struct IrBlend {
    double powerA = 0.0; // energy of IR A's impulse response
    double powerB = 0.0; // energy of IR B's impulse response
    double rho = 0.0;    // normalized inner product, -1 .. 1
    bool measured = false;

    // Blend weights at dial position x in 0 .. 1. Always exact at the endpoints, whatever the
    // measurement says, and safe with an unmeasured or degenerate profile (it falls back to the
    // linear mix, which is the right answer for the correlated case and a 3 dB dip for the other -
    // wrong, but bounded and never louder than the endpoints).
    void weights(double x, double &a, double &b) const;
};

// Profile two impulse responses against each other. The arrays are the responses themselves,
// captured by running the weighted stimulus through the two live ImpulseResponse objects, so they
// carry the resampling, the gain and the truncation that the audio path will actually apply.
IrBlend measureIrBlend(const double *ha, const double *hb, size_t n);

// The whole cabinet stage, and D10's three cases in one place. Either slot may be null.
//
//   both filled     -> the two IRs mixed at the measured weights, written through `mixPtr`
//   exactly one     -> that IR at unity, in ONE Process call with the blend never consulted, so a
//                      user who leaves the second slot empty gets bit-identical audio to a
//                      single-slot build; `position` does not reach the signal at all
//   neither         -> `input` handed straight back; the stage is bypassed
//
// Real-time: allocates nothing, provided both IRs have been run once at this block size off the
// audio thread (see the profiling pass, which doubles as that warm-up) and `*mixPtr` points at a
// buffer of at least `numFrames`. It lives here rather than inline in the processor so the
// allocation harness and the offline proofs drive the shipped stage instead of a paraphrase.
DSP_SAMPLE **processCabinet(dsp::ImpulseResponse *a, dsp::ImpulseResponse *b, const IrBlend &blend,
                            double position, DSP_SAMPLE **input, size_t numFrames,
                            DSP_SAMPLE **mixPtr);

} // namespace Rations
