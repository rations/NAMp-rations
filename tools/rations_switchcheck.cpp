// rations_switchcheck — the offline proof that a channel change is exact, not merely smooth.
//
// The gain crossfade has its own proof (rations_fadecheck) and it rests on one claim: because
// these captures run on a strictly feed-forward network, a model fed live input for one receptive
// field is bit-identical to one that had been running since the session began. A CHANNEL change
// makes a harder demand of the same claim. A knob turn always has the incoming capture already
// running at zero weight before it is needed; a footswitch stomp does not. The incoming channel
// has been idle, has no convolution history at all, and has to be correct within a few tens of
// milliseconds of the stomp.
//
// The design's answer is to feed it its own past: the rack keeps a ring of native-rate input and,
// on a switch, replays the last R samples into the incoming channel faster than real time with
// the output discarded, then fades. If the claim above holds, the incoming channel is
// then EXACT, and the fade is between two true signals rather than a cosmetic mask over a wrong
// one. If the claim does not hold, this tool says so.
//
// It measures the shipped ChannelRack — the same object the plug-in hands to the resampler, driven
// in host-sized blocks — rather than a reimplementation of it, because a proof of a paraphrase is
// not a proof of the code.
//
//   refA        channel A running continuously from t = 0
//   refB        channel B running continuously from t = 0   <- the ground truth
//   switched    the rack: A, then a request for B at the stomp
//   restomped   the rack: A, a request for B, then a request for A again inside the switch window
//   held        the rack: A, a request for B, with A's own dial swept so the switch cannot be
//               taken until the dial settles
//   hard        A, then B's engine bound cold and let run    <- the control
//
// and asserts, rather than prints and hopes:
//
//   1. CONVERGENCE. Once the switch has completed, `switched` equals `refB` to within 1e-6. This
//      is the whole premise. Failing it means either the catch-up is shorter than the receptive
//      field or the architecture is not finite-memory.
//   2. NO CLICK. The largest sample-to-sample step across the switch, against the material's own
//      typical step, must not spike for `switched`. `hard` MUST spike, or the test material is too
//      tame to be evidence of anything.
//
//      Two things about that control had to be got right before it measured anything. It has to
//      be fed silence first, or the engine's own first-sound ramp turns the "hard" swap into a
//      15 ms fade and the control comes out gentler than it should. And the yardstick has to come
//      from BOTH channels, because a clean channel and a high-gain one are legitimately at very
//      different levels on the same quiet input, and the output really does travel between them.
//   3. RE-ENTRANCY. A second stomp inside the switch window is handled in place, never queued and
//      never by leaving a half-primed model bound: `restomped` must come back to `refA` exactly,
//      because the channel it returns to never stopped sounding.
//   5. THE HOLD. When the outgoing channel's own dial is moving there is no model budget left to
//      catch anyone up with, and the switch is held rather than taken. `held` must still arrive,
//      and must still be exact when it does — a hold that quietly dropped input history would
//      complete and be wrong, which is the failure this catches.
//   4. LATENCY. How long the switch actually took, measured off the output rather than predicted
//      from the constants — which is also the check that the model budget is buying what it claims.

#include "channelrack.h"
#include "crossfadeengine.h"
#include "engineconfig.h"
#include "modelbank.h"
// For the trim's range. Headers only - nothing here links the plug-in - but the fade sweep has to
// score the fade at the widest level difference the trims actually permit, and that number lives
// with the parameter rather than being restated here.
#include "rationsids.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr double kNativeRate = Rations::kNativeSampleRate;
// Matches the plug-in: per-capture loudness compensation on, no input calibration.
constexpr int kOutputModeNormalized = 1;
constexpr double kUnusedCalLevelDbu = 12.0;

// Enough silence to open the engine's first-sound ramp (engine::kBypassRampMs) several times
// over, while leaving every model history at the zeros Reset left it in.
constexpr int kGateOpenSamples = 4096;

