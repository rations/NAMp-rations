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
#include "pedals/flanger.h"
#include "pedals/primitives.h"

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

    printf("\n");
    if (gFailures) {
        fprintf(stderr, "FAILED — %d measurement(s) disagree with the circuit\n", gFailures);
        return 1;
    }
    printf("PASSED — the pedals match their sources within the stated tolerances\n");
    return 0;
}
