// rations_pedalcheck — measures the pedalboard's DSP against the circuit it claims to model.
//
// Links the pedal sources directly rather than driving the built bundle, and that is deliberate.
// The PRE chain sits upstream of the amp models, so anything measured through the plug-in's output
// has four neural captures and a cabinet convolution in front of it; what is under test here is
// one circuit, and the only way to see it is to look at it on its own. The bundle-level questions
// this cannot answer — that the pedal is reachable through a real parameter queue, and that the
// reported latency does not move when a footswitch is stomped — are asserted in rations_offline,
// which does drive the bundle.
//
// EVERY EXPECTED NUMBER BELOW IS DERIVED FROM THE CIRCUIT, NOT FROM A PREVIOUS RUN OF THIS CODE.
// A test that records what the implementation happens to do would pass forever and prove nothing;
// these come out of David T. Yeh's component values and equations (Stanford CCRMA 2009, §2.4,
// in third_party/refs/pedals), computed here from the same constants the DSP reads.
//
// usage: rations_pedalcheck [--verbose]

#include "pedals/boost.h"
#include "pedals/chorus.h"
#include "pedals/delay.h"
#include "pedals/flanger.h"
#include "pedals/primitives.h"
#include "pedals/reverb.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Rations;
using namespace Rations::pedals;

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

// Set ABOVE the measured worst case rather than at it - the same rule the IR blend gate follows -
// so this reports a regression and not the run-to-run noise of whichever condition happens to be
// worst. The worst measured is -51.1 dB, at Drive 10 with a 0.5 V 3.3 kHz tone: maximum gain into
// the diodes, at a frequency high enough that its harmonics run off the top of the 4x domain, at
// a level 6 dB below full scale. It is deliberately a brutal stimulus and not a musical one.
constexpr double kAliasGateDb = -46.0;

// A backstop on how far a cubic delay read may drift from a 32-tap windowed-sinc read of the same
// moving position, above the measured worst of -47.5 dB (the Flanger with every knob at maximum,
// on a 5 kHz tone) rather than at it — the alias gate's rule, so this reports a regression and not
// the run-to-run noise of whichever case is worst today. It is NOT the answer to the project
// plan's question about whether cubic is enough under fast modulation; that answer is the
// comparison against the same read standing still, which needs no constant at all. See the
// interpolation section of main().
constexpr double kInterpGateDb = -42.0;

bool gVerbose = false;
int gFailures = 0;

void fail(const char *what, double got, double want, double tol, const char *unit)
{
    fprintf(stderr, "pedalcheck: %s is %.4f %s but the circuit says %.4f +- %.4f\n", what, got,
            unit, want, tol);
    ++gFailures;
}

// Reports pass or fail, and prints the measurement either way: a gate that only speaks when it is
// unhappy makes a regression in the OTHER direction invisible.
void check(const char *what, double got, double want, double tol, const char *unit = "dB")
{
    if (std::fabs(got - want) > tol)
        fail(what, got, want, tol, unit);
    else if (gVerbose)
        printf("    %-46s %9.4f %-4s (want %.4f +- %.4f)\n", what, got, unit, want, tol);
}

//------------------------------------------------------------------------------------------------
// A Boost, driven exactly as the chain drives it: parameters in kPedalParams order, engaged, and
// fed in blocks of the size the processor uses.
class Rig
{
public:
    Rig(double drive, double tone, double level, double rate = kRate) : mRate(rate)
    {
        mP[0] = 1.0; // the footswitch: slice entry 0, by the static_assert in rationsids.h
        mP[Boost::kDrive] = drive;
        mP[Boost::kTone] = tone;
        mP[Boost::kLevel] = level;
        mBoost.prepare(rate, kBlock);
        mBoost.setEngaged(true);
        mBoost.setParams(mP);
        mBoost.reset();
    }

    // Runs `n` samples through, in blocks. The engage ramp is settled first so that what comes
    // back is the pedal and not the footswitch crossing.
    void run(std::vector<double> &buf)
    {
        for (size_t i = 0; i < buf.size(); i += kBlock) {
            const int n = static_cast<int>(std::min<size_t>(kBlock, buf.size() - i));
            mBoost.setParams(mP);
            mBoost.process(buf.data() + i, nullptr, n);
        }
    }

    // Feeds silence until the engage ramp and the control smoothers have landed.
    void settle(double seconds = 0.5)
    {
        std::vector<double> quiet(static_cast<size_t>(mRate * seconds), 0.0);
        run(quiet);
    }

    double rate() const { return mRate; }

private:
    double mP[4] = {0, 0, 0, 0};
    double mRate;
    Boost mBoost;
};

//------------------------------------------------------------------------------------------------
// The response at one frequency, by driving a steady sine and correlating against it — a Goertzel
// in all but name. Amplitude is chosen by the caller: this circuit is nonlinear, so "the frequency
// response" only means anything at a stated level.
struct Response {
    double gainDb;
    double phaseDeg;
};

Response responseAt(Rig &rig, double freqHz, double ampVolts)
{
    const double rate = rig.rate();
    const int settle = static_cast<int>(rate * 0.30);
    const int measure = static_cast<int>(rate * 0.20);
    std::vector<double> buf(static_cast<size_t>(settle + measure));
    const double w = 2.0 * kPi * freqHz / rate;
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = ampVolts * std::sin(w * static_cast<double>(i));
    rig.run(buf);

    double re = 0.0, im = 0.0;
    for (int i = 0; i < measure; ++i) {
        const double t = w * static_cast<double>(settle + i);
        re += buf[static_cast<size_t>(settle + i)] * std::sin(t);
        im += buf[static_cast<size_t>(settle + i)] * std::cos(t);
    }
    const double norm = 2.0 / static_cast<double>(measure);
    const std::complex<double> h(re * norm, im * norm);
    return {20.0 * std::log10(std::max(1e-30, std::abs(h) / ampVolts)),
            std::arg(h) * 180.0 / kPi};
}

//------------------------------------------------------------------------------------------------
// The CLIPPING STAGE below the diode knee, analytically, from eq. 2.10/2.12/2.13:
//
//     V_o/V_i = 1 + (R2/R1) · s/(s+wz) · wp/(s+wp)
//
// The leading 1 is eq. 2.13's clean path; wz = 1/(R1·Cz) is the 720 Hz bass cut, which is in the
// clipped path ONLY; wp = 1/(R2·Cc) is the drive-dependent pole the ODE exists to carry.
//
// Written once and used by every expectation below, because the first version of this file wrote
// each expected number by hand as "1 + R2/R1" and got three of them wrong: at Drive 10 the pole is
// at 5.7 kHz, so there is no frequency in the audio band where the plateau is actually reached,
// and a measurement taken at 3 kHz was being compared with a plateau the circuit never gets to.
double clipStageDb(double drivePlain, double freqHz)
{
    // THE DIODES ARE NEVER OUT OF CIRCUIT. 2·Is·sinh(V/Vt) linearises to a resistance
    // rd = Vt/(2·Is) = 8.99 MOhm about the origin, which shunts R2 at every level — so the
    // small-signal gain is 1 + (R2 ∥ rd)/R1, not 1 + R2/R1, and the difference is a constant
    // −0.05 dB at Drive 0 rising to −0.51 dB at Drive 10 because R2 grows towards rd.
    //
    // This was the model disagreeing with the reference by exactly that much, at every level and
    // every frequency, and the reference being wrong. It is worth keeping because it is the one
    // place a "small-signal" test can quietly stop being one.
    const double rd = ts9::kVt / (2.0 * ts9::kIs);
    const double r2raw = ts9::kR2Fixed + (drivePlain * 0.1) * ts9::kR2Drive;
    const double r2 = r2raw * rd / (r2raw + rd);
    const std::complex<double> s(0.0, 2.0 * kPi * freqHz);
    const double wz = 1.0 / (ts9::kR1 * ts9::kCz);
    const double wp = 1.0 / (r2 * ts9::kCc);
    return 20.0 * std::log10(
        std::abs(1.0 + (r2 / ts9::kR1) * (s / (s + wz)) * (wp / (s + wp))));
}

// Fig. 2.26's output divider, which is a constant and is in every measurement.
double levelDb(double levelPlain)
{
    return 20.0 * std::log10(std::max(1e-30, (levelPlain * 0.1) * ts9::kLevelPot /
                                                 (ts9::kLevelPot + ts9::kLevelSeries)));
}

//------------------------------------------------------------------------------------------------
// The analog tone stage, straight from eq. 2.14, evaluated on the jw axis. This is the reference
// the discrete filter is measured against, and it is written out from the equation rather than
// from the implementation so that a transcription error in one does not hide in the other.
double toneAnalogDb(double t, double freqHz)
{
    const double rl = t * ts9::kTonePot;
    const double rr = (1.0 - t) * ts9::kTonePot;
    const double rpar = rl * rr / (rl + rr);
    const double y = (rl + rr) * (ts9::kToneRz + rpar);
    const double wz = 1.0 / (ts9::kToneCz * (ts9::kToneRz + rpar));
    const double wp =
        1.0 / (ts9::kToneCs * (ts9::kToneRs * ts9::kToneRi / (ts9::kToneRs + ts9::kToneRi)));
    const double x = (rr / (rl + rr)) * wz;

    const std::complex<double> s(0.0, 2.0 * kPi * freqHz);
    const std::complex<double> rf = rl * ts9::kToneRf + y;
    const std::complex<double> num = rf * (s + (y / rf) * wz);
    const std::complex<double> den =
        y * ts9::kToneRs * ts9::kToneCs * (s + wp) * (s + wz) + x * s;
    return 20.0 * std::log10(std::abs(num / den));
}

