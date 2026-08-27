// rations_rtcheck — proves the audio path allocates nothing.
//
// "No allocation on the real-time thread" is a claim that inspection cannot settle: a std::function
// that captures one byte too many, a vector that grows on its first call, a model handed a block
// larger than the size it was Reset with — none of those are visible by reading. So this counts
// them, using the DSP core's own malloc/free interception harness.
//
// Four things are measured, and the last two are the ones most likely to be wrong:
//
//   1. The crossfade engine, sweeping the knob across a whole bank so branches bind, swap and
//      collapse while the count is running.
//   2. The engine through the resampler at 48 kHz, where the resampler is a straight call-through.
//   3. The engine through the resampler at 44.1 kHz, where it is not. ResamplingContainer takes its
//      block-processing callable as a std::function BY VALUE, and its own header warns that
//      captures can malloc. The plug-in relies on a one-reference capture fitting in libstdc++'s
//      small-object buffer. That is an assumption about a standard library implementation detail,
//      so it gets measured rather than asserted.
//   4. The channel rack, switching channels back and forth while the count runs. The catch-up
//      burst is still the audio thread: it feeds the incoming channel out of the input ring inside
//      the same process() call, so every rule that applies to the steady state applies to it. This
//      is also where a ring read, a fade buffer or a chunk stage that was sized wrong would show
//      up, because those only run during a switch.

#include "channelrack.h"
#include "crossfadeengine.h"
#include "engineconfig.h"
#include "modelbank.h"
#include "nativeresampler.h"

#include "test/allocation_tracking.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int kBlock = 512;

// Drives the engine directly, bypassing the resampler.
struct DirectDriver {
    Rations::CrossfadeEngine &engine;
    std::vector<NAM_SAMPLE> &in;
    std::vector<NAM_SAMPLE> &out;

    void run(int blocks)
    {
        NAM_SAMPLE *ip = in.data();
        NAM_SAMPLE *op = out.data();
        for (int b = 0; b < blocks; ++b) {
            engine.setPositionNorm(static_cast<double>(b) / static_cast<double>(blocks));
            engine.processNative(&ip, &op, kBlock);
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rations_rtcheck <capture directory A> <capture directory B>\n"
                        "  two directories, because a channel switch needs somewhere to switch to "
                        "and the catch-up burst is part of the audio path\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string dirB = argv[2];

    Rations::ModelBank loader;
    loader.start();
    Rations::CrossfadeEngine engine;
    engine.setLoader(&loader);

    Rations::NativeResampler resampler48, resampler44;
    resampler48.configure(48000.0, kBlock);
    resampler44.configure(44100.0, kBlock);
    const int maxNative =
        std::max(resampler48.maxNativeBlock(kBlock), resampler44.maxNativeBlock(kBlock));
    engine.prepare(maxNative, Rations::kNativeSampleRate);

    loader.loadDirectory(dir, 1.0, Rations::engine::kChunk);

    // Wait for the whole bank, so no model is built while the count is running.
    for (int i = 0; i < 600 && loader.progress() < 1.0f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.pollBank();
    if (engine.entryCount() == 0) {
        fprintf(stderr, "rations_rtcheck: no captures loaded from %s (%s)\n", dir.c_str(),
                loader.lastError().c_str());
        return 1;
    }
    printf("bank           %d captures, progress %.0f%%\n", engine.entryCount(),
           100.0 * loader.progress());
    printf("resampler      48000 Hz engaged=%d latency=%d | 44100 Hz engaged=%d latency=%d\n\n",
           resampler48.engaged() ? 1 : 0, resampler48.latency(), resampler44.engaged() ? 1 : 0,
           resampler44.latency());

    std::vector<NAM_SAMPLE> in(static_cast<size_t>(maxNative), 0.0);
    std::vector<NAM_SAMPLE> out(static_cast<size_t>(maxNative), 0.0);
    for (int i = 0; i < maxNative; ++i)
        in[static_cast<size_t>(i)] = 0.25 * std::sin(2.0 * M_PI * 110.0 * i / 48000.0);

    // Warm every lazily-sized buffer BEFORE counting, exactly as setActive() does in the plug-in.
    // Counting the first-call growth would measure the warm-up, not the steady state.
    DirectDriver direct{engine, in, out};
    direct.run(8);
    {
        NAM_SAMPLE *ip = in.data();
        NAM_SAMPLE *op = out.data();
        for (int b = 0; b < 8; ++b) {
            resampler48.process(&ip, &op, kBlock, engine);
            resampler44.process(&ip, &op, kBlock, engine);
        }
    }

    // First, prove the harness has teeth. A checker that cannot fail is not evidence of anything,
    // and operator-new interception is exactly the kind of thing that can be silently defeated by
    // a link order or an inlining decision.
    {
        allocation_tracking::g_allocation_count = 0;
        allocation_tracking::g_deallocation_count = 0;
        allocation_tracking::g_tracking_enabled = true;
        volatile auto *leak = new double[64];
        delete[] leak;
        allocation_tracking::g_tracking_enabled = false;
        if (allocation_tracking::g_allocation_count == 0) {
            fprintf(stderr, "FAIL: the allocation harness did not see a deliberate allocation, so "
                            "the results below would be meaningless\n");
            return 1;
        }
        printf("harness        sees a deliberate allocation (%d), so the counts below mean "
               "something\n\n",
               allocation_tracking::g_allocation_count);
    }

    printf("counting       crossfade engine, knob sweeping\n");
    // Sweeping the position is the point: it binds branches, swaps them at integer crossings and
    // collapses them at rest. A static position would exercise almost none of the engine.
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr, [&] { direct.run(400); }, nullptr, "crossfade engine, knob sweeping");

    printf("counting       engine through the resampler at 48 kHz (bypassed)\n");
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr,
        [&] {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < 200; ++b) {
                engine.setPositionNorm(static_cast<double>(b) / 200.0);
                resampler48.process(&ip, &op, kBlock, engine);
            }
        },
        nullptr, "engine through the resampler at 48 kHz (bypassed)");

