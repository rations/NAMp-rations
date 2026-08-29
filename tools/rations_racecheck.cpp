// rations_racecheck — the ThreadSanitizer gate for the prime worker.
//
// Background priming introduced the first genuine concurrent access to the DSP objects in this
// project: an engine and its models are touched by the audio thread when it owns them and by the
// prime worker when it does not, and the whole safety argument is a three-state atomic and the
// claim it publishes. That argument is exactly the kind that reads as obviously correct and is
// not, so RULES.md requires it to be proved by a sanitizer rather than by reading — and it earned
// that requirement immediately: the first version of the ownership handshake looked right, passed
// review, and could not complete a switch at all, because the audio thread handed the engine back
// at the end of the very block that claimed it.
//
// What this does is hammer the handover from both sides at once:
//
//   * stomps between channels at a period that keeps changing, so the claim lands at every
//     possible phase against the worker's tick rather than at one convenient one;
//   * re-stomps INSIDE the switch window, which is the D6 case where an in-flight catch-up is
//     abandoned and a different engine claimed in the same block;
//   * sweeps all four dials continuously, so idle channels rebind entries under the worker while
//     the audio thread is publishing new positions for them;
//   * optionally republishes a bank mid-run, which invalidates every model pointer in a channel
//     underneath whichever thread happens to own it.
//
// It asserts nothing about the audio. Its output is a race report or the absence of one, plus the
// worst lag the prime worker ever had to close — which is the measurement that decides whether
// engine::kInputRingSamples is large enough, since a worker lapped by the ring loses its history
// and the channel it was keeping warm goes cold.
//
// Build with -DRATIONS_ENABLE_TSAN=ON and run under a TSan-instrumented build; without it this is
// still a useful stress test, it just cannot see a race.
//
// Usage:
//   rations_racecheck <dirA> <dirB> [dirC] [dirD] [--seconds 2] [--block 128] [--republish]

#include "channelrack.h"
#include "engineconfig.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Rations;