//------------------------------------------------------------------------------------------------
// Radix-2 FFT, in place, so a 32768-point control run costs nothing to analyse.
void fft(std::vector<std::complex<double>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

//------------------------------------------------------------------------------------------------
// Harmonic and alias content of a steady sine, ON AN EXACT DFT GRID.
//
// The fundamental is placed at bin `p` of an `n`-point window, so f0 = rate·p/n exactly and the
// window is a whole number of periods. No window function is used and none is wanted: with an
// exact grid there is no leakage to taper, and a window would smear every harmonic across its
// neighbours, which is precisely the distinction being measured.
//
// TWO THINGS THIS GETS RIGHT THAT THE FIRST VERSION DID NOT, both found by the numbers being
// impossible rather than merely wrong.
//
// The grid has to be EXACT. A 220 Hz sine in a window that is not a whole number of periods leaks,
// and the leak sat at −88 dB and was being read as the even-harmonic content of a symmetric diode
// pair — a floor produced by the measurement, reported as a property of the circuit.
//
// And p and n must not share the fundamental's own aliasing. At f0 = 4 kHz with a 48 kHz rate,
// every harmonic folds back onto a multiple of 4 kHz, because 48 is 12 times 4 — so aliases land
// exactly on harmonic bins, are counted as harmonics, and the alias floor reads as −266 dB no
// matter how bad it is. Choosing p coprime with n scatters the folded harmonics off the harmonic
// grid, where they can be seen. Everything at or above Nyquist is alias BY CONSTRUCTION: a
// harmonic that high cannot be represented, so wherever its energy appears, it is folded.
struct Spectrum {
    std::vector<double> harmonicDb; // index 1 = fundamental, relative to the fundamental
    double aliasDb;                 // total non-harmonic energy, relative to the fundamental
    double f0;
};

Spectrum analyse(Rig &rig, int n, int p, double ampVolts, int maxHarmonic)
{
    const double rate = rig.rate();
    const double f0 = rate * static_cast<double>(p) / static_cast<double>(n);
    const int settle = static_cast<int>(rate * 0.30);
    std::vector<double> buf(static_cast<size_t>(settle + n));
    const double w = 2.0 * kPi * static_cast<double>(p) / static_cast<double>(n);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = ampVolts * std::sin(w * static_cast<double>(i));
    rig.run(buf);

    std::vector<std::complex<double>> spec(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        spec[static_cast<size_t>(i)] = buf[static_cast<size_t>(settle + i)];
    fft(spec);
    std::vector<double> mag(static_cast<size_t>(n / 2), 0.0);
    for (int k = 0; k < n / 2; ++k)
        mag[static_cast<size_t>(k)] =
            std::abs(spec[static_cast<size_t>(k)]) / static_cast<double>(n);

    Spectrum out;
    out.f0 = f0;
    const double fund = mag[static_cast<size_t>(p)];
    out.harmonicDb.assign(static_cast<size_t>(maxHarmonic) + 1, -300.0);
    // Only harmonics that FIT below Nyquist are legitimate; the rest have folded and are alias.
    std::vector<bool> legit(static_cast<size_t>(n / 2), false);
    for (int h = 1; h * p < n / 2; ++h) {
        legit[static_cast<size_t>(h * p)] = true;
        if (h <= maxHarmonic && fund > 0.0)
            out.harmonicDb[static_cast<size_t>(h)] =
                20.0 * std::log10(std::max(1e-300, mag[static_cast<size_t>(h * p)] / fund));
    }

    double alias = 0.0;
    for (int k = 1; k < n / 2; ++k)
        if (!legit[static_cast<size_t>(k)])
            alias += mag[static_cast<size_t>(k)] * mag[static_cast<size_t>(k)];
    out.aliasDb = 20.0 * std::log10(std::max(1e-300, std::sqrt(alias) / std::max(1e-300, fund)));
    return out;
}

//==================================================================================================
// P5 — the Chorus and the Flanger.
//
// The references are Julius O. Smith III, *Physical Audio Signal Processing* ("Flanging",
// "Flanger Speed and Excursion", "Flanger Feedback Control", "Chorus Effect"), copies in
// third_party/refs/pedals/pasp/. As above, every expected number is computed here from the
// published relation, never recorded from a previous run.
//==================================================================================================

// A pedal driven exactly as the chain drives it: parameters in kPedalParams order, engaged, fed in
// blocks of the size the processor uses. One template rather than a Rig per pedal, because what
// differs between them is the length of the slice and nothing else.
template <typename P, int NParams>
class Board
{
public:
    explicit Board(double rate = kRate) : mRate(rate)
    {
        mP[0] = 1.0; // the footswitch: slice entry 0, by the static_assert in rationsids.h
        mPedal.prepare(rate, kBlock);
        mPedal.setEngaged(true);
    }

    void set(int idx, double v) { mP[idx] = v; }

    // Clears the DSP and lands the smoothers on the current knob positions, so what follows is the
    // pedal at those settings rather than the pedal on its way to them.
    void restart()
    {
        mPedal.reset();
        mPedal.setParams(mP); // the first push after a reset snaps; see chorus.h
    }

    void run(double *l, double *r, size_t n)
    {
        for (size_t i = 0; i < n; i += kBlock) {
            const int k = static_cast<int>(std::min<size_t>(kBlock, n - i));
            mPedal.setParams(mP);
            mPedal.process(l + i, r ? r + i : nullptr, k);
        }
    }

    void run(std::vector<double> &l) { run(l.data(), nullptr, l.size()); }

    void silence(size_t n)
    {
        std::vector<double> q(n, 0.0);
        run(q);
    }

    double rate() const { return mRate; }
    P &pedal() { return mPedal; }

private:
    double mP[NParams] = {0};
    double mRate;
    P mPedal;
};

using ChorusBoard = Board<Chorus, 4>;
using FlangerBoard = Board<Flanger, 5>;
using DelayBoard = Board<Delay, 7>;

//------------------------------------------------------------------------------------------------
// THE DELAY's LOOP FILTER, WRITTEN OUT FROM ITS DECLARED CORNERS, and these three numbers are
// LITERALS ON PURPOSE. Reading delaydef's own constants for the expectation is what the Chorus's
// first frozen-tap check did, and it had no teeth at all: moving a corner moved both sides of the
// comparison and everything still passed. A corner frequency is a design decision rather than
// something derivable from physics, so the only way to pin it is to write it down twice and make
// the two disagree when one of them moves.
constexpr double kExpToneLoHz = 800.0;
constexpr double kExpToneHiHz = 12000.0;
constexpr double kExpLoopHpHz = 40.0;

// Where Tone puts the low-pass, geometrically over that span. The knob is 0..10.
double toneCornerHz(double tonePlain)
{
    return kExpToneLoHz * std::pow(kExpToneHiHz / kExpToneLoHz, tonePlain * 0.1);
}

// H(e^jw) of the loop filter: a one-pole low-pass, then that low-pass's complement as the
// high-pass, which is how the DSP builds it (`return mLp - mHp`). Derived from the corner
// frequencies above, not from the DSP's own variables, so a transcription error in one cannot
// hide in the other.
std::complex<double> loopFilterH(double tonePlain, double w)
{
    const std::complex<double> z = std::exp(std::complex<double>(0.0, -w));
    const double aLp = 1.0 - std::exp(-2.0 * kPi * toneCornerHz(tonePlain) / kRate);
    const double aHp = 1.0 - std::exp(-2.0 * kPi * kExpLoopHpHz / kRate);
    const std::complex<double> lp = aLp / (1.0 - (1.0 - aLp) * z);
    const std::complex<double> hp = aHp / (1.0 - (1.0 - aHp) * z);
    return lp * (1.0 - hp);
}

// The phase of a wet impulse response at one low FFT bin.
//
// This is how the delay time is measured, and the reason it is not the Flanger's echo centroid is
// worth stating: the loop's high-pass has ZERO gain at DC, so the impulse response sums to zero
// and its first moment is 0/0. Every DC-referenced moment is degenerate here, which the centroid
// method has no way to report.
//
// A single bin low enough that w*D stays under pi needs no unwrapping: arg(Y_k) = -w_k*D +
// arg(H(w_k)), so D falls straight out. At 2^19 points a four-second delay is 2.3 radians, which
// is inside pi with room to spare.
constexpr int kPhaseFftLog2 = 19;
constexpr int kPhaseBin = 1;

double wetPhaseAtBin(const std::vector<double> &ir)
{
    const size_t n = size_t(1) << kPhaseFftLog2;
    std::vector<std::complex<double>> spec(n, {0.0, 0.0});
    for (size_t i = 0; i < n && i < ir.size(); ++i)
        spec[i] = ir[i];
    fft(spec);
    return std::arg(spec[kPhaseBin]);
}

constexpr double kPhaseBinW = 2.0 * kPi * kPhaseBin / double(size_t(1) << kPhaseFftLog2);

// The wet impulse response of a Delay at the settings already on the board: Mix at 100 % so the
// output is the line read and nothing else, and Feedback at 0 so there is exactly one echo.
std::vector<double> delayWetIr(DelayBoard &b, size_t n)
{
    std::vector<double> l(n, 0.0), r(n, 0.0);
    l[0] = r[0] = 1.0;
    b.run(l.data(), r.data(), n);
    return l;
}

// Delay in samples, from the phase at that bin with the loop filter's own phase taken off.
//
// ONE SUBTRACTION AND NOT TWO, which the first version of this got wrong in a way worth keeping:
// it removed arg(H) and THEN removed the filter's group delay as well, and every delay in the
// pedal came out 4.0205 ms short — identically, at every setting, which is what said it was the
// measurement rather than the DSP. 4.0205 ms is 193 samples, and the loop filter's group delay at
// this bin is 190.5 (the 40 Hz high-pass) + 0.5 (its zero at z = 1) + 2.0 (the low-pass) = 193.0.
// Subtracting arg(H) IS subtracting the group delay, because arg(H) = -w*tau near DC; doing both
// counts it twice. The model-free difference check below is what proves this subtraction is not
// quietly wrong in some other way, since the filter cancels there without being modelled at all.
double measuredDelaySamples(DelayBoard &b, double tonePlain, size_t n)
{
    const std::vector<double> ir = delayWetIr(b, n);
    const double phase = wetPhaseAtBin(ir);
    return (std::arg(loopFilterH(tonePlain, kPhaseBinW)) - phase) / kPhaseBinW;
}

//------------------------------------------------------------------------------------------------
// Where a delay line's echo actually landed, in samples, from the impulse response.
//
// This is EXACT for the interpolator in use, and that is a property of Catmull-Rom rather than a
// convenient approximation. Reading a unit impulse at fractional delay D puts the kernel's samples
// at the integers around D, and for any kernel that is (a) a partition of unity and (b) symmetric
// about its own centre, sum(w) = 1 and sum(n*w) = D exactly. Catmull-Rom is both. So the first
// moment of the echo IS the delay, to floating point, with no peak-picking and no interpolation of
// the measurement itself.
//
// `skip` drops the direct signal: the Flanger's output is 0.5*(x + d), so sample 0 carries the
// input's own impulse and is not part of the echo.
double echoCentroid(const std::vector<double> &ir, size_t skip)
{
    double sum = 0.0, moment = 0.0;
    for (size_t n = skip; n < ir.size(); ++n) {
        sum += ir[n];
        moment += static_cast<double>(n) * ir[n];
    }
    if (std::fabs(sum) < 1e-12)
        return 0.0;
    return moment / sum;
}

//------------------------------------------------------------------------------------------------
// |H(f)| of an impulse response, on the FFT grid. Used to find comb notches.
std::vector<double> magnitudeOf(const std::vector<double> &ir, int n)
{
    std::vector<std::complex<double>> spec(static_cast<size_t>(n), {0.0, 0.0});
    for (int i = 0; i < n && static_cast<size_t>(i) < ir.size(); ++i)
        spec[static_cast<size_t>(i)] = ir[static_cast<size_t>(i)];
    fft(spec);
    std::vector<double> mag(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k)
        mag[static_cast<size_t>(k)] = std::abs(spec[static_cast<size_t>(k)]);
    return mag;
}

// Frequencies of the local minima of |H|, refined by fitting a parabola through the three bins
// around each one — a comb notch is far narrower than a bin, so the bin index alone would quantise
// the answer to 11.7 Hz and swamp what is being measured.
std::vector<double> notchesOf(const std::vector<double> &mag, double rate, int n, double maxHz)
{
    std::vector<double> out;
    const double binHz = rate / static_cast<double>(n);
    const size_t last = std::min(mag.size() - 1, static_cast<size_t>(maxHz / binHz));
    for (size_t k = 1; k < last; ++k) {
        if (!(mag[k] < mag[k - 1] && mag[k] < mag[k + 1]))
            continue;
        // Parabolic interpolation in dB, which is where a notch is locally quadratic.
        const double a = 20.0 * std::log10(std::max(1e-300, mag[k - 1]));
        const double b = 20.0 * std::log10(std::max(1e-300, mag[k]));
        const double c = 20.0 * std::log10(std::max(1e-300, mag[k + 1]));
        const double denom = a - 2.0 * b + c;
        const double delta = (std::fabs(denom) > 1e-12) ? 0.5 * (a - c) / denom : 0.0;
        out.push_back((static_cast<double>(k) + delta) * binHz);
    }
    return out;
}

//------------------------------------------------------------------------------------------------
// A 32-tap Blackman-windowed-sinc read of a delay line, as the reference the two shipping
// interpolators are measured against. Far too expensive for the audio path and not on it: it exists
// so that "cubic is good enough" is a number rather than an opinion.
double sincRead(const std::vector<double> &x, double t, int halfTaps = 16)
{
    const long i0 = static_cast<long>(std::floor(t));
    const double f = t - static_cast<double>(i0);
    double acc = 0.0;
    for (int k = -halfTaps + 1; k <= halfTaps; ++k) {
        const long idx = i0 + k;
        if (idx < 0 || static_cast<size_t>(idx) >= x.size())
            continue;
        const double d = static_cast<double>(k) - f;
        const double s = (std::fabs(d) < 1e-12) ? 1.0 : std::sin(kPi * d) / (kPi * d);
        // Blackman window over the 2*halfTaps span.
        const double w = 0.42
                         - 0.5 * std::cos(kPi * (d + halfTaps) / halfTaps)
                         + 0.08 * std::cos(2.0 * kPi * (d + halfTaps) / halfTaps);
        acc += x[static_cast<size_t>(idx)] * s * w;
    }
    return acc;
}

double rmsDb(const std::vector<double> &v, size_t from = 0)
{
    double acc = 0.0;
    for (size_t i = from; i < v.size(); ++i)
        acc += v[i] * v[i];
    const size_t n = v.size() - from;
    return 20.0 * std::log10(std::max(1e-300, std::sqrt(acc / std::max<size_t>(1, n))));
}

// True if any sample is subnormal — the state a decaying feedback loop falls into, where each
// operation costs a hundred times what it should.
bool hasSubnormal(const std::vector<double> &v)
{
    for (double x : v)
        if (x != 0.0 && std::fabs(x) < 2.2250738585072014e-308)
            return true;
    return false;
}

//==================================================================================================
// P7 — the Reverb.
//
// The references are PASP's "Freeverb" and the two pages under it, "Freeverb Main Loop" and
// "Lowpass-Feedback Comb Filter", copies in third_party/refs/pedals/pasp/. What is under test is
// the WRAPPER — the map from the four knobs onto the vendored engine's two settings, the pre-delay,
// the level compensation and the mix — because the engine itself is Cockos' file and this tree does
// not get to change it. That is not a weaker test than the other four pedals get: every claim the
// panel makes is a claim about the wrapper, and PASP states what the engine's settings mean
// precisely enough to check every one of them.
//==================================================================================================

using ReverbBoard = Board<Reverb, 5>;

// THE ENGINE'S STRUCTURE, WRITTEN OUT A SECOND TIME ON PURPOSE. These are read out of
// deps/wdl/verbengine.h — `wdl_verb__combtunings[]`, the six-entry `wdl_verb__allpasstunings[]`,
// the 0.5 fed to every `setfeedback` in Reset(), and Freeverb's 0.015 that WDL applies to the
// engine's input — and they are duplicated here rather than included for the reason the Delay's
// tone corners are: a constant that both sides of a comparison read cannot be checked by that
// comparison. If a re-vendored verbengine.h ever changes its tuning table, this is what says so.
constexpr int kVerbCombTunings[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617, 1685, 1748};
constexpr int kVerbCombCount = static_cast<int>(sizeof(kVerbCombTunings) / sizeof(int));
constexpr int kVerbAllpassCount = 6;
constexpr double kVerbAllpassG = 0.5;
constexpr double kVerbFixedGain = 0.015;
constexpr double kVerbTuningRate = 44100.0;

// The Decay knob's declared span, and Tone's declared direction. Literals, same rule.
constexpr double kExpT60MinSec = 0.4;
constexpr double kExpT60MaxSec = 6.0;

double expectedT60(double decayPlain)
{
    return kExpT60MinSec
           * std::pow(kExpT60MaxSec / kExpT60MinSec, std::clamp(decayPlain * 0.1, 0.0, 1.0));
}

// The engine's comb lengths at a given rate, reproducing its own `(int)(tuning * srate/44100)`.
std::vector<int> combLengths(double rate)
{
    std::vector<int> n;
    for (int t : kVerbCombTunings)
        n.push_back(static_cast<int>(static_cast<double>(t) * rate / kVerbTuningRate));
    return n;
}

// T60 by Schroeder backward integration and a least-squares fit over the standard -5 to -35 dB
// window, extrapolated to -60. The window is part of the definition and not a detail: the ensemble
// ratio below is a property of it, and a different window would give a different constant.
double t60Of(const std::vector<double> &h, double rate)
{
    const size_t n = h.size();
    std::vector<double> e(n + 1, 0.0);
    for (size_t i = n; i-- > 0;)
        e[i] = e[i + 1] + h[i] * h[i];
    if (e[0] <= 0.0)
        return -1.0;
    auto db = [&](size_t i) { return 10.0 * std::log10(std::max(1e-300, e[i] / e[0])); };
    long i1 = -1, i2 = -1;
    for (size_t i = 0; i < n; ++i) {
        if (i1 < 0 && db(i) <= -5.0)
            i1 = static_cast<long>(i);
        if (i2 < 0 && db(i) <= -35.0) {
            i2 = static_cast<long>(i);
            break;
        }
    }
    if (i1 < 0 || i2 <= i1)
        return -1.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    const double m = static_cast<double>(i2 - i1 + 1);
    for (long i = i1; i <= i2; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double y = db(static_cast<size_t>(i));
        sx += t;
        sy += y;
        sxx += t * t;
        sxy += t * y;
    }
    const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    return (slope < 0.0) ? -60.0 / slope : -1.0;
}

// THE ENSEMBLE RATIO, DERIVED HERE FROM THE TEN COMB LENGTHS AND NOTHING ELSE.
//
// PASP's LBCF gives the decay of ONE comb exactly. Ten of them in parallel decay at ten different
// rates, so a T30 fit over the -5 to -35 dB window lands short of what the longest comb alone would
// give. That shortfall is what reverbdef::kEnsembleT60Ratio corrects for, and this function is the
// independent second derivation of it that the header's prose promises.
//
// Comb i's ENERGY decays as exp(2*t*fs*ln(f)/N_i), so writing u = t*fs*|ln f| the bank's energy is
// sum(exp(-2u/N_i)) and its Schroeder integral is sum((N_i/2)*exp(-2u/N_i)) in closed form — no
// impulse response and no reverb needed. Because u carries both t and f, the curve only stretches
// when f changes, so the fitted-to-asymptotic ratio is a constant of the lengths and the window.
double ensembleT60Ratio(double rate)
{
    const std::vector<int> n = combLengths(rate);
    int nmax = 0;
    for (int v : n)
        nmax = std::max(nmax, v);
    auto sch = [&](double u) {
        double a = 0.0;
        for (int v : n)
            a += static_cast<double>(v) * std::exp(-2.0 * u / static_cast<double>(v));
        return a;
    };
    const double s0 = sch(0.0);
    auto db = [&](double u) {
        const double v = sch(u);
        return (v > 0.0) ? 10.0 * std::log10(v / s0) : -1e9;
    };
    auto solve = [&](double target) {
        double lo = 0.0, hi = 20.0 * static_cast<double>(nmax);
        for (int k = 0; k < 200; ++k) {
            const double mid = 0.5 * (lo + hi);
            (db(mid) > target ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    };
    const double u1 = solve(-5.0), u2 = solve(-35.0);
    const int m = 4000;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (int k = 0; k < m; ++k) {
        const double u = u1 + (u2 - u1) * static_cast<double>(k) / static_cast<double>(m - 1);
        const double y = db(u);
        sx += u;
        sy += y;
        sxx += u * u;
        sxy += u * y;
    }
    const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    // The asymptote is the longest comb on its own: its AMPLITUDE falls by 60 dB after
    // 3*ln(10)*N_max of u, which is the same relation PASP's "Achieving Desired Reverberation
    // Times" states as G = 10^(-3/n60) per sample.
    return (-60.0 / slope) / (3.0 * std::log(10.0) * static_cast<double>(nmax));
}

// The engine's broadband amplitude gain BEFORE the wrapper's kWetNorm, derived from the structural
// constants above. For a broadband input a filter's power gain is the sum of the squares of its
// impulse response, which is 1 per comb once the wrapper's sqrt(1 - f^2) compensation is in, and
// 1 + 1/(1 - g^2) per Freeverb "allpass" — see reverb.h for the same derivation in prose.
//
// IT MUST NOT READ reverbdef::kWetNorm, and that is not fastidiousness. The first version of this
// multiplied it in and compared the result against the measurement, so doubling the pedal's
// kWetNorm moved both sides of the comparison by 6 dB and the deliberate fault walked straight
// through. It is the Chorus's frozen-tap mistake exactly, in a third place. What the check does now
// is derive what kWetNorm SHOULD be and compare the constant itself, then assert the measured gain
// is unity — two independent statements, neither of which can absorb an error in the other.
double predictedRawWetGain()
{
    const double perAllpass = 1.0 + 1.0 / (1.0 - kVerbAllpassG * kVerbAllpassG);
    return kVerbFixedGain * std::sqrt(static_cast<double>(kVerbCombCount))
           * std::pow(perAllpass, 0.5 * static_cast<double>(kVerbAllpassCount));
}

// The wet impulse response of one channel, at Mix 100 %.
std::vector<double> verbWetIr(ReverbBoard &b, size_t n, bool rightChannel = false)
{
    std::vector<double> l(n, 0.0), r(n, 0.0);
    l[0] = r[0] = 1.0;
    b.run(l.data(), r.data(), n);
    return rightChannel ? r : l;
}

// A two-pole band-pass, built as the difference of two two-pole low-passes, for splitting a tail
// into the bands PASP's damping sentence is about. Zero-phase would be better and is not needed:
// what is measured is a DECAY RATE, and a fixed filter delays the whole curve without tilting it.
std::vector<double> bandOf(const std::vector<double> &x, double rate, double loHz, double hiHz)
{
    const double aHi = 1.0 - std::exp(-2.0 * kPi * hiHz / rate);
    const double aLo = 1.0 - std::exp(-2.0 * kPi * loHz / rate);
    std::vector<double> y(x.size());
    double s1 = 0.0, s2 = 0.0, s3 = 0.0, s4 = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        s1 += aHi * (x[i] - s1);
        s2 += aHi * (s1 - s2);
        s3 += aLo * (s2 - s3);
        s4 += aLo * (s3 - s4);
        y[i] = s2 - s4;
    }
    return y;
}

// Normalized cross-correlation at zero lag. 1.0 is the same signal, 0.0 is no shared structure.
double correlation(const std::vector<double> &a, const std::vector<double> &b)
{
    double sa = 0.0, sb = 0.0, sab = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        sa += a[i] * a[i];
        sb += b[i] * b[i];
        sab += a[i] * b[i];
    }
    return sab / std::max(1e-300, std::sqrt(sa * sb));
}

} // namespace

//------------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--verbose") == 0)
            gVerbose = true;

    printf("rations_pedalcheck — the pedalboard, against the sources it is built from\n");
    printf("  rate %.0f Hz, block %d, full scale %.1f V\n\n", kRate, kBlock, ts9::kFullScaleVolts);
    printf("=== the Boost (Yeh §2.4) ===\n");

    // --- 1. the small-signal response, which is the whole linear structure ----------------------
    // Below the diode knee the circuit is exactly clipStageDb + toneAnalogDb + levelDb, so ONE
    // comparison covers eq. 2.13's summation, eq. 2.10's bass cut, the drive-dependent pole and
    // eq. 2.14's tone stage at once. Measured at 1 mV, where the diodes are effectively out of
    // circuit, over a grid of drives and frequencies.
    printf("small-signal response vs eq. 2.10 / 2.13 / 2.14 (1 mV, diodes out of circuit)\n");
    {
        const double amp = 1.0e-3;
        const double freqs[] = {100.0, 220.0, 720.5, 3000.0, 8000.0, 12000.0};
        double worst = 0.0;
        for (double drive : {0.0, 5.0, 10.0}) {
            for (double tone : {0.0, 5.0, 10.0}) {
                Rig rig(drive, tone, 10.0);
                rig.settle();
                const double t = std::min(1.0 - ts9::kPotEnd, std::max(ts9::kPotEnd, tone * 0.1));
                for (double f : freqs) {
                    const Response r = responseAt(rig, f, amp);
                    const double want = clipStageDb(drive, f) + toneAnalogDb(t, f) + levelDb(10.0);
                    worst = std::max(worst, std::fabs(r.gainDb - want));
                    if (gVerbose)
                        printf("    Drive %-4.0f Tone %-4.0f %7.1f Hz  %8.3f dB  (want %8.3f, "
                               "err %+.3f)\n",
                               drive, tone, f, r.gainDb, want, r.gainDb - want);
                }
            }
        }
        // 0.6 dB over the whole grid. What is left after the analytic model is subtracted is the
        // bilinear transform's own frequency warping, which is why the bound is not tighter: it
        // grows with frequency and is largest at 12 kHz. Moving the tone stage inside the
        // oversampled region is what brought it down to this from 1.9 dB.
        // 0.25 dB over the whole grid. What is left after the analytic model is subtracted is the
        // bilinear transform's own frequency warping in the tone stage, which grows with frequency
        // and is largest at 12 kHz; moving that filter inside the oversampled region is what
        // brought it here from 1.9 dB.
        check("worst error over 54 points", worst, 0.0, 0.30);
    }

    // --- 2. the corners, named, so a regression says WHICH one moved -----------------------------
    printf("\ncorner frequencies, from the component values\n");
    {
        printf("    clipped-path bass cut  1/(2*pi*R1*Cz)          %9.1f Hz\n",
               1.0 / (2.0 * kPi * ts9::kR1 * ts9::kCz));
        for (double drive : {0.0, 10.0}) {
            const double r2 = ts9::kR2Fixed + (drive * 0.1) * ts9::kR2Drive;
            printf("    Drive %-4.0f pole        1/(2*pi*R2*Cc)          %9.1f Hz\n", drive,
                   1.0 / (2.0 * kPi * r2 * ts9::kCc));
        }
        // The pole is the reason this stage is an ODE rather than a waveshaper, so it is asserted
        // as a DIFFERENCE between the two Drive extremes at a frequency between them - a single
        // number that can only be right if the pole really moves.
        Rig lo(0.0, 5.0, 10.0), hi(10.0, 5.0, 10.0);
        lo.settle();
        hi.settle();
        const double f = 8000.0;
        const double amp = 1.0e-3;
        const double got = (hi.rate(), responseAt(hi, f, amp).gainDb - clipStageDb(10.0, f)) -
                           (responseAt(lo, f, amp).gainDb - clipStageDb(0.0, f));
        check("pole placement at 8 kHz, Drive 10 vs Drive 0", got, 0.0, 0.5);
    }

    // --- 3. Yeh's own verification point --------------------------------------------------------
    // Section 2.4.4: "a 220 Hz sine signal with amplitude of 100 mV". The figure it is compared
    // against is a PLOT, so its harmonic magnitudes cannot be read out of the text and are NOT
    // asserted here - that is recorded as a gap rather than papered over with numbers taken from
    // this implementation, which would prove nothing. What IS asserted is the structural claim the
    // thesis makes in words: the diode pair is symmetric, so the model makes odd harmonics and
    // essentially no even ones, and Yeh names exactly that as its known departure from the real
    // pedal - "missing low-level, even-order harmonics (-50 dB or less)".
    printf("\nYeh section 2.4.4 verification point: 220 Hz at 100 mV\n");
    {
        Rig rig(5.0, 5.0, 5.0);
        rig.settle();
        // 4096 points at 48 kHz puts bin 19 at 222.66 Hz — Yeh's 220 Hz to within a quarter of a
        // tone, and exactly on the grid, which matters far more here than the 2.7 Hz does.
        const Spectrum s = analyse(rig, 4096, 19, 0.1, 9);
        printf("    fundamental on the grid: %.2f Hz\n", s.f0);
        for (int h = 2; h <= 9; ++h) {
            const double d = s.harmonicDb[static_cast<size_t>(h)];
            if (d <= -299.0)
                printf("    H%-2d  identically zero\n", h);
            else
                printf("    H%-2d %8.2f dB\n", h, d);
        }
        const double evenWorst = std::max(std::max(s.harmonicDb[2], s.harmonicDb[4]),
                                          std::max(s.harmonicDb[6], s.harmonicDb[8]));
        const double oddBest = std::max(s.harmonicDb[3], s.harmonicDb[5]);
        if (oddBest < -60.0) {
            fprintf(stderr, "pedalcheck: no odd-harmonic distortion at Yeh's own test point "
                            "(H3 %.1f dB) - the diodes are not conducting\n", s.harmonicDb[3]);
            ++gFailures;
        }
        // A MINIMUM, not a target, and it is a floor for a BROKEN symmetry rather than a bound on
        // a modelled one. Proved by deliberate fault: scaling one limb of the sinh by 0.98 - a 2 %
        // diode mismatch, which is what real parts have - measures 60.7 dB and passes, and scaling
        // it by 0.5 measures 30.0 dB and fails. That is the intended behaviour: Yeh reports the
        // real pedal's even harmonics at "-50 dB or less" and names their absence as this model's
        // known departure from it, so a small mismatch is more faithful than none and only a gross
        // one is a defect.
        // The measured separation is the whole numerical range - the even
        // harmonics are identically zero, because the diode pair in eq. 2.12 is perfectly
        // symmetric - so a two-sided tolerance would fail for being too good, which the first
        // version of this line did. 40 dB is the floor: Yeh reports the REAL pedal's even
        // harmonics at "-50 dB or less" and names their absence here as this model's known
        // departure from it, so anything above the floor means a symmetry has broken.
        const double sep = oddBest - evenWorst;
        if (evenWorst <= -299.0)
            printf("    every even harmonic is identically zero, so the separation is the whole "
                   "numerical range\n");
        else
            printf("    odd-to-even separation %.1f dB\n", sep);
        if (sep < 40.0) {
            fprintf(stderr, "pedalcheck: even harmonics are only %.1f dB below the odd ones; the "
                            "diode pair has lost its symmetry\n", sep);
            ++gFailures;
        }
    }

    // --- 4. the alias floor ---------------------------------------------------------------------
    // The point of the 4x oversampler. Measured at DRIVE 0, and that is the whole difficulty of
    // this measurement rather than an arbitrary choice: at Drive 10 the ODE's own pole sits at
    // 5.7 kHz and removes the harmonics before they can fold, so the first version of this test -
    // taken at Drive 10 - reported -146 dB and was measuring float noise. At Drive 0 the pole is
    // at 61 kHz, the harmonics survive to fold, and there is actually something to see.
    //
    // A sine at f0 can only produce output at multiples of f0, so everything else in the band is
    // alias or noise; that is exactly how the two are separated. The control raises the base rate
    // 8x so the same DSP has 8x the room - if the control is not far cleaner, this is reporting
    // its own noise floor and says nothing.
    printf("\nalias floor, 3.3 kHz driven hard, swept over Drive and level\n");
    {
        // 283 is prime and coprime with 4096, so the folded harmonics scatter off the harmonic
        // grid instead of hiding on it. f0 lands at 3316 Hz.
        //
        // Swept rather than sampled at one point, because the worst case is not where intuition
        // puts it: more Drive is not worse, since the ODE's own pole falls as Drive rises and
        // removes the very harmonics that would fold. THE GATE IS SET ABOVE THE MEASURED WORST
        // CASE, not at it, so that it reports a regression rather than the run-to-run noise of
        // whichever condition happens to be worst today.
        double worstAlias = -300.0;
        double worstDrive = 0.0, worstAmp = 0.0;
        for (double drive : {0.0, 5.0, 10.0}) {
            for (double amp : {0.25, 0.5}) {
                Rig r(drive, 5.0, 10.0, kRate);
                r.settle();
                const Spectrum sp = analyse(r, 4096, 283, amp, 9);
                printf("    Drive %-4.0f %.2f V : alias %7.2f dB, H3 %7.2f dB\n", drive, amp,
                       sp.aliasDb, sp.harmonicDb[3]);
                if (sp.aliasDb > worstAlias) {
                    worstAlias = sp.aliasDb;
                    worstDrive = drive;
                    worstAmp = amp;
                }
            }
        }
        Rig at48(worstDrive, 5.0, 10.0, kRate);
        at48.settle();
        const Spectrum a = analyse(at48, 4096, 283, worstAmp, 9);
        // The control runs the identical DSP at 8x the base rate, with n scaled by the same factor
        // so that p, and therefore f0, are bit-for-bit the same frequency. If it is not far
        // cleaner then this is reporting its own noise floor and says nothing about aliasing.
        Rig fast(worstDrive, 5.0, 10.0, kRate * 8.0);
        fast.settle();
        const Spectrum b = analyse(fast, 4096 * 8, 283 * 8, worstAmp, 9);
        printf("    worst is Drive %.0f at %.2f V\n", worstDrive, worstAmp);
        printf("    48 kHz  (4x internal): %7.2f dB below the fundamental\n", a.aliasDb);
        printf("    384 kHz (the control): %7.2f dB below the fundamental\n", b.aliasDb);
        if (b.aliasDb >= a.aliasDb - 6.0) {
            fprintf(stderr,
                    "pedalcheck: the 8x control is not materially cleaner than the shipping rate "
                    "(%.2f vs %.2f dB), so this is reporting its own noise floor and says nothing "
                    "about aliasing\n",
                    b.aliasDb, a.aliasDb);
            ++gFailures;
        }
        if (a.aliasDb > kAliasGateDb) {
            fprintf(stderr, "pedalcheck: alias floor %.2f dB is above the %.0f dB gate\n",
                    a.aliasDb, kAliasGateDb);
            ++gFailures;
        } else {
            printf("    alias floor %.2f dB, gate %.0f dB\n", a.aliasDb, kAliasGateDb);
        }
    }

    //==============================================================================================
    printf("\n");
    printf("=== the pedalboard's primitives ===\n");
    //==============================================================================================

    // --- the LFO ------------------------------------------------------------------------------
    // Rate is asserted twice, because there are two different claims in the word. The PHASE after
    // N samples says the increment is right; the number of WRAPS over ten seconds says the thing a
    // player would time with a stopwatch. Both are exact relations, not tolerances on a fit.
    printf("LFO — rate, shape and phase offsets\n");
    {
        // Rates chosen so that f*10 s is NOT a whole number of turns. At an exact wrap the phase
        // is 0 and 1 at the same instant and which one comes back is a rounding accident, so a
        // rate like 0.3 Hz would be testing floating-point luck rather than the oscillator. The
        // comparison is circular for the same reason.
        for (double f : {0.05, 0.33, 1.07, 5.13, 9.7}) {
            Lfo lfo;
            lfo.prepare(kRate);
            lfo.setRate(f);
            const int n = static_cast<int>(kRate * 10.0);
            int wraps = 0;
            double prev = lfo.phase();
            for (int i = 0; i < n; ++i) {
                lfo.advance();
                if (lfo.phase() < prev)
                    ++wraps;
                prev = lfo.phase();
            }
            const double expectPhase = f * 10.0 - std::floor(f * 10.0);
            double err = std::fabs(lfo.phase() - expectPhase);
            err = std::min(err, 1.0 - err); // a phase is a circle
            char label[64];
            snprintf(label, sizeof(label), "%.2f Hz: phase error after 10 s", f);
            check(label, err, 0.0, 1e-6, "turns");
            snprintf(label, sizeof(label), "%.2f Hz: cycles in 10 s", f);
            check(label, wraps, std::floor(f * 10.0), 0.5, "");
        }

        // Shape. A triangle is +-1 with a zero mean and no step bigger than one increment's worth
        // of its own slope, which is what makes it safe to modulate a delay with.
        Lfo tri;
        tri.prepare(kRate);
        tri.setRate(1.0);
        const int n = static_cast<int>(kRate);
        double lo = 1e9, hi = -1e9, sum = 0.0, step = 0.0, prev = tri.triangleAt(0.0);
        double quad = 0.0, anti = 0.0;
        for (int i = 0; i < n; ++i) {
            const double v = tri.triangleAt(0.0);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
            step = std::max(step, std::fabs(v - prev));
            prev = v;
            // Quadrature is a property that can be stated as an identity rather than eyeballed:
            // two sines a quarter turn apart satisfy s^2 + c^2 = 1 at EVERY phase.
            const double s = tri.sineAt(0.0), c = tri.sineAt(0.25);
            quad = std::max(quad, std::fabs(s * s + c * c - 1.0));
            // Antiphase likewise: the triangle half a turn away is the negative of this one.
            anti = std::max(anti, std::fabs(tri.triangleAt(0.5) + v));
            tri.advance();
        }
        check("triangle minimum", lo, -1.0, 1e-4, "");
        check("triangle maximum", hi, 1.0, 1e-4, "");
        check("triangle mean over one cycle", sum / n, 0.0, 1e-6, "");
        check("triangle largest one-sample step", step, 4.0 / kRate, 1e-9, "");
        check("sine quadrature identity s^2+c^2-1", quad, 0.0, 1e-12, "");
        check("triangle antiphase identity", anti, 0.0, 1e-12, "");
    }

    // --- the fractional delay line --------------------------------------------------------------
    printf("fractional delay line — position and gain\n");
    {
        // At an integer delay a Catmull-Rom read must return the stored sample untouched, not a
        // weighted average that happens to be close: the kernel is an interpolator, so its
        // off-centre taps are exactly zero at zero fraction.
        FracDelay line;
        line.prepare(1024);
        double worstInt = 0.0;
        std::vector<double> hist;
        for (int i = 0; i < 900; ++i) {
            const double v = std::sin(0.017 * i) + 0.3 * std::cos(0.11 * i);
            line.write(v);
            hist.push_back(v);
            for (int d : {1, 7, 64, 511}) {
                if (i < d)
                    continue;
                worstInt = std::max(worstInt,
                                    std::fabs(line.read(static_cast<double>(d))
                                              - hist[static_cast<size_t>(i - d)]));
            }
        }
        check("integer delay, error vs the stored sample", worstInt, 0.0, 0.0, "");

        // Fractional position, by the first moment of the echo. See echoCentroid: for a symmetric
        // partition-of-unity kernel this is the delay EXACTLY, so the tolerance is floating point
        // and not a fitting error. DC gain is the same statement read the other way: the kernel's
        // samples sum to one, so a delay line cannot change the level of a constant.
        double worstPos = 0.0, worstGain = 0.0;
        for (double d = 2.0; d <= 40.0; d += 0.03125) {
            FracDelay l2;
            l2.prepare(128);
            std::vector<double> ir;
            for (int i = 0; i < 64; ++i) {
                l2.write(i == 0 ? 1.0 : 0.0);
                ir.push_back(l2.read(d));
            }
            double gain = 0.0;
            for (double v : ir)
                gain += v;
            worstPos = std::max(worstPos, std::fabs(echoCentroid(ir, 0) - d));
            worstGain = std::max(worstGain, std::fabs(gain - 1.0));
        }
        check("fractional delay, position error", worstPos, 0.0, 1e-10, "samples");
        check("fractional delay, DC gain error", worstGain, 0.0, 1e-12, "");
    }

    // --- is cubic interpolation enough? ---------------------------------------------------------
    // The project plan left this open with the note that it is "a measurement in P5, with
    // signalsmith-dsp's windowed-sinc interpolators as the upgrade". Here is the measurement.
    //
    // A delay line read at a MOVING fractional position is measured against a 32-tap
    // Blackman-windowed-sinc read of the same input at the same positions — far too expensive for
    // the audio path, which is the point: it is the answer the two shipping reads are
    // approximating.
    // The stimulus is deliberately harsher than either pedal produces: a 5 kHz tone with the delay
    // swept at 8 Hz through 3 ms, which is ten times the Chorus's fastest sweep.
    printf("interpolation — cubic vs linear, against a 32-tap windowed-sinc reference (dB)\n");
    {
        // A delay line read at a MOVING fractional position, measured against a 32-tap
        // Blackman-windowed-sinc read of the same input at the same positions — far too expensive
        // for the audio path, which is the point: it is the answer the two shipping reads are
        // approximating.
        //
        // MEASURED AT THE OPERATING POINTS THE TWO PEDALS ACTUALLY REACH, not at one invented
        // stimulus. The first version of this used a single made-up sweep and compared it with a
        // gate written down before anything had been measured, which is the habit this project
        // exists to avoid. What matters to the error is the delay's rate of change and the
        // signal's frequency, so each row below is one pedal's knobs at a stated position, with
        // the excursion and modulation rate read off its own constants.
        struct Case {
            const char *what;
            double f0;     // signal frequency
            double fm;     // modulation rate, Hz
            double d0;     // centre delay, samples
            double amp;    // excursion, samples
        };
        const double ms = kRate * 0.001;
        const double flangerMax = flangerdef::kMaxMs * ms;
        const double flangerDef = (flangerdef::kMinMs
                                   + (flangerdef::kMaxMs - flangerdef::kMinMs) * 0.30)
                                  * ms;
        const Case cases[] = {
            {"Chorus, defaults (0.8 Hz, Depth 50)", 1000.0, 0.8, chorusdef::kTap1Ms * ms,
             0.50 * chorusdef::kSweepMs * ms},
            {"Chorus, everything at maximum", 5000.0, 10.0, chorusdef::kTap1Ms * ms,
             chorusdef::kSweepMs * ms},
            {"Flanger, defaults (0.3 Hz, Depth 70)", 1000.0, 0.3, flangerDef,
             0.70 * flangerdef::kMaxExcursion * flangerDef},
            {"Flanger, everything at maximum", 5000.0, 5.0, flangerMax,
             flangerdef::kMaxExcursion * flangerMax},
        };

        double worstCubic = -300.0, worstLinear = -300.0, worstStill = -300.0;
        const char *worstWhat = "";
        for (const Case &c : cases) {
            const int n = 1 << 15;
            const double pad = c.d0 + c.amp + 64.0;
            std::vector<double> x(static_cast<size_t>(n) + static_cast<size_t>(pad), 0.0);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = std::sin(2.0 * kPi * c.f0 * static_cast<double>(i) / kRate);

            FracDelay line;
            line.prepare(static_cast<int>(pad) + 8);
            std::vector<double> cub, lin, ref, still, stillRef;
            // Held at a HALF-SAMPLE fraction for the static run — the worst fraction there is
            // for any symmetric interpolator, and therefore the right thing to compare a sweep
            // that passes through every fraction against. The first version used the delay's own
            // midpoint, which for the Chorus is a whole number of samples, so the "static" column
            // read -317 dB and was measuring nothing at all.
            const double dStill = std::floor(c.d0 + 0.5 * c.amp) + 0.5;
            for (int i = 0; i < n; ++i) {
                line.write(x[static_cast<size_t>(i)]);
                const double d = c.d0 + c.amp * std::sin(2.0 * kPi * c.fm * i / kRate);
                cub.push_back(line.read(d));
                lin.push_back(line.readLinear(d));
                ref.push_back(sincRead(x, static_cast<double>(i) - d));
                still.push_back(line.read(dStill));
                stillRef.push_back(sincRead(x, static_cast<double>(i) - dStill));
            }
            // Discard the first pass, where the line is still filling.
            const size_t skip = static_cast<size_t>(pad) + 64;
            std::vector<double> ec, el, es;
            for (size_t i = skip; i < cub.size(); ++i) {
                ec.push_back(cub[i] - ref[i]);
                el.push_back(lin[i] - ref[i]);
                es.push_back(still[i] - stillRef[i]);
            }
            const double sig = rmsDb(ref, skip);
            const double cubDb = rmsDb(ec) - sig;
            const double linDb = rmsDb(el) - sig;
            const double stillDb = rmsDb(es) - rmsDb(stillRef, skip);
            printf("    %-38s cubic %7.2f   linear %7.2f   (cubic standing still %7.2f)\n", c.what,
                   cubDb, linDb, stillDb);
            if (cubDb > worstCubic) {
                worstCubic = cubDb;
                worstLinear = linDb;
                worstStill = stillDb;
                worstWhat = c.what;
            }
        }
        printf("    worst is \"%s\": cubic %.2f dB, %.1f dB cleaner than linear\n", worstWhat,
               worstCubic, worstLinear - worstCubic);
        printf("    modulation costs %.2f dB over the same read standing still\n",
               worstCubic - worstStill);

        // THE GATE THAT ANSWERS THE QUESTION, and it needs no constant chosen by anybody. The
        // plan asked whether cubic is enough UNDER FAST MODULATION. The static column is the same
        // read at the worst fixed fraction, where the error is a fixed, gentle high-frequency
        // tilt and not an artefact — so if the swept figure is no worse than the standing one,
        // modulation is adding nothing and the answer is yes. That is what the numbers say: at
        // the Flanger's most extreme setting the sweep costs about a decibel over standing still,
        // and at every ordinary setting the error is 90 dB down.
        if (worstCubic - worstStill > 3.0) {
            fprintf(stderr, "pedalcheck: sweeping the read head costs %.2f dB over the same read "
                            "standing still, so the interpolator IS being strained by the "
                            "modulation — signalsmith-dsp's windowed-sinc interpolators are the "
                            "documented upgrade\n",
                    worstCubic - worstStill);
            ++gFailures;
        }
        // A backstop above the measured worst, on the alias gate's rule: it reports a regression,
        // not the noise floor of whichever case happens to be worst today.
        if (worstCubic > kInterpGateDb) {
            fprintf(stderr, "pedalcheck: cubic interpolation error %.2f dB is above the %.0f dB "
                            "gate\n",
                    worstCubic, kInterpGateDb);
            ++gFailures;
        }
        if (worstLinear - worstCubic < 6.0) {
            fprintf(stderr, "pedalcheck: cubic is only %.1f dB better than linear, so this is "
                            "measuring the reference's own floor rather than the interpolators\n",
                    worstLinear - worstCubic);
            ++gFailures;
        }
    }

    //==============================================================================================
    printf("\n");
    printf("=== the Chorus (PASP \"Chorus Effect\") ===\n");
    //==============================================================================================

    // With Depth at zero the two taps stand still, so the pedal is exactly a two-tap comb summed
    // with the dry path and its response can be written down. Both rest positions are a whole
    // number of samples at 48 kHz (480 and 864), so the interpolator is exact there and the
    // comparison is against the algebra rather than against a tolerance.
    printf("frozen taps: |H(f)| vs (1-mix) + mix*0.5*(exp(-jwD0) + exp(-jwD1))\n");
    {
        double worst = 0.0;
        for (double mixPct : {25.0, 50.0, 100.0}) {
            ChorusBoard b;
            b.set(Chorus::kRate, 1.0);
            b.set(Chorus::kDepth, 0.0);
            b.set(Chorus::kMix, mixPct);
            b.restart();
            b.silence(static_cast<size_t>(kRate * 0.2)); // land the engage ramp and the smoothers

            const int n = 1 << 14;
            std::vector<double> ir(static_cast<size_t>(n), 0.0);
            ir[0] = 1.0;
            b.run(ir);
            const std::vector<double> mag = magnitudeOf(ir, n);

            const double d0 = chorusdef::kTap0Ms * kRate * 0.001;
            const double d1 = chorusdef::kTap1Ms * kRate * 0.001;
            const double mix = mixPct * 0.01;
            for (double f : {80.0, 220.0, 500.0, 1000.0, 3000.0, 7000.0}) {
                // The analytic value is evaluated at the BIN's own frequency, not at the one that
                // was asked for. 80 Hz falls on bin 27.31 of a 16384-point window at 48 kHz, i.e.
                // 79.1 Hz, and inside a comb whose teeth are 125 Hz apart that 0.9 Hz is worth
                // 0.43 dB — which is exactly what this measured before the frequency was taken
                // from the grid instead of from the wish.
                const size_t bin = static_cast<size_t>(std::llround(f * n / kRate));
                const double fBin = static_cast<double>(bin) * kRate / n;
                const double w = 2.0 * kPi * fBin / kRate;
                const std::complex<double> h =
                    (1.0 - mix)
                    + mix * 0.5
                          * (std::exp(std::complex<double>(0.0, -w * d0))
                             + std::exp(std::complex<double>(0.0, -w * d1)));
                const double gotDb = 20.0 * std::log10(std::max(1e-300, mag[bin]));
                const double wantDb = 20.0 * std::log10(std::max(1e-300, std::abs(h)));
                worst = std::max(worst, std::fabs(gotDb - wantDb));
                if (gVerbose)
                    printf("    mix %-5.0f %7.1f Hz  %8.3f dB (want %8.3f)\n", mixPct, fBin, gotDb,
                           wantDb);
            }
        }
        check("frozen-tap response, worst error", worst, 0.0, 0.05, "dB");
    }

    // WHERE THE TAPS ACTUALLY ARE, measured off the impulse response, against the two positions
    // the header's own prose declares.
    //
    // The numbers are literals here ON PURPOSE, and that is the point of this check rather than an
    // oversight. The comb comparison above reads chorusdef's constants for its expectation, so it
    // verifies the STRUCTURE — two taps, summed at half, mixed linearly against dry — and moves
    // with the constants by design. It therefore cannot notice a tap being moved, which was proved
    // by moving one 1 ms and watching every measurement still pass. A tap position is not
    // derivable from physics the way a corner frequency is; it is a design decision, so what the
    // gate can hold it to is the decision as written down. Changing kTap0Ms or kTap1Ms should fail
    // here and make somebody update the paragraph that explains why they are what they are.
    printf("tap positions, measured: the design says 10 ms and 18 ms\n");
    {
        ChorusBoard b;
        b.set(Chorus::kRate, 1.0);
        b.set(Chorus::kDepth, 0.0);
        b.set(Chorus::kMix, 100.0); // fully wet: the dry path is out, so only the taps are left
        b.restart();

        std::vector<double> ir(4096, 0.0);
        ir[0] = 1.0;
        b.run(ir);

        std::vector<size_t> hits;
        for (size_t i = 0; i < ir.size(); ++i)
            if (std::fabs(ir[i]) > 0.25) // each tap arrives at exactly 0.5
                hits.push_back(i);
        if (hits.size() != 2) {
            fprintf(stderr, "pedalcheck: expected two taps in the frozen impulse response, found "
                            "%zu\n",
                    hits.size());
            ++gFailures;
        } else {
            check("first tap", static_cast<double>(hits[0]), 10.0 * kRate * 0.001, 0.5, "samples");
            check("second tap", static_cast<double>(hits[1]), 18.0 * kRate * 0.001, 0.5, "samples");
            check("tap gain", ir[hits[0]], 0.5, 1e-12, "");
            printf("    taps at %zu and %zu samples (%.1f and %.1f ms), each at %.3f\n", hits[0],
                   hits[1], hits[0] / (kRate * 0.001), hits[1] / (kRate * 0.001), ir[hits[0]]);
        }
    }

    // Mix at zero must be the input, bit for bit — not almost. A wet path that leaks at the dry end
    // of the knob is a pedal that colours the sound when it is turned off.
    printf("mix at 0 %%: the wet path is gone, not merely small\n");
    {
        ChorusBoard b;
        b.set(Chorus::kRate, 3.0);
        b.set(Chorus::kDepth, 100.0);
        b.set(Chorus::kMix, 0.0);
        b.restart();
        b.silence(static_cast<size_t>(kRate * 0.2));
        std::vector<double> sig(4096), copy(4096);
        for (size_t i = 0; i < sig.size(); ++i)
            sig[i] = copy[i] = std::sin(2.0 * kPi * 440.0 * i / kRate);
        b.run(sig);
        double worst = 0.0;
        for (size_t i = 0; i < sig.size(); ++i)
            worst = std::max(worst, std::fabs(sig[i] - copy[i]));
        check("mix 0 %, difference from the input", worst, 0.0, 0.0, "");
    }

    //==============================================================================================
    printf("\n");
    printf("=== the Flanger (PASP \"Flanging\", eq. 5.1) ===\n");
    //==============================================================================================

    // THE MILESTONE MEASUREMENT. Smith: "The notches are thus spaced at intervals of fs/M Hz ...
    // the notch spacing is inversely proportional to delay-line length", with the notches falling
    // between the peaks at w_k + pi/M — so the first one is at fs/(2M) and they repeat every fs/M.
    //
    // The LFO is frozen (Depth 0) and Regen is zero, which leaves exactly eq. 5.1's feedforward
    // comb. Manual is chosen to land M on a whole number of samples, so the interpolator is exact
    // and the expected notch frequencies are arithmetic rather than an approximation.
    printf("notch spacing vs fs/M, and the first notch at fs/2M\n");
    {
        for (double targetMs : {2.0, 4.0, 6.0}) {
            const double manualPct =
                (targetMs - flangerdef::kMinMs) / (flangerdef::kMaxMs - flangerdef::kMinMs) * 100.0;
            const double m = targetMs * kRate * 0.001; // 96, 192, 288 samples — all whole

            FlangerBoard b;
            b.set(Flanger::kRate, 0.5);
            b.set(Flanger::kDepth, 0.0);
            b.set(Flanger::kManual, manualPct);
            b.set(Flanger::kRegen, 0.0);
            b.restart();
            b.silence(static_cast<size_t>(kRate * 0.2));

            const int n = 1 << 15;
            std::vector<double> irL(static_cast<size_t>(n), 0.0), irR(static_cast<size_t>(n), 0.0);
            irL[0] = irR[0] = 1.0;
            b.run(irL.data(), irR.data(), irL.size());

            // Where the echo actually is, independently of the spectrum.
            check("echo position", echoCentroid(irL, 2), m, 1e-6, "samples");

            const std::vector<double> mag = magnitudeOf(irL, n);
            // Below fs/4 only: above it the cubic read's own phase error starts to move the
            // notches, which is a real effect and is measured separately above rather than folded
            // into this one as slop.
            const std::vector<double> notch = notchesOf(mag, kRate, n, kRate * 0.25);
            if (notch.size() < 4) {
                fprintf(stderr, "pedalcheck: only %zu notches found below fs/4 at M = %.0f\n",
                        notch.size(), m);
                ++gFailures;
                continue;
            }
            double spacing = 0.0;
            for (size_t i = 1; i < notch.size(); ++i)
                spacing += notch[i] - notch[i - 1];
            spacing /= static_cast<double>(notch.size() - 1);

            char label[80];
            snprintf(label, sizeof(label), "M = %.0f: first notch", m);
            check(label, notch[0], kRate / (2.0 * m), 1.0, "Hz");
            snprintf(label, sizeof(label), "M = %.0f: mean notch spacing", m);
            check(label, spacing, kRate / m, 1.0, "Hz");
            printf("    M = %3.0f samples (%.1f ms): first notch %7.1f Hz, spacing %7.1f Hz "
                   "(fs/2M = %.1f, fs/M = %.1f), %zu notches\n",
                   m, targetMs, notch[0], spacing, kRate / (2.0 * m), kRate / m, notch.size());
        }
    }

    // The LFO, measured THROUGH THE AUDIO rather than on the oscillator: the delay is read off the
    // impulse response at sixteen points around one cycle and compared with Smith's
    // M(n) = M0*[1 + A*tri(2*pi*f*n*T)]. This is the one measurement that ties Rate, Depth and
    // Manual together — the phase axis is Rate (a phase is a time multiplied by it), the swing is
    // Depth, and the centre is Manual.
    // What Manual's two ends mean, measured, against the span the header declares: 0.5 ms to
    // 8 ms. Literals here for the same reason as the Chorus's taps just above — the trajectory
    // check below reads flangerdef's constants and so cannot notice the span being changed.
    printf("Manual's endpoints, measured: the design says 0.5 ms to 8 ms\n");
    {
        for (auto pair : {std::make_pair(0.0, 0.5), std::make_pair(100.0, 8.0)}) {
            FlangerBoard b;
            b.set(Flanger::kRate, 0.5);
            b.set(Flanger::kDepth, 0.0);
            b.set(Flanger::kManual, pair.first);
            b.set(Flanger::kRegen, 0.0);
            b.restart();
            std::vector<double> l(4096, 0.0), r(4096, 0.0);
            l[0] = r[0] = 1.0;
            b.run(l.data(), r.data(), l.size());
            char label[80];
            snprintf(label, sizeof(label), "Manual %.0f %%", pair.first);
            check(label, echoCentroid(l, 2) / (kRate * 0.001), pair.second, 1e-6, "ms");
            printf("    Manual %3.0f %% -> %.4f ms\n", pair.first,
                   echoCentroid(l, 2) / (kRate * 0.001));
        }
    }

    printf("delay trajectory vs M0*[1 + A*tri(phase)] — Rate, Depth and Manual at once\n");
    {
        const double depthPct = 100.0, manualPct = 50.0;
        const double m0 = (flangerdef::kMinMs
                           + (flangerdef::kMaxMs - flangerdef::kMinMs) * manualPct * 0.01)
                          * kRate * 0.001;
        const double a = depthPct * 0.01 * flangerdef::kMaxExcursion;
        printf("    M0 = %.1f samples, excursion +-%.1f samples (Depth 100 %% = A %.2f)\n", m0,
               a * m0, a);

        // Two rates, because one rate cannot tell a correct Rate from a Rate that is out by a
        // constant factor: the phase axis below is a TIME divided by the rate, so a wrong rate and
        // a correspondingly wrong wait would agree with each other at every point.
        for (double rateHz : {0.05, 0.5}) {
            double worst = 0.0;
            const int K = 16;
            for (int k = 0; k < K; ++k) {
                const double phase = static_cast<double>(k) / K;
                FlangerBoard b;
                b.set(Flanger::kRate, rateHz);
                b.set(Flanger::kDepth, depthPct);
                b.set(Flanger::kManual, manualPct);
                b.set(Flanger::kRegen, 0.0);
                // restart() leaves the pedal fully settled — the engage mix at exactly 1.0
                // because the footswitch was already down, the smoothers snapped to the knobs,
                // the line clear and the LFO at phase zero. So the silence that follows has ONE
                // job: to wind the LFO to `phase`. That the two are the same thing is precisely
                // what the Rate control claims, which is why this is a measurement of Rate and
                // not a setup step.
                b.restart();
                b.silence(static_cast<size_t>(std::llround(phase / rateHz * kRate)));

                std::vector<double> irL(2048, 0.0), irR(2048, 0.0);
                irL[0] = irR[0] = 1.0;
                b.run(irL.data(), irR.data(), irL.size());
                const double got = echoCentroid(irL, 2);

                // THE DELAY THAT ACTED IS THE ONE IN FORCE WHEN THE ECHO CAME OUT, not the one at
                // the moment the impulse went in — the echo emerges M samples later, by which
                // time the LFO has moved on. At 0.5 Hz with this excursion that is 1.6 samples,
                // thirty times the tolerance, and reading it as an error would have been reading
                // the measurement's own geometry as a fault in the pedal. The echo appears at the
                // output sample n where n = M(n), so the expectation is that fixed point; the map
                // is a contraction here (its slope is 4*A*M0*rate/fs, about 0.008) so three
                // passes are far more than enough.
                double want = m0;
                for (int it = 0; it < 4; ++it)
                    want = m0 * (1.0 + a * Lfo::triangle(phase + want / kRate * rateHz));

                worst = std::max(worst, std::fabs(got - want));
                if (gVerbose)
                    printf("    %.2f Hz phase %.3f  delay %8.3f samples (want %8.3f)\n", rateHz,
                           phase, got, want);
            }
            char label[80];
            snprintf(label, sizeof(label), "%.2f Hz: delay trajectory over one cycle", rateHz);
            check(label, worst, 0.0, 0.05, "samples");
            printf("    %.2f Hz: worst error over 16 phases of one cycle %.4f samples\n", rateHz,
                   worst);
        }
    }

    // Regen. Positive feedback sharpens the peaks; negative feedback puts them where the notches
    // were, which is the trade PASP's "Flanger Inverted Mode" describes for g = -1 and which this
    // pedal reaches from the Regen knob instead of a separate switch.
    printf("regen: positive sharpens the peaks, negative moves them half a spacing\n");
    {
        auto responseOf = [](double regenPct, std::vector<double> &magOut) {
            FlangerBoard b;
            b.set(Flanger::kRate, 0.5);
            b.set(Flanger::kDepth, 0.0);
            b.set(Flanger::kManual, 46.6666666666666667); // M = 192 samples
            b.set(Flanger::kRegen, regenPct);
            b.restart();
            b.silence(static_cast<size_t>(kRate * 0.2));
            const int n = 1 << 16;
            std::vector<double> irL(static_cast<size_t>(n), 0.0), irR(static_cast<size_t>(n), 0.0);
            irL[0] = irR[0] = 1.0;
            b.run(irL.data(), irR.data(), irL.size());
            magOut = magnitudeOf(irL, n);
        };
        std::vector<double> m0, mp, mn;
        responseOf(0.0, m0);
        responseOf(90.0, mp);
        responseOf(-90.0, mn);
        auto peakDb = [](const std::vector<double> &m) {
            double p = 0.0;
            for (double v : m)
                p = std::max(p, v);
            return 20.0 * std::log10(std::max(1e-300, p));
        };
        printf("    peak gain: regen 0 %% %6.2f dB, +90 %% %6.2f dB, -90 %% %6.2f dB\n", peakDb(m0),
               peakDb(mp), peakDb(mn));
        if (peakDb(mp) <= peakDb(m0) + 6.0) {
            fprintf(stderr, "pedalcheck: +90 %% regen only raises the peak from %.2f to %.2f dB; "
                            "the feedback comb is not in circuit\n",
                    peakDb(m0), peakDb(mp));
            ++gFailures;
        }
        // The notches move by half a spacing when the sign flips. fs/M is 250 Hz here, so the
        // first notch should shift by 125 Hz.
        const double spacing = kRate / 192.0;
        const std::vector<double> np = notchesOf(mp, kRate, 1 << 16, 3000.0);
        const std::vector<double> nn = notchesOf(mn, kRate, 1 << 16, 3000.0);
        if (np.empty() || nn.empty()) {
            fprintf(stderr, "pedalcheck: no notches found either side of the regen sign flip\n");
            ++gFailures;
        } else {
            const double shift = std::fabs(nn[0] - np[0]);
            check("first-notch shift on a regen sign flip", shift, spacing * 0.5, 6.0, "Hz");
        }
    }

    // The tail. At maximum regen the loop gain is 0.95 times the interpolator's magnitude response,
    // which for Catmull-Rom never exceeds one — so this must decay, and it must decay to true zero
    // rather than into the subnormal range, where every operation costs about a hundred times what
    // it should. FTZ/DAZ is deliberately NOT armed in this tool: the processor arms it on the audio
    // thread, and a flush that only works because of that would be a flush this cannot see.
    printf("tail at maximum regen: decays, and decays to zero rather than to subnormals\n");
    {
        FlangerBoard b;
        b.set(Flanger::kRate, 0.5);
        b.set(Flanger::kDepth, 70.0);
        b.set(Flanger::kManual, 30.0);
        b.set(Flanger::kRegen, 95.0);
        b.restart();
        b.silence(static_cast<size_t>(kRate * 0.2));

        // Sixty seconds, and the length is the whole point. The loop decays about 0.95 per
        // round trip, so after ten seconds it is at 1e-107 — small, but a perfectly ordinary
        // double, and a run that short would pass whether the flush existed or not. It first
        // reaches the subnormal range at about 38 s and spends some two seconds crossing it,
        // which is the window this is watching.
        const size_t n = static_cast<size_t>(kRate * 60.0);
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1.0;
        b.run(l.data(), r.data(), n);

        const size_t lastSecond = n - static_cast<size_t>(kRate);
        std::vector<double> tail(l.begin() + static_cast<long>(lastSecond), l.end());
        const double tailDb = rmsDb(tail);
        printf("    tail after 60 s: %.1f dB; subnormals present: %s\n", tailDb,
               hasSubnormal(l) ? "YES" : "no");
        if (tailDb > -120.0) {
            fprintf(stderr, "pedalcheck: the regen tail is still at %.1f dB after sixty seconds\n",
                    tailDb);
            ++gFailures;
        }
        if (hasSubnormal(l)) {
            fprintf(stderr, "pedalcheck: the feedback path produced subnormal samples — the "
                            "denormal flush is not working\n");
            ++gFailures;
        }
    }

    // Stereo. The two channels differ only because their LFOs are half a turn apart, so with the
    // LFO stopped they must be identical to the last bit — which is the check that the difference
    // is the antiphase and not an accident of two delay lines being separately initialised.
    printf("stereo: antiphase LFOs decorrelate the channels, and nothing else does\n");
    {
        auto pair = [](double depthPct, double &diff) {
            FlangerBoard b;
            b.set(Flanger::kRate, 0.7);
            b.set(Flanger::kDepth, depthPct);
            b.set(Flanger::kManual, 50.0);
            b.set(Flanger::kRegen, 40.0);
            b.restart();
            b.silence(static_cast<size_t>(kRate * 0.2));
            const size_t n = static_cast<size_t>(kRate * 2.0);
            std::vector<double> l(n), r(n);
            for (size_t i = 0; i < n; ++i)
                l[i] = r[i] = std::sin(2.0 * kPi * 330.0 * i / kRate)
                              + 0.5 * std::sin(2.0 * kPi * 1750.0 * i / kRate);
            b.run(l.data(), r.data(), n);
            double d = 0.0;
            for (size_t i = 0; i < n; ++i)
                d = std::max(d, std::fabs(l[i] - r[i]));
            diff = d;
        };
        double still = 0.0, moving = 0.0;
        pair(0.0, still);
        pair(100.0, moving);
        check("depth 0 %: L minus R", still, 0.0, 0.0, "");
        printf("    depth 100 %%: peak |L - R| = %.4f\n", moving);
        if (moving < 0.05) {
            fprintf(stderr, "pedalcheck: the two channels differ by only %.4f with the LFO "
                            "running — the antiphase is not reaching the delay\n",
                    moving);
            ++gFailures;
        }
    }

    // Zipper. A control that is applied once per block steps at block boundaries and nowhere else,
    // so the test needs no threshold plucked out of the air: measure the largest sample-to-sample
    // step AT a block boundary against the largest one inside a block. A per-sample smoother makes
    // the two the same population; a per-block one makes the first much larger.
    printf("no zipper under a dragged control: block-boundary steps vs steps inside a block\n");
    {
        for (int which : {Flanger::kManual, Flanger::kRate, Flanger::kRegen}) {
            FlangerBoard b;
            b.set(Flanger::kRate, 1.0);
            b.set(Flanger::kDepth, 60.0);
            b.set(Flanger::kManual, 0.0);
            b.set(Flanger::kRegen, 0.0);
            b.restart();
            b.silence(static_cast<size_t>(kRate * 0.2));

            // Drag the knob from one end of its range to the other in a tenth of a second, which
            // is far faster than a hand and is the point.
            const double lo = (which == Flanger::kRegen) ? -95.0 : 0.0;
            const double hi = (which == Flanger::kRate) ? 5.0 : 95.0;
            const size_t n = static_cast<size_t>(kRate * 0.1);
            std::vector<double> l(n), r(n);
            for (size_t i = 0; i < n; ++i)
                l[i] = r[i] = std::sin(2.0 * kPi * 5000.0 * i / kRate);

            double boundary = 0.0, inside = 0.0;
            for (size_t i = 0; i < n; i += kBlock) {
                const size_t k = std::min<size_t>(kBlock, n - i);
                b.set(which, lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n));
                const double before = (i > 0) ? l[i - 1] : 0.0;
                b.run(l.data() + i, r.data() + i, k);
                if (i > 0)
                    boundary = std::max(boundary, std::fabs(l[i] - before));
                for (size_t j = 1; j < k; ++j)
                    inside = std::max(inside, std::fabs(l[i + j] - l[i + j - 1]));
            }
            const char *name = which == Flanger::kManual ? "Manual"
                               : which == Flanger::kRate ? "Rate"
                                                         : "Regen";
            printf("    dragging %-7s: worst step at a boundary %.5f, inside a block %.5f\n", name,
                   boundary, inside);
            if (boundary > inside * 1.5) {
                fprintf(stderr, "pedalcheck: dragging %s steps %.2fx harder at block boundaries "
                                "than inside a block — the control is not smoothed per sample\n",
                        name, boundary / std::max(1e-12, inside));
                ++gFailures;
            }
        }
    }

    //==============================================================================================
    // THE DELAY. Structure and stability condition from PASP's "Feedback Comb Filters"; the loop
    // filter's corners are a voicing decision and are pinned against literals written above.
    //==============================================================================================
    printf("\n-- Delay ------------------------------------------------------------------------\n");

    // A helper for every measurement below: a board at one setting, restarted so the smoothers are
    // already where the knobs are rather than on their way there.
    auto delayAt = [](DelayBoard &b, double timeMs, double fbPct, double tone, double mixPct,
                      int sync, bool ping) {
        b.set(Delay::kTime, timeMs);
        b.set(Delay::kFeedback, fbPct);
        b.set(Delay::kTone, tone);
        b.set(Delay::kMix, mixPct);
        b.set(Delay::kSync, double(sync));
        b.set(Delay::kPingPong, ping ? 1.0 : 0.0);
        b.restart();
    };

    // Delay time, free-running. The whole span of the knob, and the tolerance is a thousandth of a
    // sample rather than the milestone's one sample, because the loop is either exactly the length
    // the knob says or it is not.
    printf("delay time: the loop is as long as the knob says, over the whole span\n");
    {
        const size_t n = size_t(1) << kPhaseFftLog2;
        for (double ms : {20.0, 100.0, 400.0, 1000.0, 2000.0}) {
            DelayBoard b;
            delayAt(b, ms, 0.0, 5.0, 100.0, 0, false);
            const double got = measuredDelaySamples(b, 5.0, n);
            char label[64];
            snprintf(label, sizeof(label), "Time %.0f ms", ms);
            check(label, got / (kRate * 0.001), ms, 1e-3, "ms");
        }
    }

    // The same measurement with NO model of the filter in it at all. Two runs at different times
    // share every coefficient, so the loop filter's contribution to the phase is identical and
    // cancels exactly in the difference — which is what makes this the check on the check: if the
    // closed form above were wrong, the absolute figures would all be wrong together and only this
    // would notice.
    printf("delay time, model-free: the DIFFERENCE between two settings cancels the filter\n");
    {
        const size_t n = size_t(1) << kPhaseFftLog2;
        auto phaseAt = [&](double ms) {
            DelayBoard b;
            delayAt(b, ms, 0.0, 5.0, 100.0, 0, false);
            return wetPhaseAtBin(delayWetIr(b, n));
        };
        const double p1 = phaseAt(150.0), p2 = phaseAt(950.0);
        const double diff = (p1 - p2) / kPhaseBinW / (kRate * 0.001);
        check("950 ms minus 150 ms", diff, 800.0, 1e-3, "ms");
    }

    // Tempo sync. kDelaySyncBeats is in quarter notes, so a division of b beats at t BPM is
    // b*60/t seconds; every expectation here is that arithmetic and nothing else.
    printf("tempo sync: a division is beats x 60 / BPM, and it re-locks when the tempo moves\n");
    {
        const size_t n = size_t(1) << kPhaseFftLog2;
        const double bpm = 120.0;
        for (int sync : {1, 2, 3, 4, 5, 7, 10}) {
            DelayBoard b;
            b.pedal().setTempo(bpm);
            // Time is left at a value the division must OVERRIDE, so a sync that quietly did
            // nothing would read as 400 ms rather than as the division.
            delayAt(b, 400.0, 0.0, 5.0, 100.0, sync, false);
            const double want = kDelaySyncBeats[sync] * 60000.0 / bpm;
            const double got = measuredDelaySamples(b, 5.0, n) / (kRate * 0.001);
            char label[64];
            snprintf(label, sizeof(label), "%-5s at 120 BPM", kDelaySyncNames[sync]);
            check(label, got, want, 1e-3, "ms");
        }

        // A tempo CHANGE, with no restart: the time smoother has to walk from one division to the
        // other and land exactly on it. Three seconds of silence is comfortably longer than the
        // 1.8 s that smoother needs to close 8000 samples to within its snap threshold.
        DelayBoard b;
        b.pedal().setTempo(120.0);
        delayAt(b, 400.0, 0.0, 5.0, 100.0, 4, false); // 1/4
        b.silence(size_t(kRate * 0.5));
        b.pedal().setTempo(90.0);
        b.silence(size_t(kRate * 3.0));
        const double got = measuredDelaySamples(b, 5.0, n) / (kRate * 0.001);
        check("1/4 after 120 -> 90 BPM", got, 60000.0 / 90.0, 1e-3, "ms");

        // And the two ways of asking for free-running. A host that supplies no tempo must not be
        // able to silence the division by leaving the delay at zero length.
        DelayBoard f;
        f.pedal().setTempo(0.0);
        delayAt(f, 333.0, 0.0, 5.0, 100.0, 4, false);
        check("1/4 with no host tempo", measuredDelaySamples(f, 5.0, n) / (kRate * 0.001), 333.0,
              1e-3, "ms");

        DelayBoard g;
        g.pedal().setTempo(120.0);
        delayAt(g, 333.0, 0.0, 5.0, 100.0, 0, false); // Free
        check("Free with a host tempo", measuredDelaySamples(g, 5.0, n) / (kRate * 0.001), 333.0,
              1e-3, "ms");
    }

    // The loop filter. Magnitude only, so the delay's own linear phase drops out, and against the
    // closed form written from the literal corners above.
    printf("loop filter: one-pole low-pass swept by Tone, over a fixed high-pass\n");
    {
        const int n = 1 << 16;
        for (double tone : {0.0, 5.0, 10.0}) {
            DelayBoard b;
            delayAt(b, 100.0, 0.0, tone, 100.0, 0, false);
            const std::vector<double> mag = magnitudeOf(delayWetIr(b, size_t(n)), n);

            double worst = 0.0, worstHz = 0.0;
            for (double f : {30.0, 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0,
                             16000.0}) {
                const int k = int(f * n / kRate + 0.5);
                const double fBin = double(k) * kRate / n;
                const double w = 2.0 * kPi * fBin / kRate;
                const double got = 20.0 * std::log10(std::max(1e-300, mag[size_t(k)]));
                const double want = 20.0 * std::log10(std::abs(loopFilterH(tone, w)));
                if (std::fabs(got - want) > worst) {
                    worst = std::fabs(got - want);
                    worstHz = fBin;
                }
            }
            // The corner the filter actually has, which at the bright end is NOT the nominal
            // 12 kHz: 1 - exp(-2pi f/fs) is a quarter of the sample rate away from its own
            // half-power point up there. Printed rather than asserted, because what is asserted is
            // the whole response.
            double cornerHz = 0.0;
            const double ref = std::abs(loopFilterH(tone, 2.0 * kPi * 500.0 / kRate));
            for (int k = 1; k < n / 2; ++k) {
                const double f = double(k) * kRate / n;
                if (f > 500.0 && mag[size_t(k)] < ref * 0.70794578) {
                    cornerHz = f;
                    break;
                }
            }
            printf("    Tone %-4.1f: nominal %7.1f Hz, measured -3 dB at %7.1f Hz; worst error "
                   "%.4f dB at %.0f Hz\n",
                   tone, toneCornerHz(tone), cornerHz, worst, worstHz);
            if (worst > 0.02) {
                fprintf(stderr,
                        "pedalcheck: the loop filter at Tone %.1f is %.3f dB from its declared "
                        "corners at %.0f Hz\n",
                        tone, worst, worstHz);
                ++gFailures;
            }
        }
    }

    // Decay. PASP states the condition this rests on: |g| < 1, or "each echo will be louder than
    // the previous echo, producing a never-ending, growing series". The knob stops at 95, and the
    // loop filter can only lose, so every repeat must be quieter than the one before it.
    printf("decay: |g| < 1, so the repeats must shrink and the tail must reach true zero\n");
    {
        DelayBoard b;
        delayAt(b, 100.0, 95.0, 5.0, 100.0, 0, false);
        const size_t d = size_t(kRate * 0.1);
        const size_t n = size_t(kRate * 60.0);
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1.0;
        b.run(l.data(), r.data(), n);

        // Energy in the window around each of the first eight repeats.
        double prev = 0.0;
        double worstRatioDb = -1e9;
        for (int k = 1; k <= 8; ++k) {
            double e = 0.0;
            for (size_t i = k * d - d / 4; i < k * d + d / 4 && i < n; ++i)
                e += l[i] * l[i];
            const double db = 10.0 * std::log10(std::max(1e-300, e));
            if (k > 1)
                worstRatioDb = std::max(worstRatioDb, db - prev);
            prev = db;
        }
        printf("    worst repeat-to-repeat change over eight repeats: %+.3f dB\n", worstRatioDb);
        if (worstRatioDb > 0.0) {
            fprintf(stderr, "pedalcheck: a repeat was %+.3f dB LOUDER than the one before it — the "
                            "loop is not decaying\n",
                    worstRatioDb);
            ++gFailures;
        }

        const std::vector<double> tail(l.begin() + long(n - size_t(kRate)), l.end());
        const double tailDb = rmsDb(tail);
        printf("    tail after 60 s: %.1f dB\n", tailDb);
        if (tailDb > -120.0) {
            fprintf(stderr, "pedalcheck: the delay tail is still at %.1f dB after sixty seconds\n",
                    tailDb);
            ++gFailures;
        }
    }

    // And it must reach true zero rather than settling into the subnormal range, where every
    // operation costs about a hundred times what it should — the lesson the Flanger's tail taught,
    // which is that flushing what goes INTO the line is not enough on its own: the line still holds
    // ordinary values just above the floor for one delay's worth of samples after the loop bottoms
    // out, and a fraction of one of those is subnormal. FTZ/DAZ is deliberately NOT armed in this
    // tool, because a flush that only worked because the audio thread had armed it would be a flush
    // this cannot see, and the same DSP is reachable from tools that arm nothing.
    //
    // THE IMPULSE IS 1e-300 AND NOT 1, which is the only reason this is affordable. The Flanger's
    // loop is 4 ms round and crosses the subnormal range unaided in 38 s; this one is 20 ms round
    // at its shortest and loses about 0.9 dB a lap, so reaching 1e-308 from unity takes some 6800
    // laps — eleven minutes of audio to test one thing. The loop is linear, so an input 300 decades
    // down is the identical signal 250 seconds later, and starting there measures exactly the same
    // arithmetic in thirty seconds. Mix is at 50 % rather than 100 % because at 100 % the output IS
    // the line read and nothing scales it, and it is the scaling that turns the line's last
    // ordinary values into subnormals.
    printf("the tail reaches true zero rather than the subnormal range\n");
    {
        DelayBoard b;
        delayAt(b, 20.0, 95.0, 10.0, 50.0, 0, false);
        const size_t n = size_t(kRate * 30.0);
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1e-300;
        b.run(l.data(), r.data(), n);
        // What this can see is the OUTPUT flush. The loop filter's own flush is invisible here by
        // construction — whatever those states hold reaches the buffer only through this same
        // output flush — so its justification is cost, and the cost is measured rather than
        // asserted: removing it takes this whole tool from 1.94 s to 4.39 s. See LoopTone.
        printf("    seeded at 1e-300; subnormals in the output: %s\n",
               hasSubnormal(l) || hasSubnormal(r) ? "YES" : "no");
        if (hasSubnormal(l) || hasSubnormal(r)) {
            fprintf(stderr, "pedalcheck: the delay's feedback path produced subnormal samples\n");
            ++gFailures;
        }
    }

    // And the feedback is the number the knob says, not merely something below one.
    //
    // THIS CHECK EXISTS BECAUSE THE ONE ABOVE HAS A HOLE, found by deliberately putting the
    // feedback 6 % over what the knob asked for: the loop filter loses more than 6 % per pass, so
    // the repeats still shrink and "it decays" is still true. Decaying is the stability condition
    // and it is worth asserting on its own, but it says nothing about the coefficient.
    //
    // Echo k+1 IS echo k convolved with fb times the loop filter — exactly, since the loop is
    // linear — so the ratio of their spectra at any frequency is fb*|H(f)| and nothing else. That
    // makes the expectation the knob's own value times the closed form written from the literal
    // corners, with no envelope, no windowing assumption and no fitting.
    printf("feedback: the ratio of one repeat's spectrum to the next is exactly fb x |H(f)|\n");
    {
        const double fbPct = 95.0;
        DelayBoard b;
        delayAt(b, 300.0, fbPct, 5.0, 100.0, 0, false);
        const size_t d = size_t(kRate * 0.3);
        const size_t n = d * 5;
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1.0;
        b.run(l.data(), r.data(), n);

        const int m = 1 << 14;
        auto echoSpectrum = [&](size_t k) {
            std::vector<double> w(size_t(m), 0.0);
            const size_t from = k * d - d / 3;
            for (size_t i = 0; i < size_t(m) && from + i < n && i < d * 2 / 3; ++i)
                w[i] = l[from + i];
            return magnitudeOf(w, m);
        };
        const std::vector<double> s1 = echoSpectrum(1), s2 = echoSpectrum(2), s3 = echoSpectrum(3);

        for (double f : {200.0, 500.0, 1000.0, 2000.0}) {
            const int k = int(f * m / kRate + 0.5);
            const double fBin = double(k) * kRate / m;
            const double want =
                20.0 * std::log10(fbPct * 0.01
                                  * std::abs(loopFilterH(5.0, 2.0 * kPi * fBin / kRate)));
            char label[64];
            snprintf(label, sizeof(label), "repeat 2/1 at %.0f Hz", fBin);
            check(label, 20.0 * std::log10(s2[size_t(k)] / s1[size_t(k)]), want, 0.02);
            snprintf(label, sizeof(label), "repeat 3/2 at %.0f Hz", fBin);
            check(label, 20.0 * std::log10(s3[size_t(k)] / s2[size_t(k)]), want, 0.02);
        }
    }

    // Ping-pong, verified by CHANNEL rather than by listening: repeat 1 belongs to the left, 2 to
    // the right, 3 to the left again.
    printf("ping-pong: the repeats alternate channels, and off, the two loops never meet\n");
    {
        DelayBoard b;
        delayAt(b, 100.0, 60.0, 5.0, 100.0, 0, true);
        const size_t d = size_t(kRate * 0.1);
        const size_t n = d * 5;
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1.0; // correlated, which is what the cabinet hands over
        b.run(l.data(), r.data(), n);

        auto energyDb = [&](const std::vector<double> &v, size_t k) {
            double e = 0.0;
            for (size_t i = k * d - d / 4; i < k * d + d / 4 && i < n; ++i)
                e += v[i] * v[i];
            return 10.0 * std::log10(std::max(1e-300, e));
        };
        double worstSep = 1e9;
        for (int k = 1; k <= 4; ++k) {
            const double le = energyDb(l, size_t(k)), re = energyDb(r, size_t(k));
            const bool wantLeft = (k % 2) == 1;
            const double sep = wantLeft ? le - re : re - le;
            printf("    repeat %d: L %7.1f dB, R %7.1f dB — expected on the %s, by %.1f dB\n", k,
                   le, re, wantLeft ? "left " : "right", sep);
            worstSep = std::min(worstSep, sep);
        }
        if (worstSep < 40.0) {
            fprintf(stderr, "pedalcheck: a ping-pong repeat is only %.1f dB louder on the channel "
                            "it belongs to than on the other one\n",
                    worstSep);
            ++gFailures;
        }

        // With ping-pong OFF the two loops are independent, and the strongest form of that claim
        // is the one worth asserting: an impulse on the left alone must leave the right BIT-EXACTLY
        // untouched. A crossfeed of any size at all fails this.
        DelayBoard s;
        delayAt(s, 100.0, 60.0, 5.0, 100.0, 0, false);
        std::vector<double> l2(n, 0.0), r2(n, 0.0);
        l2[0] = 1.0;
        s.run(l2.data(), r2.data(), n);
        double leak = 0.0;
        for (size_t i = 0; i < n; ++i)
            leak = std::max(leak, std::fabs(r2[i]));
        check("crossfeed with ping-pong off", leak, 0.0, 0.0, "");
    }

    // Mix at 0 is the dry signal and nothing else — the same bit-exactness the Chorus is held to,
    // and what makes a pedal that is engaged but turned down cost the player nothing.
    printf("mix at 0 %%: the output is the input, bit for bit\n");
    {
        DelayBoard b;
        delayAt(b, 250.0, 80.0, 5.0, 0.0, 0, false);
        const size_t n = size_t(kRate * 0.5);
        std::vector<double> l(n), r(n), refL(n), refR(n);
        for (size_t i = 0; i < n; ++i) {
            l[i] = refL[i] = std::sin(2.0 * kPi * 220.0 * double(i) / kRate);
            r[i] = refR[i] = std::sin(2.0 * kPi * 310.0 * double(i) / kRate);
        }
        b.run(l.data(), r.data(), n);
        double worst = 0.0;
        for (size_t i = 0; i < n; ++i)
            worst = std::max(worst, std::max(std::fabs(l[i] - refL[i]), std::fabs(r[i] - refR[i])));
        check("worst |out - in| at Mix 0", worst, 0.0, 0.0, "");
    }

    // Zipper, by the same self-calibrating comparison the Flanger uses: a per-block control steps
    // at block boundaries and nowhere else. Time is included even though its smoother is
    // deliberately the slowest in the tree, because "deliberately slow" and "not smoothed at all"
    // look identical until they are measured.
    printf("no zipper under a dragged control: block-boundary steps vs steps inside a block\n");
    {
        // TONE IS NOT IN THIS LIST, and that is a finding rather than an omission: a per-block
        // Tone was tried as a deliberate fault and this test did not catch it. It cannot. A
        // one-pole's output is continuous in its coefficient — a step of da moves the output by
        // da*(x - lp), which is a fraction of a step the signal's own slope already exceeds —
        // where a stepped delay TIME teleports the read head and a stepped gain steps the level
        // outright. So Tone is asserted by its settling time instead, below.
        for (int which : {Delay::kTime, Delay::kFeedback, Delay::kMix}) {
            DelayBoard b;
            // MIX AT 100 %, and a stimulus whose own slope is gentle, because the first version of
            // this had no teeth at all and the numbers said so: at Mix 50 % with a 700 Hz tone,
            // three different controls reported the same worst step to six digits, which was the
            // DRY sine's own sample-to-sample slope and nothing to do with the delay. All wet, and
            // at 200 Hz, the floor this compares against is 0.026 instead of 0.046, and what is
            // being measured is the delayed signal rather than the input.
            // MIX GETS ITS OWN OPERATING POINT, and the choice is what gives that one teeth. A
            // mix step moves the output by the step times (wet - dry), so it is invisible while
            // the two are similar — at 200 Hz through a 200 ms line they are merely uncorrelated,
            // and a deliberately per-block Mix walked past the first version of this. At the
            // shortest delay the knob offers, 20 ms, a 175 Hz tone comes back three and a half
            // periods later, which is ANTIPHASE: wet is -dry, the difference is 2x, and Mix maps
            // straight onto amplitude.
            const bool antiphase = (which == Delay::kMix);
            const double baseMs = antiphase ? 20.0 : 200.0;
            const double stimHz = antiphase                  ? 75.0
                                  : (which == Delay::kTime)  ? 200.0
                                                             : 100.0;
            // FEEDBACK IS DRAGGED DOWNWARDS FROM ITS MAXIMUM, and with Tone wide open, and both
            // are what give that one teeth. A step in feedback is a step of dfb times whatever is
            // ALREADY GOING ROUND, so dragging up from zero measures it against an echo that is
            // still nearly silent; and the step is then written through the loop's own low-pass,
            // which at Tone 5 passes only a third of it on the sample it lands. Loaded, and at
            // Tone 10, the same fault moves the figure by ten times as much.
            const bool loaded = (which == Delay::kFeedback);
            delayAt(b, baseMs, loaded ? 95.0 : 60.0, loaded ? 10.0 : 5.0, 100.0, 0, false);
            b.silence(size_t(kRate * 0.3));
            if (loaded) {
                // AND THE LOOP HAS TO BE FULL BEFORE THE DRAG, which is the third thing this one
                // needed. At 95 % feedback the loop builds with a time constant of the delay over
                // (1 - fb), so 4 s at 200 ms; scored half a second in it has been round twice and
                // is carrying about 0.3, which is why a step of dfb TIMES WHAT IS GOING ROUND was
                // too small to see. Two seconds of the tone first — 100 Hz is a multiple of the
                // comb's own 5 Hz spacing, so it lands on a peak and the loop loads properly.
                std::vector<double> pl(size_t(kRate * 2.0)), pr(pl.size());
                for (size_t i = 0; i < pl.size(); ++i)
                    pl[i] = pr[i] = std::sin(2.0 * kPi * stimHz * double(i) / kRate);
                b.run(pl.data(), pr.data(), pl.size());
            }

            const double lo = (which == Delay::kTime)       ? 200.0
                              : (which == Delay::kFeedback) ? 95.0
                                                            : 0.0;
            const double hi = (which == Delay::kTime)       ? 600.0
                              : (which == Delay::kFeedback) ? 0.0
                                                            : 100.0;
            // The line has to be FULL of signal before anything is measured, or at Mix 100 % the
            // first delay's worth of output is the silence that primed it and every step reads as
            // zero — which is how the first run of this reported a perfect result while measuring
            // nothing at all. So the drag runs over six tenths of a second and only the last two
            // are scored, by which time the longest delay asked for here has been through twice.
            const size_t n = size_t(kRate * 0.6);
            const size_t from = size_t(kRate * 0.5);
            const size_t span = n - from;

            // WHEN THE KNOB IS DRAGGED IS PART OF THE TEST, and getting it wrong is why a
            // deliberately per-block Feedback walked past the first two versions of this. Time and
            // Mix act on the sample in hand — one moves the read head, the other mixes the output —
            // so they are dragged across the window being scored. Feedback acts on what is
            // WRITTEN, and what is written is not heard until one delay later, so it is dragged one
            // delay BEFORE the window instead. The 200 ms it is measured at is 9600 samples, which
            // is exactly 75 blocks, so a step written at a block boundary still arrives at one; at
            // a delay that was not a whole number of blocks the step would land somewhere inside a
            // block and inflate the very figure it is being compared against.
            const size_t sweepFrom = (which == Delay::kFeedback)
                                         ? from - size_t(baseMs * kRate * 0.001)
                                         : from;
            std::vector<double> l(n), r(n);
            for (size_t i = 0; i < n; ++i)
                l[i] = r[i] = std::sin(2.0 * kPi * stimHz * double(i) / kRate);

            double boundary = 0.0, inside = 0.0;
            for (size_t i = 0; i < n; i += kBlock) {
                const size_t k = std::min<size_t>(kBlock, n - i);
                const double t =
                    (i < sweepFrom) ? 0.0 : std::min(1.0, double(i - sweepFrom) / double(span));
                b.set(which, lo + (hi - lo) * t);
                const double before = (i > 0) ? l[i - 1] : 0.0;
                b.run(l.data() + i, r.data() + i, k);
                if (i < from)
                    continue;
                if (i > 0)
                    boundary = std::max(boundary, std::fabs(l[i] - before));
                for (size_t j = 1; j < k; ++j)
                    inside = std::max(inside, std::fabs(l[i + j] - l[i + j - 1]));
            }
            const char *name = which == Delay::kTime       ? "Time"
                               : which == Delay::kFeedback ? "Repeats"
                                                           : "Mix";
            printf("    dragging %-7s: worst step at a boundary %.6f, inside a block %.6f\n", name,
                   boundary, inside);
            if (boundary > inside * 1.5) {
                fprintf(stderr, "pedalcheck: dragging %s steps %.2fx harder at block boundaries "
                                "than inside a block — the control is not smoothed per sample\n",
                        name, boundary / std::max(1e-12, inside));
                ++gFailures;
            }
        }
    }

    // Ping-pong is a SWITCH, so it cannot be dragged and the zipper test above has nothing to say
    // about it — but stomping it is a change of topology, which is exactly the kind of thing that
    // steps. Its effect reaches the output one delay later, like feedback's and for the same
    // reason, so it is scored one delay after the stomp at a delay that is a whole number of
    // blocks (200 ms is 9600 samples, which is 75 of them).
    printf("ping-pong is stomped, not stepped: the crossfade reaches the output smoothly\n");
    {
        DelayBoard b;
        delayAt(b, 200.0, 70.0, 10.0, 100.0, 0, false);
        const size_t d = size_t(kRate * 0.2);
        const size_t n = d * 5;
        std::vector<double> l(n), r(n);
        // 101 Hz AND NOT 100, and the odd number is the whole test. What ping-pong changes when
        // the two channels carry the same signal is the RIGHT line's injection, which goes from x
        // to zero — so the step it makes is the size of x at the instant of the stomp. The stomp
        // is at 0.6 s, and any frequency that fits a whole number of periods into 0.6 s is at a
        // zero crossing there: at 100 Hz the step is exactly nothing, and a hard switch and a
        // crossfade produce bit-identical output. 101 Hz lands at 0.59 of full scale instead.
        for (size_t i = 0; i < n; ++i)
            l[i] = r[i] = std::sin(2.0 * kPi * 101.0 * double(i) / kRate);

        const size_t stomp = d * 3;
        for (size_t i = 0; i < n; i += kBlock) {
            const size_t k = std::min<size_t>(kBlock, n - i);
            b.set(Delay::kPingPong, i >= stomp ? 1.0 : 0.0);
            b.run(l.data() + i, r.data() + i, k);
        }
        double before = 0.0, after = 0.0;
        for (size_t i = 1; i < n; ++i) {
            const double step = std::max(std::fabs(l[i] - l[i - 1]), std::fabs(r[i] - r[i - 1]));
            if (i > d && i < stomp)
                before = std::max(before, step);
            else if (i > stomp && i < stomp + 2 * d)
                after = std::max(after, step);
        }
        printf("    worst step before the stomp %.5f, in the two delays after it %.5f\n", before,
               after);
        if (after > before * 1.5) {
            fprintf(stderr, "pedalcheck: stomping ping-pong steps %.2fx harder than the signal's "
                            "own slope — the routing is switched rather than crossfaded\n",
                    after / std::max(1e-12, before));
            ++gFailures;
        }
    }

    // Tone sweeps rather than steps, asserted by how long it TAKES rather than by what it does to
    // any one sample. A control smoothed per sample over kSmoothSec cannot reach its new value
    // inside a block; one applied per block reaches it inside one, which is the whole difference
    // and is the only form of it this signal can see.
    printf("Tone sweeps rather than steps: the response settles over tens of ms, not one block\n");
    {
        DelayBoard b;
        delayAt(b, 20.0, 0.0, 0.0, 100.0, 0, false);
        const size_t n = size_t(kRate * 0.4);
        std::vector<double> l(n), r(n);
        for (size_t i = 0; i < n; ++i)
            l[i] = r[i] = std::sin(2.0 * kPi * 5000.0 * double(i) / kRate);

        // A 5 kHz tone, so the two ends of Tone are far apart in level: the low-pass sits at
        // 800 Hz at one end and 12 kHz at the other.
        const size_t flip = size_t(kRate * 0.2);
        b.run(l.data(), r.data(), flip);
        b.set(Delay::kTone, 10.0);
        b.run(l.data() + flip, r.data() + flip, n - flip);

        // Envelope by peak over a 1 ms window, and the 10 % to 90 % crossing of the change.
        const size_t w = size_t(kRate * 0.001);
        auto env = [&](size_t at) {
            double e = 0.0;
            for (size_t i = at; i < at + w && i < n; ++i)
                e = std::max(e, std::fabs(l[i]));
            return e;
        };
        const double before = env(flip - 3 * w);
        const double after = env(n - 2 * w);
        double t10 = -1.0, t90 = -1.0;
        for (size_t i = flip; i + w < n; i += w) {
            const double e = (env(i) - before) / std::max(1e-12, after - before);
            if (t10 < 0.0 && e >= 0.1)
                t10 = double(i - flip) / kRate * 1000.0;
            if (t90 < 0.0 && e >= 0.9)
                t90 = double(i - flip) / kRate * 1000.0;
        }
        // THE RISE, from 10 % to 90 %, and NOT the time from the push — which is what the first
        // version of this measured and is why a deliberately per-block Tone walked straight past
        // it. The delay line is 20 ms long, so the change cannot appear at the output until 20 ms
        // after the push however the coefficient is applied; a threshold on the arrival is a
        // threshold on the delay time. The rise itself is the smoother and nothing else.
        const double rise = t90 - t10;
        printf("    5 kHz level %.4f -> %.4f; 10 %% at %.1f ms, 90 %% at %.1f ms, rise %.1f ms\n",
               before, after, t10, t90, rise);
        // A one-pole at kSmoothSec spans 10 % to 90 % in about 2.2 time constants, so 44 ms; a
        // snapped coefficient rises only as fast as the filter behind it settles, which is a
        // handful of samples.
        if (rise < 10.0) {
            fprintf(stderr, "pedalcheck: Tone went from 10 %% to 90 %% of its change in %.1f ms — "
                            "that is the filter settling and not a smoother, so the coefficient is "
                            "being applied per block\n",
                    rise);
            ++gFailures;
        }
        if (rise > 150.0) {
            fprintf(stderr, "pedalcheck: Tone took %.1f ms to cross its own change, which is a "
                            "knob that lags the hand turning it\n",
                    rise);
            ++gFailures;
        }
    }

    //==============================================================================================
    // THE REVERB. Every expected number below comes from PASP's account of what the vendored
    // engine's two settings mean, or from the engine's own tuning table written out again above.
    //==============================================================================================
    printf("\n-- Reverb -----------------------------------------------------------------------\n");

    auto reverbAt = [](ReverbBoard &b, double decay, double tone, double preMs, double mixPct) {
        b.set(Reverb::kDecay, decay);
        b.set(Reverb::kTone, tone);
        b.set(Reverb::kPreDelay, preMs);
        b.set(Reverb::kMix, mixPct);
        b.restart();
    };

    // The constant that converts one comb's decay into the bank's, derived here from the ten comb
    // lengths and compared against the literal the pedal carries. This is the whole of what makes
    // the Decay knob's map a derivation rather than a curve fitted to a previous run.
    printf("the ensemble T60 ratio, derived from the ten comb lengths\n");
    {
        const double r48 = ensembleT60Ratio(kRate);
        const double r44 = ensembleT60Ratio(44100.0);
        const double r96 = ensembleT60Ratio(96000.0);
        check("ensemble ratio at 48 kHz vs reverb.h's own constant", r48,
              reverbdef::kEnsembleT60Ratio, 0.005, "");
        // If this were rate-dependent the pedal's single constant would be wrong at every rate but
        // one, and T60 would silently follow the sample rate.
        check("the same ratio at 44.1 kHz", r44, r48, 0.002, "");
        check("the same ratio at 96 kHz", r96, r48, 0.002, "");
    }

    // T60 against the Decay control — the phase's own milestone.
    printf("T60 against Decay, by Schroeder backward integration and a -5..-35 dB fit\n");
    {
        double worstErr = 0.0;
        for (double decay = 0.0; decay <= 10.001; decay += 1.25) {
            ReverbBoard b;
            // Tone wide open, so the tail is undamped and what is measured is f alone; PASP: at
            // d = 0 "the LBCF reduces to the feedback comb filter ... in which the feedback was
            // not filtered".
            reverbAt(b, decay, 10.0, 0.0, 100.0);
            const double want = expectedT60(decay);
            // Long enough for the fit window to close at the longest decay the knob offers.
            const std::vector<double> ir = verbWetIr(b, size_t(kRate * (2.5 * want + 1.0)));
            const double got = t60Of(ir, kRate);
            const double relPct = 100.0 * (got / want - 1.0);
            worstErr = std::max(worstErr, std::fabs(relPct));
            if (gVerbose)
                printf("    Decay %4.2f: T60 %7.3f s, asked for %7.3f s (%+.2f %%)\n", decay, got,
                       want, relPct);
        }
        // 3 %, against a measured worst of 1.35 %. The residual has a name and is not noise: the
        // six diffusion allpasses ring at a fixed g = 0.5 whatever f is, so they add a decay the
        // ensemble derivation does not model, and they add relatively more of it at the short end —
        // which is the direction the error drifts. Above the measured worst rather than at it, the
        // rule the alias and IR-blend gates follow.
        check("worst T60 error over the whole Decay knob", worstErr, 0.0, 3.0, "%");
    }

    // AND DECAY HAS TO WORK WHILE THE PEDAL IS RUNNING, which is not the same statement as the one
    // above and is not implied by it. Every measurement so far restarts the pedal at the setting it
    // is about to measure, so all of them would pass a build whose knob only took effect on a
    // reset — and that fault was tried and did pass them. The engine's feedback and damping live
    // inside thirty-two vendored filter objects and reach them only through a Reset(false) that the
    // wrapper calls itself, guarded on the value having changed; this is what says the guard lets a
    // change through. A user turning Decay mid-song is the ordinary case, not an edge one.
    printf("Decay takes effect on a running pedal, not only on a reset\n");
    {
        ReverbBoard b;
        reverbAt(b, 0.0, 10.0, 0.0, 100.0); // restarted at the SHORT end
        b.silence(size_t(kRate * 0.5));
        b.set(Reverb::kDecay, 10.0); // and moved to the long end with no restart at all
        b.silence(size_t(kRate * 0.5)); // long enough for the smoother to land
        std::vector<double> l(size_t(kRate * 16.0), 0.0), r(l.size(), 0.0);
        l[0] = r[0] = 1.0;
        b.run(l.data(), r.data(), l.size());
        const double got = t60Of(l, kRate);
        printf("    Decay driven 0 -> 10 without a reset: T60 %.3f s\n", got);
        check("T60 after a mid-run Decay change", got, expectedT60(10.0),
              0.03 * expectedT60(10.0), "s");
    }

    // T60 must not follow the sample rate. The engine scales its own tuning table by srate/44100
    // and the pedal derives its lap length the same way, so the two have to agree — and if either
    // drifted, a project would change its decay time when the interface did.
    printf("T60 does not follow the sample rate\n");
    {
        double at[2] = {0.0, 0.0};
        const double rates[2] = {44100.0, kRate};
        for (int k = 0; k < 2; ++k) {
            ReverbBoard b(rates[k]);
            reverbAt(b, 5.0, 10.0, 0.0, 100.0);
            at[k] = t60Of(verbWetIr(b, size_t(rates[k] * 5.0)), rates[k]);
        }
        printf("    Decay 5: %.3f s at 44.1 kHz, %.3f s at 48 kHz\n", at[0], at[1]);
        check("T60 at 44.1 kHz against T60 at 48 kHz", at[0], at[1], 0.03 * at[1], "s");
    }

    // TONE IS DAMPING, NOT A LOW-PASS ACROSS THE OUTPUT, and this is the check that tells the two
    // apart. PASP: "the roomsize parameter can be interpreted as setting the low-frequency T60,
    // while the damping parameter controls how rapidly T60 shortens as a function of increasing
    // frequency". So the low band's decay must ignore Tone and the high band's must follow it. A
    // low-pass on the wet output would sound similar on a first listen, would pass any test that
    // only looked at the spectrum of the tail, and fails this: it shortens NOTHING.
    printf("Tone is the engine's damping: T60 by band, against PASP's own sentence\n");
    {
        double lowAt[3] = {0, 0, 0}, highAt[3] = {0, 0, 0};
        const double tones[3] = {0.0, 5.0, 10.0};
        for (int k = 0; k < 3; ++k) {
            ReverbBoard b;
            reverbAt(b, 5.0, tones[k], 0.0, 100.0);
            const std::vector<double> ir = verbWetIr(b, size_t(kRate * 6.0));
            lowAt[k] = t60Of(bandOf(ir, kRate, 100.0, 400.0), kRate);
            highAt[k] = t60Of(bandOf(ir, kRate, 4000.0, 9000.0), kRate);
            printf("    Tone %4.1f: T60 is %.3f s from 100-400 Hz and %.3f s from 4-9 kHz\n",
                   tones[k], lowAt[k], highAt[k]);
        }
        // The low band is the room size and nothing else. 3 % over the whole Tone knob.
        check("low-band T60 at Tone 0 against Tone 10", lowAt[0], lowAt[2], 0.03 * lowAt[2], "s");
        // The high band has to move, and by enough that it is the control and not a rounding.
        // Measured: 1.117 s at full damping against 1.532 s at none, which is 27 % shorter.
        const double shortenPct = 100.0 * (1.0 - highAt[0] / std::max(1e-9, highAt[2]));
        printf("    full damping shortens the 4-9 kHz tail by %.1f %%\n", shortenPct);
        if (shortenPct < 15.0) {
            fprintf(stderr, "pedalcheck: Tone shortens the high-frequency tail by only %.1f %% — "
                            "that is not damping in the feedback loop, which is what PASP says "
                            "the engine's second parameter does\n",
                    shortenPct);
            ++gFailures;
        }
        // And it has to be monotonic, or the knob does two things at once somewhere in the middle.
        if (!(highAt[0] < highAt[1] && highAt[1] < highAt[2])) {
            fprintf(stderr, "pedalcheck: the high-band tail is not monotonic in Tone: %.3f, %.3f, "
                            "%.3f s\n",
                    highAt[0], highAt[1], highAt[2]);
            ++gFailures;
        }
    }

    // Pre-delay, to the sample. The engine has no feedthrough — a comb's output is what its line
    // holds, so nothing reaches the output before the SHORTEST comb has run once — which makes the
    // arrival of the first wet sample an exact quantity: the pre-delay plus that comb's length.
    printf("pre-delay, measured as the arrival of the first wet sample\n");
    {
        std::vector<int> lens = combLengths(kRate);
        const int shortest = *std::min_element(lens.begin(), lens.end());
        for (double ms : {0.0, 10.0, 20.0, 50.0, 100.0, 200.0}) {
            ReverbBoard b;
            reverbAt(b, 5.0, 10.0, ms, 100.0);
            const std::vector<double> ir = verbWetIr(b, size_t(kRate * 0.6));
            long first = -1;
            for (size_t i = 1; i < ir.size(); ++i)
                if (std::fabs(ir[i]) > 1e-14) {
                    first = static_cast<long>(i);
                    break;
                }
            // The line's own floor is one sample, because the cubic kernel reaches one sample
            // newer than the read point and x[n] is the newest there is — so Pre at 0 is 21 us
            // rather than nothing. reverb.h says so at the read; this is what holds it to it.
            const long want =
                static_cast<long>(std::max(1.0, std::round(ms * kRate * 0.001))) + shortest;
            check(("first wet sample at Pre " + std::to_string(int(ms)) + " ms").c_str(),
                  double(first), double(want), 0.0, "samples");
        }
    }

    // Both channels decorrelated — the phase's second milestone. The engine detunes its right bank
    // against its left by `stereospread` = 23 samples per line, and the wrapper leaves width at
    // 1.0, which is PASP's wet2 = 0, "maximally different left and right reverberation signals".
    // Fed the SAME signal on both inputs, the two tails must therefore share nothing.
    printf("both channels decorrelated, on identical input\n");
    {
        for (double decay : {0.0, 5.0, 10.0}) {
            ReverbBoard b;
            reverbAt(b, decay, 10.0, 0.0, 100.0);
            std::vector<double> l(size_t(kRate * 6.0), 0.0), r(l.size(), 0.0);
            l[0] = r[0] = 1.0;
            b.run(l.data(), r.data(), l.size());
            const double rho = correlation(l, r);
            if (gVerbose)
                printf("    Decay %4.1f: correlation between the two tails %+.5f\n", decay, rho);
            if (std::fabs(rho) > 0.10) {
                fprintf(stderr, "pedalcheck: the two reverb tails correlate at %+.4f on identical "
                                "input — the stereo banks are not detuned against each other\n",
                        rho);
                ++gFailures;
            }
        }

        // THE ENGINE IS TRUE STEREO, and this is what says so — the check the correlation test
        // above CANNOT make, because it feeds both channels the same signal and so cannot tell a
        // pedal that keeps them apart from one that quietly feeds both banks from the left input.
        // That fault was tried and walked straight past it. At width 1.0 there is no cross-feed at
        // all (PASP's wet2 = 0), so an impulse on the LEFT alone must leave the right output
        // holding nothing but its own dry silence — bit-zero, not merely quiet.
        {
            ReverbBoard t;
            reverbAt(t, 5.0, 10.0, 0.0, 100.0);
            std::vector<double> l(size_t(kRate * 2.0), 0.0), r(l.size(), 0.0);
            l[0] = 1.0;
            t.run(l.data(), r.data(), l.size());
            double leak = 0.0, sig = 0.0;
            for (size_t i = 0; i < r.size(); ++i) {
                leak = std::max(leak, std::fabs(r[i]));
                sig = std::max(sig, std::fabs(l[i]));
            }
            printf("    an impulse on the left alone: left peaks at %.4f, right at %.4g\n", sig,
                   leak);
            check("cross-feed from the left input to the right output", leak, 0.0, 0.0, "");
        }

        // AND THE MEASUREMENT HAS TO BE ABLE TO REPORT A ONE, or "uncorrelated" means nothing. At
        // Mix 0 the output is the input, which is the same signal on both channels, so this is the
        // same statistic over a case whose answer is known.
        ReverbBoard b;
        reverbAt(b, 5.0, 10.0, 0.0, 0.0);
        std::vector<double> l(size_t(kRate * 0.5)), r(l.size());
        for (size_t i = 0; i < l.size(); ++i)
            l[i] = r[i] = std::sin(2.0 * kPi * 220.0 * double(i) / kRate);
        b.run(l.data(), r.data(), l.size());
        check("the same statistic on two channels that ARE the same", correlation(l, r), 1.0, 1e-12,
              "");
    }

    // The wet level, against the prediction made from the engine's structure. Two claims: that it
    // does not move as Decay is swept, which is what makes the Mix knob stay put, and that it is
    // unity, which is what makes Mix at 100 % a replacement for the dry signal rather than a jump.
    printf("wet level: flat across Decay, and equal to what the structure predicts\n");
    {
        // A deterministic broadband stimulus, so the figure is repeatable: a chirp covering the
        // band, which has flat magnitude by construction and needs no generator.
        const size_t n = size_t(kRate * 8.0);
        std::vector<double> src(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / kRate;
            src[i] = std::sin(2.0 * kPi * (20.0 * t + 0.5 * 2400.0 * t * t));
        }
        const double inRms = std::pow(10.0, rmsDb(src) / 20.0);
        double lo = 1e9, hi = -1e9;
        for (double decay = 0.0; decay <= 10.001; decay += 2.5) {
            ReverbBoard b;
            reverbAt(b, decay, 10.0, 0.0, 100.0);
            std::vector<double> l = src, r = src;
            b.run(l.data(), r.data(), n);
            // Skip the first two seconds: the tail has to be established before its level means
            // anything, and at Decay 10 that takes a while.
            const double gainDb = rmsDb(l, size_t(kRate * 2.0)) - 20.0 * std::log10(inRms);
            if (gVerbose)
                printf("    Decay %4.1f: wet gain %+.3f dB\n", decay, gainDb);
            lo = std::min(lo, gainDb);
            hi = std::max(hi, gainDb);
        }
        printf("    wet gain spans %+.3f to %+.3f dB over the whole Decay knob\n", lo, hi);
        // The compensation's whole job. Without it this spread is 9.75 dB.
        check("spread of the wet level across Decay", hi - lo, 0.0, 0.5, "dB");
        // kWetNorm is the reciprocal of what the structure gives, derived here from the tuning
        // table and the allpass gain rather than read out of the pedal.
        check("reverb.h's kWetNorm against the structural derivation", reverbdef::kWetNorm,
              1.0 / predictedRawWetGain(), 0.01, "");
        // And the product of the two is unity, which is what Mix at 100 % being a REPLACEMENT for
        // the dry signal means. Measured 0.12 dB under, which is the 1.4 % the derivation's
        // "the cross terms vanish" step waves away.
        check("wet level at Mix 100 %", 0.5 * (lo + hi), 0.0, 0.5, "dB");
    }

    // Mix at 0 is the pedal not being there at all — bit-identical, not merely quiet, because
    // anything else means an engaged-but-dry pedal colours the signal.
    printf("Mix at 0 leaves the signal bit-identical\n");
    {
        ReverbBoard b;
        reverbAt(b, 7.0, 3.0, 40.0, 0.0);
        const size_t n = size_t(kRate * 0.5);
        std::vector<double> l(n), r(n);
        for (size_t i = 0; i < n; ++i) {
            l[i] = std::sin(2.0 * kPi * 220.0 * double(i) / kRate);
            r[i] = std::sin(2.0 * kPi * 330.0 * double(i) / kRate);
        }
        const std::vector<double> refL = l, refR = r;
        b.run(l.data(), r.data(), n);
        double worst = 0.0;
        for (size_t i = 0; i < n; ++i)
            worst = std::max(worst, std::max(std::fabs(l[i] - refL[i]), std::fabs(r[i] - refR[i])));
        check("worst |out - in| at Mix 0", worst, 0.0, 0.0, "");
    }

    // The tail reaches silence — the phase's third milestone — and gets there without spending
    // time in the subnormal range on the way.
    //
    // SEEDED 300 DECADES DOWN, for the Delay's reason and by the Delay's licence: this is a linear
    // system, so an impulse of 1e-300 is the identical signal some forty T60s later, and measuring
    // it there costs seconds instead of the ten minutes an impulse of 1.0 would need to decay into
    // the subnormals. Decay at 0 for the shortest T60 the knob offers.
    printf("the tail decays to exact zero, with no subnormals on the way\n");
    {
        ReverbBoard b;
        reverbAt(b, 0.0, 10.0, 0.0, 100.0);
        const size_t n = size_t(kRate * 4.0);
        std::vector<double> l(n, 0.0), r(n, 0.0);
        l[0] = r[0] = 1e-300;
        b.run(l.data(), r.data(), n);
        if (hasSubnormal(l) || hasSubnormal(r)) {
            fprintf(stderr, "pedalcheck: the reverb tail passes subnormal samples to its output\n");
            ++gFailures;
        }
        double tail = 0.0;
        for (size_t i = size_t(kRate * 3.0); i < n; ++i)
            tail = std::max(tail, std::max(std::fabs(l[i]), std::fabs(r[i])));
        check("largest sample in the last second of a 4 s tail", tail, 0.0, 0.0, "");
    }

    // Zipper, by the same self-calibrating comparison the Flanger and the Delay use.
    //
    // ONLY MIX IS IN THIS LIST, and the other three knobs are absent because the fault sweep proved
    // this measurement cannot see them — not because they were forgotten. It is worth setting out
    // why, because the reason is a property of the pedal rather than of the test, and it is the
    // same reason for all three.
    //
    // Mix scales the OUTPUT, so a step in it is a step in the output on the sample it lands. Decay,
    // Tone and Pre-delay all act UPSTREAM of the engine — the first two on what is written into the
    // ten comb lines, the third on what is fed to them — and everything upstream of the engine
    // reaches the output only through the engine, which begins by multiplying its input by
    // Freeverb's `fixedgain` of 0.015. That is 36 dB of attenuation before anything can be
    // measured, and what survives it is then spread across ten comb lengths that are not whole
    // multiples of the block, of the coefficient chunk, or of each other. A step arrives as ten
    // unaligned, attenuated contributions inside a tail that is already dense.
    //
    // Measured rather than argued: snapping Decay, Tone or Pre-delay outright — no smoothing at all
    // — changes nothing this file can detect, and neither does widening the engine's coefficient
    // chunk from 16 samples to a whole block. Snapping Mix is caught at 2.25x. So Mix is gated here
    // and the other three are gated by what they can be seen to do: Pre-delay by the arrival of the
    // first wet sample, to the sample, at six settings; Decay by T60 to 3 %; Tone by the split
    // between the two bands. Their smoothers are margin, and reverb.h says so.
    printf("no zipper under a dragged control: block-boundary steps vs steps inside a block\n");
    {
        ReverbBoard b;
        // ALL WET, and at 25 Hz. A mix step moves the output by the step times (wet - dry), so what
        // this can see is bounded below by the stimulus's own sample-to-sample slope — and the
        // first version ran at 150 Hz, where that slope is 0.0196 and a deliberately per-block Mix
        // (a step of 0.004 a block) sat comfortably underneath it. At 25 Hz the floor is 0.0057 and
        // the same fault stands 2.25x clear of it. The Delay's first version of this made the same
        // mistake in the same place.
        reverbAt(b, 5.0, 10.0, 40.0, 100.0);
        b.silence(size_t(kRate * 0.3));
        const size_t n = size_t(kRate * 1.2);
        const size_t from = size_t(kRate * 0.6);
        // The drag is short on purpose: what is being looked for is one block's worth of change, so
        // a slower drag shrinks the fault without lowering the floor it is measured against.
        const size_t span = size_t(kRate * 0.3);
        std::vector<double> l(n), r(n);
        for (size_t i = 0; i < n; ++i)
            l[i] = r[i] = std::sin(2.0 * kPi * 25.0 * double(i) / kRate);

        double boundary = 0.0, inside = 0.0;
        for (size_t i = 0; i < n; i += kBlock) {
            const size_t k = std::min<size_t>(kBlock, n - i);
            const double t = (i < from) ? 0.0 : std::min(1.0, double(i - from) / double(span));
            b.set(Reverb::kMix, 100.0 * t);
            const double before = (i > 0) ? l[i - 1] : 0.0;
            b.run(l.data() + i, r.data() + i, k);
            if (i < from || i > from + span)
                continue;
            boundary = std::max(boundary, std::fabs(l[i] - before));
            for (size_t j = 1; j < k; ++j)
                inside = std::max(inside, std::fabs(l[i + j] - l[i + j - 1]));
        }
        printf("    dragging Mix: worst step at a boundary %.6f, inside a block %.6f\n", boundary,
               inside);
        if (boundary > inside * 1.5) {
            fprintf(stderr, "pedalcheck: dragging Mix steps %.2fx harder at block boundaries than "
                            "inside a block — the control is not smoothed per sample\n",
                    boundary / std::max(1e-12, inside));
            ++gFailures;
        }
    }

    // Slamming Decay and Tone from one end of their travel to the other in a single push, which is
    // the worst thing a host automating either can do.
    //
    // WHAT THIS ACTUALLY GUARDS is the wet-level compensation, and that is worth naming rather than
    // leaving as "no zipper on Decay". A Decay move changes two things: the comb feedback, which
    // the note above shows cannot step the output, and the sqrt(1 - f^2) wet gain, which is a plain
    // multiplier on the output and steps it by a factor of three across this slam if it is not
    // ramped. Snapping that gain is caught here at 17.5x — and was NOT caught by the first version,
    // whose scoring window began one sample after the slam and so stepped over the very sample the
    // step lands on. The slam is placed on a block boundary on purpose; the window starts on it.
    printf("slamming Decay and Tone end to end does not step the output\n");
    {
        for (int which : {Reverb::kDecay, Reverb::kTone}) {
            ReverbBoard b;
            reverbAt(b, which == Reverb::kDecay ? 0.0 : 0.0, which == Reverb::kTone ? 0.0 : 10.0,
                     0.0, 100.0);
            const size_t n = size_t(kRate * 2.0);
            const size_t slam = size_t(kRate * 1.0);
            std::vector<double> l(n), r(n);
            for (size_t i = 0; i < n; ++i)
                l[i] = r[i] = std::sin(2.0 * kPi * 150.0 * double(i) / kRate);
            for (size_t i = 0; i < n; i += kBlock) {
                const size_t k = std::min<size_t>(kBlock, n - i);
                b.set(which, i >= slam ? 10.0 : 0.0);
                b.run(l.data() + i, r.data() + i, k);
            }
            double before = 0.0, after = 0.0;
            for (size_t i = 1; i < n; ++i) {
                const double step = std::fabs(l[i] - l[i - 1]);
                if (i > size_t(kRate * 0.5) && i < slam)
                    before = std::max(before, step);
                // FROM `slam`, NOT FROM slam + 1. The push lands on the first block boundary at
                // or after the slam, and the slam is placed on one — so the step this is looking
                // for is l[slam] - l[slam-1], which a window starting at slam + 1 steps over. A
                // deliberately snapped wet-level compensation, which steps the output by a factor
                // of three, walked past the first version of this for exactly that one index.
                else if (i >= slam && i < slam + size_t(kRate * 0.3))
                    after = std::max(after, step);
            }
            const char *name = (which == Reverb::kDecay) ? "Decay" : "Tone";
            printf("    slamming %-5s: worst step before %.6f, after %.6f\n", name, before, after);
            if (after > before * 2.0) {
                fprintf(stderr, "pedalcheck: slamming %s steps the output %.2fx harder than the "
                                "material's own slope — the control is not ramped\n",
                        name, after / std::max(1e-12, before));
                ++gFailures;
            }
        }
    }

    // Stomping the footswitch. The base class crossfades a pedal in and out over kEngageMs, and
    // the reverb is where getting that wrong is loudest, because what is being faded is a tail that
    // outlives the fade by seconds.
    //
    // THE CONTROL IS A SPLICE, and the first version of this had none worth the name. It used
    // Mix = 0 as the "hard switch", which is not hard at all — Mix has a 20 ms smoother of its own,
    // longer than the 8 ms engage ramp it was supposed to expose — and both numbers came out at
    // 0.019766 against 0.019765, which is 2*pi*151/48000: the STIMULUS's own sample-to-sample
    // slope, and nothing to do with the pedal. The control now runs the pedal engaged for the whole
    // take and splices the dry signal in at the stomp, which is exactly what a footswitch with no
    // ramp would produce and cannot be smoothed by anything.
    printf("stomping the footswitch crossfades rather than switching\n");
    {
        const size_t n = size_t(kRate * 2.0);
        const size_t stomp = size_t(kRate * 1.0);
        const size_t window = size_t(kRate * 0.2);
        std::vector<double> dry(n);
        for (size_t i = 0; i < n; ++i)
            dry[i] = std::sin(2.0 * kPi * 151.0 * double(i) / kRate);

        auto worstStep = [&](const std::vector<double> &v) {
            double w = 0.0;
            for (size_t i = stomp; i < stomp + window; ++i)
                w = std::max(w, std::fabs(v[i] - v[i - 1]));
            return w;
        };

        ReverbBoard fade;
        reverbAt(fade, 5.0, 5.0, 0.0, 100.0);
        std::vector<double> fl = dry, fr = dry;
        fade.run(fl.data(), fr.data(), stomp);
        fade.pedal().setEngaged(false);
        fade.run(fl.data() + stomp, fr.data() + stomp, n - stomp);

        ReverbBoard held;
        reverbAt(held, 5.0, 5.0, 0.0, 100.0);
        std::vector<double> hl = dry, hr = dry;
        held.run(hl.data(), hr.data(), n);
        for (size_t i = stomp; i < n; ++i)
            hl[i] = dry[i];

        const double faded = worstStep(fl), hard = worstStep(hl);
        printf("    worst step at the stomp: %.6f faded, %.6f spliced\n", faded, hard);
        if (faded > hard * 0.25) {
            fprintf(stderr, "pedalcheck: the faded stomp steps %.6f against the splice's %.6f — "
                            "the engage ramp is not doing anything\n",
                    faded, hard);
            ++gFailures;
        }
    }


    printf("\n");
    if (gFailures) {
        fprintf(stderr, "FAILED — %d measurement(s) disagree with the circuit\n", gFailures);
        return 1;
    }
    printf("PASSED — the pedals match their sources within the stated tolerances\n");
    return 0;
}