    printf("counting       engine through the resampler at 44.1 kHz (engaged)\n");
    allocation_tracking::run_allocation_test_no_allocations(
        nullptr,
        [&] {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < 200; ++b) {
                engine.setPositionNorm(static_cast<double>(b) / 200.0);
                resampler44.process(&ip, &op, kBlock, engine);
            }
        },
        nullptr, "engine through the resampler at 44.1 kHz (engaged, std::function by value)");

    // --- the channel rack, switching -----------------------------------------------------------
    // Two channels loaded and a stomp every few blocks, so the count spans the whole switch: the
    // ring writes, the catch-up feed at kCatchupRatio, the fade, and a stomp that lands inside a
    // switch already in flight.
    {
        Rations::ChannelRack rack;
        rack.prepare(kBlock, Rations::kNativeSampleRate);
        rack.start();
        rack.loadChannel(Rations::kChannelClean, dir, 1.0, Rations::engine::kChunk);
        rack.loadChannel(Rations::kChannelOd1, dirB, 1.0, Rations::engine::kChunk);
        for (int i = 0; i < 1200 && rack.progress() < 1.0f; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (rack.progress() < 1.0f) {
            fprintf(stderr, "rations_rtcheck: the rack's banks never finished building\n");
            return 1;
        }

        // The stomp period is deliberately shorter than a whole switch takes, so some requests
        // land mid-catch-up and some mid-fade. Both are the paths that only exist here.
        auto drive = [&](int blocks) {
            NAM_SAMPLE *ip = in.data();
            NAM_SAMPLE *op = out.data();
            for (int b = 0; b < blocks; ++b) {
                const Rations::Channel want =
                    ((b / 3) % 2) ? Rations::kChannelOd1 : Rations::kChannelClean;
                rack.pollBanks();
                rack.setPositionNorm(Rations::kChannelClean, 0.0);
                rack.setPositionNorm(Rations::kChannelOd1, 0.0);
                rack.requestChannel(want);
                rack.processNative(&ip, &op, kBlock);
            }
        };
        drive(8); // warm anything lazily sized before the count starts

        printf("counting       channel rack, stomping between two channels\n");
        allocation_tracking::run_allocation_test_no_allocations(
            nullptr, [&] { drive(200); }, nullptr, "channel rack, stomping between two channels");

        rack.stop();
        rack.releaseBanks();
    }

    // Teardown happens off the counted path, which is the point of the whole retirement design.
    loader.stop();
    Rations::ModelBank::destroyBank(engine.releaseBank());

    printf("\nPASSED - no allocations on the audio path\n");
    return 0;
}
