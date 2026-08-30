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
    const std::complex<double> num = (rl * ts9::kToneRf + y) * (s + (y / (rl * ts9::kToneRf + y)) * wz);
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
        mag[static_cast<size_t>(k)] = std::abs(spec[static_cast<size_t>(k)]) / static_cast<double>(n);

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

} // namespace

//------------------------------------------------------------------------------------------------
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--verbose") == 0)
            gVerbose = true;

    printf("rations_pedalcheck — the Boost, against Yeh §2.4\n");
    printf("  rate %.0f Hz, block %d, full scale %.1f V\n\n", kRate, kBlock, ts9::kFullScaleVolts);

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

    printf("\n");
    if (gFailures) {
        fprintf(stderr, "FAILED — %d measurement(s) disagree with the circuit\n", gFailures);
        return 1;
    }
    printf("PASSED — the Boost matches Yeh §2.4 within the stated tolerances\n");
    return 0;
}