namespace
{

constexpr double kNativeRate = kNativeSampleRate;
constexpr int kOutputModeNormalized = 1;
constexpr double kUnusedCalLevelDbu = 12.0;

struct Options {
    std::vector<std::string> dirs;
    double seconds = 2.0;
    int block = 128;
    bool republish = false;
    bool proveHarness = false;
};

// A deliberate race, so that a clean run means something. A sanitizer can be silently absent -
// a build directory configured without RATIONS_ENABLE_TSAN produces a binary that runs fine and
// reports nothing - and "no races found" from an uninstrumented binary looks exactly like "no
// races". This is the ThreadSanitizer equivalent of the allocation harness proving it can see a
// deliberate allocation before any of its counts are believed.
void proveHarness()
{
    int shared = 0;
    std::thread other([&shared] {
        for (int i = 0; i < 1000000; ++i)
            shared += 1;
    });
    for (int i = 0; i < 1000000; ++i)
        shared -= 1;
    other.join();
    printf("harness        deliberate race over a plain int ran to completion (%d)\n", shared);
    printf("               ThreadSanitizer MUST have reported it above. If it did not, this "
           "build is not instrumented and a clean run below proves nothing.\n\n");
}

bool parseArgs(int argc, char **argv, Options &opt)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) {
            opt.seconds = atof(argv[++i]);
        } else if (a == "--block" && i + 1 < argc) {
            opt.block = atoi(argv[++i]);
        } else if (a == "--republish") {
            opt.republish = true;
        } else if (a == "--prove-harness") {
            opt.proveHarness = true;
        } else if (!a.empty() && a[0] != '-') {
            opt.dirs.push_back(a);
        } else {
            fprintf(stderr, "rations_racecheck: unexpected argument '%s'\n", a.c_str());
            return false;
        }
    }
    if (opt.dirs.size() < 2) {
        fprintf(stderr, "usage: rations_racecheck <dirA> <dirB> [dirC] [dirD] [--seconds S] "
                        "[--block N] [--republish] [--prove-harness]\n"
                        "  at least two channels, because a switch needs somewhere to go\n");
        return false;
    }
    if (opt.dirs.size() > static_cast<size_t>(kChannelCount))
        opt.dirs.resize(static_cast<size_t>(kChannelCount));
    if (opt.block < 16 || opt.block > 4096 || opt.seconds <= 0.0)
        return false;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    if (opt.proveHarness)
        proveHarness();

    const int channels = static_cast<int>(opt.dirs.size());
    const int total = static_cast<int>(opt.seconds * kNativeRate);

    // A signal with content, not silence: an idle channel's models must actually be doing work for
    // a race inside them to be reachable at all.
    std::vector<NAM_SAMPLE> input(static_cast<size_t>(total + opt.block), 0.0);
    for (int i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / kNativeRate;
        input[static_cast<size_t>(i)] = 0.25 * std::sin(6.283185307179586 * 110.0 * t) +
                                        0.10 * std::sin(6.283185307179586 * 349.0 * t + 0.7);
    }
    std::vector<NAM_SAMPLE> output(static_cast<size_t>(total + opt.block), 0.0);

    ChannelRack rack;
    rack.prepare(opt.block, kNativeRate);
    rack.setOutputMode(kOutputModeNormalized, kUnusedCalLevelDbu,
                       /*calibrateInput=*/false);
    rack.start();
    for (int c = 0; c < channels; ++c)
        rack.loadChannel(static_cast<Channel>(c), opt.dirs[static_cast<size_t>(c)],
                         /*isDirectory=*/true, 1.0, engine::kChunk);

    printf("racecheck      %d channel(s), %.1f s at %d frames%s\n", channels, opt.seconds,
           opt.block, opt.republish ? ", with a bank republish mid-run" : "");
    for (int i = 0; i < 2400 && rack.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    printf("banks          %.0f%% built\n", 100.0 * static_cast<double>(rack.progress()));

    // The stomp period keeps changing so the claim lands at every phase against the worker's
    // tick. A fixed period would test one alignment very thoroughly and the rest not at all.
    static const int kPeriods[] = {1, 2, 3, 5, 8, 13, 2, 1, 21, 3};
    int periodIndex = 0;
    int blocksToStomp = kPeriods[0];
    int want = 0;
    int stomps = 0;
    int outputGen = 0;
    const int republishAt = opt.republish ? total / 2 : -1;
    bool republished = false;

    for (int off = 0; off < total; off += opt.block) {
        const int n = std::min(opt.block, total - off);

        rack.pollBanks();

        // Every dial moving, every block. An idle channel whose dial crosses an integer rebinds
        // its entry under the worker while the audio thread is publishing the new position.
        for (int c = 0; c < channels; ++c) {
            const double phase = std::fmod(static_cast<double>(off) / 24000.0 + 0.17 * c, 1.0);
            rack.setPositionNorm(static_cast<Channel>(c), phase);
        }

        if (--blocksToStomp <= 0) {
            want = (want + 1) % channels;
            rack.requestChannel(static_cast<Channel>(want));
            ++stomps;
            periodIndex = (periodIndex + 1) % static_cast<int>(sizeof(kPeriods) / sizeof(int));
            blocksToStomp = kPeriods[periodIndex];

            // The output section, republished from the audio thread on every stomp. This is the
            // cross-thread write the mode publication added, and it is the reason this loop cares
            // about it: setOutputMode is called from RT (a radio click on the settings page arrives
            // as a host parameter change), while three of the four engines it concerns belong to
            // the prime worker, which reads the publication on its own tick. Nothing here would
            // exercise that if the mode were set once before the run.
            //
            // Cycling all three modes and both calibration states, so the input gain changes as
            // well as the output compensation - a change to the input is the one that also has to
            // make a channel report itself NOT warm, and a stale warm flag is a correctness bug
            // rather than a slow one.
            ++outputGen;
            rack.setOutputMode(outputGen % 3, 12.0 + (outputGen % 5), (outputGen % 2) != 0);
        }

        NAM_SAMPLE *ip = input.data() + off;
        NAM_SAMPLE *op = output.data() + off;
        rack.processNative(&ip, &op, n);

        // A republish from the message thread, underneath whichever thread owns that channel.
        // In the shipping plug-in each directory is loaded once, so this cannot happen there -
        // but "cannot" is a property of a call site somewhere else, and the failure it would
        // cause is a silently unprimed model.
        if (republishAt >= 0 && !republished && off >= republishAt) {
            rack.loadChannel(static_cast<Channel>(channels - 1), opt.dirs[opt.dirs.size() - 1],
                             /*isDirectory=*/true, 1.0, engine::kChunk);
            republished = true;
        }
    }

    // Stop stomping and let it settle. Phase A does not ACT on warmth - the audio thread still
    // primes from a whole receptive field - but whether the worker actually achieves it is the
    // question the next phase rests on entirely, so it is reported here rather than assumed from
    // the fact that the worker is running.
    const int settleBlocks = static_cast<int>(0.5 * kNativeRate) / opt.block;
    for (int b = 0; b < settleBlocks; ++b) {
        rack.pollBanks();
        for (int c = 0; c < channels; ++c)
            rack.setPositionNorm(static_cast<Channel>(c), 0.5);
        rack.requestChannel(static_cast<Channel>(0));
        NAM_SAMPLE *ip = input.data();
        NAM_SAMPLE *op = output.data();
        rack.processNative(&ip, &op, opt.block);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    printf("warm           ");
    for (int c = 0; c < channels; ++c)
        printf("ch%d=%s ", c, rack.warm(static_cast<Channel>(c)) ? "yes" : "no ");
    printf("\n               (channel 0 is the sounding one, so the audio thread owns it and the "
           "worker cannot keep it warm - that is correct, not a miss)\n");

    rack.stop();
    rack.releaseBanks();

    // The number that sizes the ring. The worker must never fall further behind than
    // kInputRingSamples minus one receptive field, or its history is overwritten and the channel
    // it was keeping warm goes cold until it can prime again.
    const long long worst = rack.worstPrimeLag();
    const long long budget =
        static_cast<long long>(engine::kInputRingSamples) - 6347; // A2 fast path at 48 kHz
    printf("stomps         %d\n", stomps);
    printf("worst lag      %lld samples (%.1f ms) of %lld before the worker is lapped (%.0f%%)\n",
           worst, 1000.0 * static_cast<double>(worst) / kNativeRate, budget,
           100.0 * static_cast<double>(worst) / static_cast<double>(budget));
    printf("\nrations_racecheck: completed - a clean run under ThreadSanitizer is the gate\n");
    return 0;
}
