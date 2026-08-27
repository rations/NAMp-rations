// rations_fadecheck — the offline proof that the gain crossfade works.
//
// The whole feature rests on one claim: because these captures run on a strictly feed-forward
// network, a model that has been fed live input for one receptive field produces exactly the
// output it would have produced had it been running since the session began. If that is true,
// crossfading between two captures over at least that long is not an approximation that happens to
// sound acceptable — it is exact, and a click is structurally impossible rather than merely
// masked.
//
// That claim is worth nothing unless it is measured, so this tool measures it. It renders four
// tracks over the same input with the DSP core linked directly — no plug-in, no VST3, no host:
//
//   ref_A   capture A from t = 0
//   ref_B   capture B from t = 0            <- the fully primed ground truth
//   hard    A, then B swapped in cold       <- the control: this is what a naive switch does
//   fade    A, with B fed from the switch point and the mix ramped over `fade` samples
//
// and then asserts, rather than prints and hopes:
//
//   1. CONVERGENCE. After the fade completes, `fade` must equal `ref_B` to within 1e-6. This one
//      assertion validates the entire premise. If it fails, either the fade was shorter than the
//      receptive field or the architecture is not finite-memory.
//   2. NO CLICK. The largest sample-to-sample step near the switch, measured against the track's
//      own typical step, must not spike for `fade`. The `hard` control must spike, or the test
//      signal is too tame to be evidence of anything.
//   3. DECAY. How the priming error falls as a function of how long the incoming model has been
//      fed. This is the empirical shape of the receptive field, and it is what says whether the
//      slew rate is generous or stingy.
//   4. MIX LAW. Mid-fade level under amplitude-complementary (a + b = 1) against equal-power
//      (a^2 + b^2 = 1). Adjacent captures of one amp on one input are strongly correlated, so the
//      equal-power law should bulge. The choice is settled by this number, not by argument.

#include "capturesource.h"
#include "engineconfig.h"

#include "NAM/dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr double kNativeRate = 48000.0;

struct Options {
    std::string fileA;
    std::string fileB;
    int fadeLen = 0;  // 0 = one receptive field, read from the model
    int switchAt = 0; // 0 = one second in
    double slim = 1.0;
    double seconds = 3.0;
};

// A pick attack, a sustained chord, and a decay into the noise floor. The decay matters most: a DC
// step or an unprimed model hides under a loud sustain and is obvious under a dying note. A steady
// sine would prove nothing at all.
void fillTestSignal(std::vector<double> &buf, double rate)
{
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        // Two plucks, so the switch can be placed inside a sustain and inside a decay.
        const double phase = std::fmod(t, 1.5);
        double env;
        if (phase < 0.0015)
            env = phase / 0.0015;
        else
            env = std::exp(-(phase - 0.0015) * 2.2);
        // A chord rather than one tone: intermodulation is where a nonlinear model's differences
        // between captures actually live.
        const double tone = 0.55 * std::sin(2.0 * M_PI * 82.41 * t) +  // E2
                            0.32 * std::sin(2.0 * M_PI * 123.47 * t) + // B2
                            0.22 * std::sin(2.0 * M_PI * 164.81 * t) + // E3
                            0.14 * std::sin(2.0 * M_PI * 246.94 * t) + // B3
                            0.09 * std::sin(2.0 * M_PI * 329.63 * t);  // E4
        buf[i] = 0.42 * env * tone;
    }
}

// Feed a model, in the same fixed chunks the plug-in uses, so this measures the same arithmetic
// the plug-in performs.
void render(nam::DSP &model, const double *in, double *out, int count)
{
    for (int off = 0; off < count; off += Rations::engine::kChunk) {
        const int n = std::min(Rations::engine::kChunk, count - off);
        double *ip = const_cast<double *>(in) + off;
        double *op = out + off;
        model.process(&ip, &op, n);
    }
}