struct Options {
    std::string dirA;
    std::string dirB;
    int block = 128; // host block size, in native-rate samples
    double seconds = 4.0;
    // Just past a pick attack, not in the tail of one. A stomp is something a player does while
    // playing, and it is also where the artefact is largest and the evidence therefore strongest:
    // in a decayed tail both channels are quiet and a hard swap has little to be wrong with.
    double stompAt = 1.52; // seconds
    double restompMs = 5.0;
    double posA = 0.0; // dial position of each channel, normalized
    double posB = 0.0;
    bool fadeSweep = false; // measure how long the channel fade has to be, rather than assume it
};

// A pick attack, a sustained chord and a decay into the noise floor. The decay matters most: an
// unprimed model hides under a loud sustain and is unmistakable under a dying note. A steady sine
// would prove nothing at all. Identical to the crossfade proof's signal, deliberately, so the two
// results are comparable.
void fillTestSignal(std::vector<double> &buf, double rate)
{
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double phase = std::fmod(t, 1.5);
        double env;
        if (phase < 0.0015)
            env = phase / 0.0015;
        else
            env = std::exp(-(phase - 0.0015) * 2.2);
        const double tone = 0.55 * std::sin(2.0 * M_PI * 82.41 * t) +  // E2
                            0.32 * std::sin(2.0 * M_PI * 123.47 * t) + // B2
                            0.22 * std::sin(2.0 * M_PI * 164.81 * t) + // E3
                            0.14 * std::sin(2.0 * M_PI * 246.94 * t) + // B3
                            0.09 * std::sin(2.0 * M_PI * 329.63 * t);  // E4
        buf[i] = 0.42 * env * tone;
    }
}

// One channel's loader and engine, standing alone. Used for the continuously-running references
// and for the hard-switch control, so those go through the same engine the rack drives rather
// than through a second copy of the arithmetic.
struct Channel {
    Rations::ModelBank loader;
    Rations::CrossfadeEngine engine;

    void open(const std::string &dir, int maxNative)
    {
        engine.setLoader(&loader);
        engine.prepare(maxNative, kNativeRate);
        engine.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);
        loader.start();
        loader.loadDirectory(dir, 1.0, Rations::engine::kChunk);
    }
    void close()
    {
        loader.stop();
        Rations::ModelBank::destroyBank(engine.releaseBank());
    }
};