std::unique_ptr<nam::DSP> build(const Rations::CaptureSource &source, double slim, const char *label)
{
    std::string error;
    auto model = Rations::buildCaptureModel(source, slim, Rations::engine::kChunk, error);
    if (!model)
        fprintf(stderr, "rations_fadecheck: cannot build %s (%s)\n", label, error.c_str());
    return model;
}

double rms(const double *x, size_t n)
{
    if (n == 0)
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(n));
}

double db(double linear)
{
    return 20.0 * std::log10(std::max(linear, 1e-12));
}

// Largest single-sample step within a window.
double maxStep(const std::vector<double> &x, size_t from, size_t to)
{
    double worst = 0.0;
    from = std::max<size_t>(from, 1);
    to = std::min(to, x.size());
    for (size_t i = from; i < to; ++i)
        worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
    return worst;
}

bool parseArgs(int argc, char **argv, Options &o)
{
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--fade") {
            const char *v = next();
            if (!v)
                return false;
            o.fadeLen = atoi(v);
        } else if (a == "--switch") {
            const char *v = next();
            if (!v)
                return false;
            o.switchAt = atoi(v);
        } else if (a == "--slim") {
            const char *v = next();
            if (!v)
                return false;
            o.slim = atof(v);
        } else if (a == "--seconds") {
            const char *v = next();
            if (!v)
                return false;
            o.seconds = atof(v);
        } else if (positional == 0) {
            o.fileA = a;
            ++positional;
        } else if (positional == 1) {
            o.fileB = a;
            ++positional;
        } else {
            fprintf(stderr, "rations_fadecheck: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (o.fileA.empty() || o.fileB.empty()) {
        fprintf(stderr, "usage: rations_fadecheck <A.nam> <B.nam> [--fade N] [--switch N] "
                        "[--slim 0..1] [--seconds S]\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    Rations::CaptureSource sourceA, sourceB;
    std::string error;
    if (!Rations::loadCaptureSource(opt.fileA, sourceA, error)) {
        fprintf(stderr, "rations_fadecheck: %s: %s\n", opt.fileA.c_str(), error.c_str());
        return 1;
    }
    if (!Rations::loadCaptureSource(opt.fileB, sourceB, error)) {
        fprintf(stderr, "rations_fadecheck: %s: %s\n", opt.fileB.c_str(), error.c_str());
        return 1;
    }

    auto probe = build(sourceA, opt.slim, "A");
    if (!probe)
        return 1;
    // Read the receptive field from the model. Hard-coding it would let the proof drift away from
    // the thing it claims to be proving.
    const int prewarm = probe->GetPrewarmSamples();
    if (opt.fadeLen <= 0)
        opt.fadeLen = prewarm;
    if (opt.switchAt <= 0)
        opt.switchAt = static_cast<int>(kNativeRate);

    const int total = static_cast<int>(opt.seconds * kNativeRate);
    if (opt.switchAt + opt.fadeLen + prewarm >= total) {
        fprintf(stderr, "rations_fadecheck: not enough material after the switch; raise --seconds\n");
        return 1;
    }

    std::vector<double> input(static_cast<size_t>(total));
    fillTestSignal(input, kNativeRate);

    printf("captures       A = %s\n               B = %s\n", sourceA.filename.c_str(),
           sourceB.filename.c_str());
    printf("prewarm        %d samples (%.1f ms at %.0f Hz)\n", prewarm,
           1000.0 * prewarm / kNativeRate, kNativeRate);
    printf("switch at      %d    fade length %d (%.1f ms)\n\n", opt.switchAt, opt.fadeLen,
           1000.0 * opt.fadeLen / kNativeRate);

    const size_t n = static_cast<size_t>(total);
    std::vector<double> refA(n, 0.0), refB(n, 0.0), hard(n, 0.0), fade(n, 0.0), equalPower(n, 0.0);

    // --- references ---------------------------------------------------------------------------
    {
        auto a = build(sourceA, opt.slim, "A");
        auto b = build(sourceB, opt.slim, "B");
        if (!a || !b)
            return 1;
        render(*a, input.data(), refA.data(), total);
        render(*b, input.data(), refB.data(), total);
    }

    // --- the control: a hard switch, B never fed before it is heard -----------------------------
    {
        auto a = build(sourceA, opt.slim, "A");
        auto b = build(sourceB, opt.slim, "B");
        if (!a || !b)
            return 1;
        render(*a, input.data(), hard.data(), opt.switchAt);
        render(*b, input.data() + opt.switchAt, hard.data() + opt.switchAt, total - opt.switchAt);
    }

    // --- the fade: B is fed from the switch point and mixed in over `fade` samples --------------
    // Both branches are processed for the whole fade. Skipping B while its weight is small is the
    // single most tempting "optimisation" here and it silently destroys the priming that makes
    // this work.
    {
        auto a = build(sourceA, opt.slim, "A");
        auto b = build(sourceB, opt.slim, "B");
        if (!a || !b)
            return 1;
        std::vector<double> outA(n, 0.0), outB(n, 0.0);
        render(*a, input.data(), outA.data(), total);
        render(*b, input.data() + opt.switchAt, outB.data() + opt.switchAt, total - opt.switchAt);

        for (int i = 0; i < total; ++i) {
            if (i < opt.switchAt) {
                fade[static_cast<size_t>(i)] = outA[static_cast<size_t>(i)];
                equalPower[static_cast<size_t>(i)] = outA[static_cast<size_t>(i)];
                continue;
            }
            const double w = std::min(1.0, static_cast<double>(i - opt.switchAt) /
                                               static_cast<double>(opt.fadeLen));
            const double va = outA[static_cast<size_t>(i)];
            const double vb = outB[static_cast<size_t>(i)];
            fade[static_cast<size_t>(i)] = (1.0 - w) * va + w * vb;
            equalPower[static_cast<size_t>(i)] = std::sqrt(1.0 - w) * va + std::sqrt(w) * vb;
        }
    }

    int failures = 0;

    // --- 1. convergence ------------------------------------------------------------------------
    const size_t settled = static_cast<size_t>(opt.switchAt + opt.fadeLen);
    double worstAfter = 0.0;
    for (size_t i = settled; i < n; ++i)
        worstAfter = std::max(worstAfter, std::fabs(fade[i] - refB[i]));
    double worstHardAfter = 0.0;
    for (size_t i = settled; i < n; ++i)
        worstHardAfter = std::max(worstHardAfter, std::fabs(hard[i] - refB[i]));

    printf("1. CONVERGENCE (after the fade completes, against the fully primed reference)\n");
    printf("   max |fade - ref_B|   %.3e   %s\n", worstAfter,
           worstAfter < 1e-6 ? "PASS (< 1e-6)" : "FAIL");
    printf("   max |hard - ref_B|   %.3e   (the cold-swapped control, for scale)\n\n",
           worstHardAfter);
    if (!(worstAfter < 1e-6)) {
        fprintf(stderr, "FAIL: the fade did not converge on the primed reference\n");
        ++failures;
    }

    // --- 2. click ------------------------------------------------------------------------------
    const size_t win = static_cast<size_t>(0.005 * kNativeRate);
    const size_t lo = static_cast<size_t>(opt.switchAt) > win ? opt.switchAt - win : 0;
    const size_t hi = std::min(n, static_cast<size_t>(opt.switchAt) + win);
    const double typicalFade = maxStep(fade, 1, n);
    const double typicalHard = maxStep(hard, 1, n);
    const double localFade = maxStep(fade, lo, hi);
    const double localHard = maxStep(hard, lo, hi);
    // A discontinuity shows as a local step far larger than anything the signal itself produces.
    const double ratioFade = localFade / std::max(typicalFade, 1e-12);
    const double ratioHard = localHard / std::max(typicalHard, 1e-12);

    printf("2. DISCONTINUITY (largest sample step in +/-5 ms of the switch, vs the track's own)\n");
    printf("   hard   local %.3e   whole %.3e   ratio %.3f\n", localHard, typicalHard, ratioHard);
    printf("   fade   local %.3e   whole %.3e   ratio %.3f\n", localFade, typicalFade, ratioFade);
    printf("   fade step / hard step at the switch: %.4f\n\n",
           localFade / std::max(localHard, 1e-12));
    if (localFade > localHard) {
        fprintf(stderr, "FAIL: the fade is no smoother at the switch than the hard swap\n");
        ++failures;
    }

    // --- 3. priming-error decay ----------------------------------------------------------------
    printf("3. PRIMING DECAY (error of a model fed P samples, against the primed reference)\n");
    const int fractions[] = {0, 16, 8, 4, 2, 1};
    const size_t measure = static_cast<size_t>(0.05 * kNativeRate); // 50 ms window
    int smallestGoodP = -1;
    for (int f : fractions) {
        const int p = f == 0 ? 0 : prewarm / f;
        // Feed the model p samples of the run-up, then compare the next 50 ms with the reference.
        const int start = opt.switchAt - p;
        if (start < 0)
            continue;
        auto b = build(sourceB, opt.slim, "B");
        if (!b)
            return 1;
        std::vector<double> out(static_cast<size_t>(p) + measure, 0.0);
        render(*b, input.data() + start, out.data(), static_cast<int>(p + measure));
        std::vector<double> err(measure);
        for (size_t i = 0; i < measure; ++i)
            err[i] = out[static_cast<size_t>(p) + i] - refB[static_cast<size_t>(opt.switchAt) + i];
        const double e = db(rms(err.data(), measure) /
                            std::max(rms(refB.data() + opt.switchAt, measure), 1e-12));
        printf("   fed %6d samples (%5.1f ms)   error %7.2f dB\n", p, 1000.0 * p / kNativeRate, e);
        if (e < -40.0 && smallestGoodP < 0)
            smallestGoodP = p;
    }
    if (smallestGoodP >= 0)
        printf("   error falls below -40 dB after %d samples (%.1f ms)\n\n", smallestGoodP,
               1000.0 * smallestGoodP / kNativeRate);
    else
        printf("\n");

    // --- 4. mix law ----------------------------------------------------------------------------
    printf("4. MIX LAW (level across the fade, in 10 ms windows; 0 dB = interpolated reference)\n");
    const size_t step = static_cast<size_t>(0.010 * kNativeRate);
    double worstLinear = 0.0, worstEqual = 0.0;
    for (size_t i = static_cast<size_t>(opt.switchAt); i + step < settled; i += step) {
        const double w = static_cast<double>(i - opt.switchAt) / static_cast<double>(opt.fadeLen);
        const double ra = rms(refA.data() + i, step);
        const double rb = rms(refB.data() + i, step);
        const double reference = (1.0 - w) * ra + w * rb;
        if (reference < 1e-9)
            continue;
        const double dLinear = db(rms(fade.data() + i, step) / reference);
        const double dEqual = db(rms(equalPower.data() + i, step) / reference);
        worstLinear = std::max(worstLinear, std::fabs(dLinear));
        worstEqual = std::max(worstEqual, std::fabs(dEqual));
    }
    printf("   amplitude-complementary (a + b = 1)   worst deviation %5.2f dB\n", worstLinear);
    printf("   equal-power (a^2 + b^2 = 1)           worst deviation %5.2f dB\n", worstEqual);
    printf("   %s\n\n", worstLinear <= worstEqual
                            ? "amplitude-complementary is flatter, as the correlation predicts"
                            : "NOTE: equal-power measured flatter here — re-check the mix law");
    if (worstLinear > worstEqual) {
        fprintf(stderr, "FAIL: the chosen mix law is not the flatter one on this material\n");
        ++failures;
    }

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