bool waitForBank(Rations::ModelBank &loader, const char *what)
{
    for (int i = 0; i < 1200 && loader.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (loader.progress() < 1.0f) {
        fprintf(stderr, "rations_switchcheck: %s never finished building (%s)\n", what,
                loader.lastError().c_str());
        return false;
    }
    return true;
}

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
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

double maxDiff(const std::vector<double> &a, const std::vector<double> &b, size_t from, size_t to)
{
    double worst = 0.0;
    to = std::min({to, a.size(), b.size()});
    for (size_t i = from; i < to; ++i)
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    return worst;
}

// --- how long the channel fade actually has to be ---------------------------------------------
//
// engine::kChannelFadeMs was carried over from the plan as a round number, back when it sat behind
// a switch that took a third of a second and so cost nothing anyone could notice. Now that the
// catch-up finishes in about thirteen milliseconds the fade is a real share of the switch, and a
// number that is merely plausible is no longer good enough for it.
//
// The question is unusually clean, because by the time the fade runs both channels are exact
// responses to the same input — the catch-up guarantees that, and the convergence assertion above
// proves it. So the fade is not masking a discontinuity; it is there only because two different
// amps sit at two different levels, and stepping between two correct signals is still a step. That
// makes the length a question about the two continuously-running references and nothing else, and
// it can be asked of them directly, with the same arithmetic mixFade() runs: w advances by 1/L per
// sample, is used after it advances, and snaps to exactly 1.0. A fade of one sample is therefore
// the hard switch, which is why the table below is its own control.
//
// The metric is the one the no-click assertion already uses — the largest sample-to-sample step —
// but taken LOCALLY. Each candidate fade is scored against the material's own step in the window
// it lands in, and the worst score over several hundred landing points spread across the take is
// kept, in both directions. A single global yardstick would let a fade that lands in a dying note
// hide behind a pick attack somewhere else in the take, and a dying note is exactly where a short
// fade shows.
struct FadeScore {
    double ms = 0.0;
    double worstRatio = 0.0; // worst local (fade step / material step)
    double worstStep = 0.0;  // the step itself, at that point
    double atSeconds = 0.0;  // where it happened
};

FadeScore scoreFade(const std::vector<double> &a, const std::vector<double> &b, double ms,
                    size_t from, size_t to, size_t hop, double stepFloor, double gA, double gB)
{
    FadeScore r;
    r.ms = ms;
    const int len = std::max(1, static_cast<int>(ms * 0.001 * kNativeRate));
    const double step = 1.0 / static_cast<double>(len);
    const size_t guard = 32; // enough either side to catch the corner the ramp puts in

    for (size_t s = from; s + static_cast<size_t>(len) + guard < to; s += hop) {
        const size_t lo = s - guard;
        const size_t hi = s + static_cast<size_t>(len) + guard;

        // The material's own step, here rather than in general, and AFTER the trims: what the
        // fade has to hide under is the level the listener actually hears from each channel.
        // Below the floor nothing is playing and a ratio would be a statement about the noise
        // floor.
        const double local = std::max(gA * maxStep(a, lo, hi), gB * maxStep(b, lo, hi));
        if (local < stepFloor)
            continue;

        double prev = gA * a[lo];
        double worst = 0.0;
        double w = 0.0;
        for (size_t i = lo + 1; i < hi; ++i) {
            double y;
            if (i < s) {
                y = gA * a[i];
            } else if (w < 1.0) {
                w += step;
                if (w > 1.0)
                    w = 1.0;
                y = (1.0 - w) * gA * a[i] + w * gB * b[i];
            } else {
                y = gB * b[i];
            }
            worst = std::max(worst, std::fabs(y - prev));
            prev = y;
        }

        if (worst / local > r.worstRatio) {
            r.worstRatio = worst / local;
            r.worstStep = worst;
            r.atSeconds = static_cast<double>(s) / kNativeRate;
        }
    }
    return r;
}

void fadeSweep(const std::vector<double> &a, const std::vector<double> &b, int total)
{
    // Skip the first second: the engine's own first-sound ramp lives there and is not material.
    const size_t from = static_cast<size_t>(kNativeRate);
    const size_t to = static_cast<size_t>(total);
    if (to <= from + kNativeRate / 2) {
        printf("fade sweep     skipped, the run is too short — raise --seconds\n");
        return;
    }
    const size_t hop = static_cast<size_t>(kNativeRate / 400); // a landing point every 2.5 ms

    // A floor below which the material is not playing, taken from the take itself rather than
    // chosen: one thousandth of the largest step anywhere in it.
    const double loudest = std::max(maxStep(a, from, to), maxStep(b, from, to));
    const double floorStep = loudest * 0.001;

    static const double kCandidates[] = {0.0, 0.25, 0.5, 0.75, 1.0,  1.5, 2.0,
                                         3.0, 4.0,  5.0, 7.5,  10.0, 15.0};

    // The per-channel trims are part of this question, not a separate one. Two channels the player
    // has pushed to opposite ends of the trim range are further apart in level than any two
    // captures naturally are, and the step the fade has to cross is the difference between them.
    // So the sweep is run at the two worst trim settings the plug-in permits as well as at unity,
    // and the worst of the three is what each candidate is scored on.
    const double kTrimHigh = dbToGain(Rations::ranges::kLevelMax);
    const double kTrimLow = dbToGain(Rations::ranges::kLevelMin);
    struct TrimCase {
        double gA, gB;
        const char *what;
    };
    const TrimCase kTrims[] = {
        {1.0, 1.0, "0/0"},
        {kTrimHigh, kTrimLow, "+/-"},
        {kTrimLow, kTrimHigh, "-/+"},
    };

    printf("\n--- fade length, measured ---------------------------------------------------\n");
    printf("landing points %zu per direction, %.1f ms apart, over %.1f s of material\n",
           (to - from) / hop, 1000.0 * hop / kNativeRate, (to - from) / kNativeRate);
    printf("trims          unity, and both channels at opposite ends of the %+.0f/%+.0f dB range\n",
           Rations::ranges::kLevelMin, Rations::ranges::kLevelMax);
    printf("threshold      a fade may not step more than 1.50x the material's own step, which is\n"
           "               the same threshold the no-click assertion above is gated on\n\n");
    printf("   fade ms     worst step / material's own      verdict\n");

    double shortestClean = -1.0;
    for (double ms : kCandidates) {
        FadeScore worst;
        worst.ms = ms;
        const char *dir = "A->B";
        const char *trim = kTrims[0].what;
        for (const TrimCase &t : kTrims) {
            const FadeScore fwd = scoreFade(a, b, ms, from, to, hop, floorStep, t.gA, t.gB);
            const FadeScore rev = scoreFade(b, a, ms, from, to, hop, floorStep, t.gB, t.gA);
            const bool forward = fwd.worstRatio >= rev.worstRatio;
            const FadeScore &w = forward ? fwd : rev;
            if (w.worstRatio > worst.worstRatio) {
                worst = w;
                dir = forward ? "A->B" : "B->A";
                trim = t.what;
            }
        }
        const bool clean = worst.worstRatio <= 1.5;
        if (clean && shortestClean < 0.0)
            shortestClean = ms;
        printf("   %6.2f      x%-7.2f (%.4f, at %.2f s, %s, trim %s)   %s\n", ms, worst.worstRatio,
               worst.worstStep, worst.atSeconds, dir, trim, clean ? "clean" : "CLICKS");
    }

    if (shortestClean >= 0.0)
        printf("\n               shortest clean fade = %.2f ms; shipped kChannelFadeMs = %.2f ms\n",
               shortestClean, Rations::engine::kChannelFadeMs);
    else
        printf("\n               nothing in the sweep was clean\n");
}

bool parseArgs(int argc, char **argv, Options &o)
{
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "--block") {
            if (!(v = next()))
                return false;
            o.block = atoi(v);
        } else if (a == "--seconds") {
            if (!(v = next()))
                return false;
            o.seconds = atof(v);
        } else if (a == "--stomp") {
            if (!(v = next()))
                return false;
            o.stompAt = atof(v);
        } else if (a == "--restomp-ms") {
            if (!(v = next()))
                return false;
            o.restompMs = atof(v);
        } else if (a == "--pos-a") {
            if (!(v = next()))
                return false;
            o.posA = atof(v);
        } else if (a == "--fade-sweep") {
            o.fadeSweep = true;
        } else if (a == "--pos-b") {
            if (!(v = next()))
                return false;
            o.posB = atof(v);
        } else if (positional == 0) {
            o.dirA = a;
            ++positional;
        } else if (positional == 1) {
            o.dirB = a;
            ++positional;
        } else {
            fprintf(stderr, "rations_switchcheck: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (o.dirA.empty() || o.dirB.empty() || o.block <= 0) {
        fprintf(stderr,
                "usage: rations_switchcheck <captures/A> <captures/B> [--block N] [--seconds S]\n"
                "       [--stomp S] [--restomp-ms MS] [--pos-a 0..1] [--pos-b 0..1]\n"
                "       [--fade-sweep]\n");
        return false;
    }
    return true;
}

// Run a bare engine over the input in host blocks, from `from` to `to`.
void renderEngine(Rations::CrossfadeEngine &e, double pos, const std::vector<double> &in,
                  std::vector<double> &out, int from, int to, int block)
{
    e.pollBank();
    e.setPositionNorm(pos);
    for (int off = from; off < to; off += block) {
        const int n = std::min(block, to - off);
        NAM_SAMPLE *ip = const_cast<NAM_SAMPLE *>(in.data()) + off;
        NAM_SAMPLE *op = out.data() + off;
        e.processNative(&ip, &op, n);
    }
}

// The two channel slots the rack is given. Any two distinct channels would do; these are named so
// the printout reads like the panel.
constexpr Rations::Channel kSlotA = Rations::kChannelClean;
constexpr Rations::Channel kSlotB = Rations::kChannelOd1;

// Drive the rack over the whole input in host blocks, requesting `kSlotB` at `stompAt` and, if
// `restompAt` is inside the run, `kSlotA` again at that point.
void renderRack(Rations::ChannelRack &rack, const Options &o, const std::vector<double> &in,
                std::vector<double> &out, int total, int stompAt, int restompAt, int sweepUntil = 0)
{
    for (int off = 0; off < total; off += o.block) {
        const int n = std::min(o.block, total - off);

        rack.pollBanks();
        if (off < sweepUntil) {
            // Channel A's own gain dial, moving. That holds TWO of its captures bound, which is
            // the state in which the rack has no model budget left to catch anyone up with and
            // must hold the switch rather than take it.
            const double phase = static_cast<double>(off % 48000) / 48000.0;
            rack.setPositionNorm(kSlotA, phase);
        } else {
            rack.setPositionNorm(kSlotA, o.posA);
        }
        rack.setPositionNorm(kSlotB, o.posB);
        // The request is evaluated once per block, exactly as the processor evaluates the
        // parameter once per block. A stomp lands on a block boundary, which is where a host
        // parameter change lands too.
        if (restompAt >= 0 && off >= restompAt)
            rack.requestChannel(kSlotA);
        else if (off >= stompAt)
            rack.requestChannel(kSlotB);
        else
            rack.requestChannel(kSlotA);

        NAM_SAMPLE *ip = const_cast<NAM_SAMPLE *>(in.data()) + off;
        NAM_SAMPLE *op = out.data() + off;
        rack.processNative(&ip, &op, n);
    }
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    const int total = static_cast<int>(opt.seconds * kNativeRate);
    const int stompAt = static_cast<int>(opt.stompAt * kNativeRate);
    const int restompAt = stompAt + static_cast<int>(opt.restompMs * 0.001 * kNativeRate);
    if (stompAt <= 0 || stompAt >= total) {
        fprintf(stderr, "rations_switchcheck: --stomp is outside the run; raise --seconds\n");
        return 1;
    }

    std::vector<double> input(static_cast<size_t>(total));
    fillTestSignal(input, kNativeRate);

    // --- the continuously-running references ---------------------------------------------------
    Channel refA, refB;
    refA.open(opt.dirA, opt.block);
    refB.open(opt.dirB, opt.block);
    if (!waitForBank(refA.loader, "channel A") || !waitForBank(refB.loader, "channel B")) {
        refA.close();
        refB.close();
        return 1;
    }

    const size_t n = static_cast<size_t>(total);
    std::vector<double> outRefA(n, 0.0), outRefB(n, 0.0), outHard(n, 0.0);
    renderEngine(refA.engine, opt.posA, input, outRefA, 0, total, opt.block);
    renderEngine(refB.engine, opt.posB, input, outRefB, 0, total, opt.block);

    const int prewarm = refB.engine.restPrewarmSamples();
    printf("channels       A = %s\n               B = %s\n", opt.dirA.c_str(), opt.dirB.c_str());
    printf("bank sizes     A = %d captures, B = %d captures\n", refA.engine.entryCount(),
           refB.engine.entryCount());
    printf("prewarm        %d samples (%.1f ms at %.0f Hz), read from the incoming capture\n",
           prewarm, 1000.0 * prewarm / kNativeRate, kNativeRate);
    // At rest the sounding channel costs one model, so the catch-up gets budget - 1 and closes
    // the gap at (budget - 2) samples per sample.
    const double restRate = Rations::engine::kSwitchModelBudget - 1.0;
    printf("model budget   %.2f  -> catch-up at %.2fx real time, predicted %.0f ms, plus a %.1f ms "
           "fade\n",
           Rations::engine::kSwitchModelBudget, restRate,
           1000.0 * prewarm / ((restRate - 1.0) * kNativeRate), Rations::engine::kChannelFadeMs);
    printf("host block     %d samples    stomp at %d (%.3f s)\n\n", opt.block, stompAt,
           opt.stompAt);

    // Opt-in, because it is a measurement rather than an assertion: it is what SET
    // engine::kChannelFadeMs, and it is kept so that the number can be re-derived rather than
    // taken on trust.
    if (opt.fadeSweep)
        fadeSweep(outRefA, outRefB, total);

    // --- the control: a hard model swap, B never fed before it is heard -------------------------
    // A fresh loader and engine, so B's models have no convolution history whatever at the moment
    // they are asked for output. This is what a channel switch is if the catch-up is left out.
    //
    // It has to be fed SILENCE first, and that is not a detail. The engine opens a short ramp the
    // first time a channel produces anything, so a control that simply started rendering at the
    // stomp would be a hard swap wearing a 15 ms fade-in — it would understate the artefact, and
    // the control would be quietly softer than the thing it is the control for. Feeding zeros
    // opens that ramp while leaving every convolution history exactly as Reset left it, which is
    // the state a genuinely idle channel is in.
    {
        Channel coldB;
        coldB.open(opt.dirB, opt.block);
        if (!waitForBank(coldB.loader, "the control's channel B")) {
            coldB.close();
            refA.close();
            refB.close();
            return 1;
        }
        std::vector<double> silence(static_cast<size_t>(kGateOpenSamples), 0.0);
        std::vector<double> discard(static_cast<size_t>(kGateOpenSamples), 0.0);
        renderEngine(coldB.engine, opt.posB, silence, discard, 0, kGateOpenSamples, opt.block);

        std::copy(outRefA.begin(), outRefA.begin() + stompAt, outHard.begin());
        renderEngine(coldB.engine, opt.posB, input, outHard, stompAt, total, opt.block);
        coldB.close();
    }

    // --- the rack: one stomp -------------------------------------------------------------------
    std::vector<double> outSwitched(n, 0.0), outRestomped(n, 0.0);
    {
        Rations::ChannelRack rack;
        rack.prepare(opt.block, kNativeRate);
        rack.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);
        rack.start();
        rack.loadChannel(kSlotA, opt.dirA, 1.0, Rations::engine::kChunk);
        rack.loadChannel(kSlotB, opt.dirB, 1.0, Rations::engine::kChunk);
        for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        renderRack(rack, opt, input, outSwitched, total, stompAt, -1);
        rack.stop();
        rack.releaseBanks();
    }

    // --- the rack: a stomp while the outgoing channel's own dial is moving ----------------------
    // With two of channel A's captures bound there is no model budget left, so the catch-up is
    // held: parked one receptive field behind the write head, losing nothing, waiting. When the
    // dial stops and the auto-detent collapses A back to a single capture the budget reappears and
    // the catch-up resumes. What must survive all that is the convergence — a hold that quietly
    // dropped input history would still complete, and would still be wrong.
    std::vector<double> outHeld(n, 0.0);
    {
        Rations::ChannelRack rack;
        rack.prepare(opt.block, kNativeRate);
        rack.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);
        rack.start();
        rack.loadChannel(kSlotA, opt.dirA, 1.0, Rations::engine::kChunk);
        rack.loadChannel(kSlotB, opt.dirB, 1.0, Rations::engine::kChunk);
        for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        renderRack(rack, opt, input, outHeld, total, stompAt, -1,
                   stompAt + static_cast<int>(0.3 * kNativeRate));
        rack.stop();
        rack.releaseBanks();
    }

    // --- the rack: a second stomp inside the switch window --------------------------------------
    {
        Rations::ChannelRack rack;
        rack.prepare(opt.block, kNativeRate);
        rack.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu);
        rack.start();
        rack.loadChannel(kSlotA, opt.dirA, 1.0, Rations::engine::kChunk);
        rack.loadChannel(kSlotB, opt.dirB, 1.0, Rations::engine::kChunk);
        for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        renderRack(rack, opt, input, outRestomped, total, stompAt, restompAt);
        rack.stop();
        rack.releaseBanks();
    }

    refA.close();
    refB.close();

    // --- 4. LATENCY, measured off the output rather than predicted from the constants -----------
    // Where the switched track leaves A, and where it arrives at B. Both are observations: if the
    // catch-up ratio or the fade length ever drifts away from what the constants say, this is what
    // notices.
    const double kEps = 1e-6;
    int leftA = -1, reachedB = -1;
    for (int i = stompAt; i < total; ++i) {
        if (leftA < 0 &&
            std::fabs(outSwitched[static_cast<size_t>(i)] - outRefA[static_cast<size_t>(i)]) > kEps)
            leftA = i;
    }
    for (int i = total - 1; i >= stompAt; --i) {
        if (std::fabs(outSwitched[static_cast<size_t>(i)] - outRefB[static_cast<size_t>(i)]) >
            kEps) {
            reachedB = i + 1;
            break;
        }
    }
    if (leftA < 0 || reachedB < 0 || reachedB >= total) {
        fprintf(stderr,
                "FAIL: the switch never completed — the output never left channel A (%d) or never "
                "arrived at channel B (%d) inside %d samples\n",
                leftA, reachedB, total);
        return 1;
    }

    printf("switch         left A at +%.1f ms, reached B at +%.1f ms  (fade itself %.1f ms)\n",
           1000.0 * (leftA - stompAt) / kNativeRate, 1000.0 * (reachedB - stompAt) / kNativeRate,
           1000.0 * (reachedB - leftA) / kNativeRate);

    int failures = 0;

    // --- 1. CONVERGENCE -------------------------------------------------------------------------
    const double convergence = maxDiff(outSwitched, outRefB, static_cast<size_t>(reachedB), n);
    printf("\nconvergence    max |switched - refB| after the switch = %.3e  (limit 1e-6)\n",
           convergence);
    if (!(convergence <= 1e-6)) {
        fprintf(stderr, "FAIL: the switched channel did not converge on the continuously-running "
                        "reference. Either the catch-up fed less than one receptive field, or the "
                        "architecture is not finite-memory.\n");
        ++failures;
    }
    // And it must be a real convergence, not a track that was never different: the hard control
    // has to disagree with the reference at the same point, or the two captures are too alike for
    // any of this to be evidence.
    const double controlGap = maxDiff(outHard, outRefB, static_cast<size_t>(stompAt),
                                      static_cast<size_t>(stompAt + prewarm));
    printf("               max |hard - refB| over the first receptive field = %.3e\n", controlGap);
    if (!(controlGap > 1e-4)) {
        fprintf(stderr, "FAIL: a COLD channel B already matches the primed one, so convergence "
                        "proves nothing. The captures are too similar, or nothing is being fed.\n");
        ++failures;
    }

    // --- 2. NO CLICK ----------------------------------------------------------------------------
    const size_t window = static_cast<size_t>(0.005 * kNativeRate);
    const size_t from = static_cast<size_t>(std::max(0, leftA - static_cast<int>(window)));
    const size_t to = static_cast<size_t>(std::min<long long>(
        total, static_cast<long long>(reachedB) + static_cast<long long>(window)));
    // The track's own typical step, taken well away from the switch, is the yardstick. An absolute
    // threshold would only be a statement about this test signal's loudness.
    // Taken from BOTH references over the 100 ms leading up to the stomp, and the larger kept.
    // Which one matters: the output legitimately travels from one channel to the other, and these
    // are different amps — at a quiet moment a high-gain channel is many times louder than a clean
    // one on the same input, because that is what a high-gain channel does. Measuring against the
    // quieter of the two would score an entirely correct switch as a click, and measuring at a
    // louder moment elsewhere in the take would score a real one as clean.
    const size_t yardstickFrom = static_cast<size_t>(stompAt) - window * 20;
    const double typical = std::max(maxStep(outRefA, yardstickFrom, static_cast<size_t>(stompAt)),
                                    maxStep(outRefB, yardstickFrom, static_cast<size_t>(stompAt)));
    const double stepSwitched = maxStep(outSwitched, from, to);
    const double stepHard = maxStep(outHard, static_cast<size_t>(stompAt) - window,
                                    static_cast<size_t>(stompAt) + window);
    printf("\nstep size      typical %.4f | switched %.4f (x%.2f) | hard %.4f (x%.2f)\n", typical,
           stepSwitched, stepSwitched / typical, stepHard, stepHard / typical);
    if (!(stepSwitched <= typical * 1.5)) {
        fprintf(stderr,
                "FAIL: the switch produced a step %.2fx the track's own — that is the "
                "click this design exists to prevent.\n",
                stepSwitched / typical);
        ++failures;
    }
    if (!(stepHard > typical * 1.5)) {
        fprintf(stderr, "FAIL: the HARD control did not click, so this material cannot show the "
                        "difference and the result above is not evidence.\n");
        ++failures;
    }

    // --- 3. RE-ENTRANCY -------------------------------------------------------------------------
    // A stomp back to the channel that is still sounding costs nothing and must change nothing:
    // that channel never stopped, so the whole track has to equal refA, including inside the
    // window where a switch was briefly in flight.
    const double restompGap = maxDiff(outRestomped, outRefA, 0, n);
    printf("\nre-entrancy    second stomp at +%.1f ms; max |restomped - refA| over the whole run = "
           "%.3e\n",
           opt.restompMs, restompGap);
    if (!(restompGap <= 1e-6)) {
        fprintf(stderr, "FAIL: stomping back to the channel that never stopped sounding disturbed "
                        "it. A switch requested during a switch must be resolved in place, not by "
                        "leaving a half-primed model bound.\n");
        ++failures;
    }

    // --- 5. THE HOLD ----------------------------------------------------------------------------
    // The switch must still arrive, and must still be exact, after having been held for want of
    // CPU. Measured off the output the same way the unheld one is.
    int heldReached = -1;
    for (int i = total - 1; i >= stompAt; --i) {
        if (std::fabs(outHeld[static_cast<size_t>(i)] - outRefB[static_cast<size_t>(i)]) > kEps) {
            heldReached = i + 1;
            break;
        }
    }
    printf("\nheld switch    dial swept for 300 ms after the stomp; ");
    if (heldReached < 0 || heldReached >= total) {
        printf("never arrived\n");
        fprintf(stderr, "FAIL: a switch held for want of model budget never completed. The hold "
                        "must park the catch-up, not abandon it.\n");
        ++failures;
    } else {
        const double heldGap = maxDiff(outHeld, outRefB, static_cast<size_t>(heldReached), n);
        printf("arrived at +%.0f ms, max |held - refB| after = %.3e\n",
               1000.0 * (heldReached - stompAt) / kNativeRate, heldGap);
        if (!(heldGap <= 1e-6)) {
            fprintf(stderr, "FAIL: a switch that was held did not converge. Parking the catch-up "
                            "lost input history that the priming needed.\n");
            ++failures;
        }
    }

    if (failures > 0) {
        printf("\nFAILED - %d assertion(s)\n", failures);
        return 1;
    }
    printf("\nPASSED - the channel switch converges on a continuously-running reference, does not "
           "click where a hard swap does, survives a stomp inside its own window, and still "
           "arrives exact after being held for want of CPU\n");
    return 0;
}
